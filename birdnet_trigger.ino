#include <WiFi.h>
#include <driver/i2s.h>

// ---------- WiFi / server ----------
const char* WIFI_SSID     = "Parashar";
const char* WIFI_PASSWORD = "getyourownwifi";
const char* SERVER_HOST   = "10.86.178.50";   // laptop's WiFi IP
const int   SERVER_PORT   = 5000;

// ---------- Pins ----------
const int LED_PIN     = 4;
const int MIC_PWR_PIN = 32;   // -> 10k -> BJT base
const int I2S_SCK     = 26;
const int I2S_WS      = 25;
const int I2S_SD      = 33;
const i2s_port_t I2S_PORT = I2S_NUM_0;

// ---------- Audio ----------
const uint32_t SAMPLE_RATE    = 48000;
const uint32_t RECORD_SECONDS = 3;
const uint32_t WARMUP_MS      = 250;   // mic start-up noise, thrown away
const int      GAIN_SHIFT     = 14;    // 16 = no gain, 14 = 4x louder
const uint32_t LOG_EVERY_MS   = 500;   // print a MIC level line this often while recording
const int32_t  SILENT_PEAK    = 20;    // peak below this = mic is giving nothing
const i2s_channel_fmt_t MIC_CHANNEL = I2S_CHANNEL_FMT_ONLY_LEFT;  // try ONLY_RIGHT if LEVEL is always 0

// ---------- Behaviour ----------
const unsigned long BLINK_MS    = 500;
const unsigned long COOLDOWN_MS = 15000;  // ignore new BLINKs for 15 s after a recording

volatile bool busy = false;
volatile unsigned long lastRecordEnd = 0;
int recCount = 0;
bool i2sInstalled = false;

bool blinking = false;
bool ledOn = false;
unsigned long lastToggle = 0;
String serialLine = "";

static int32_t i2sBuf[1024];
static int16_t pcmBuf[1024];

// ================= Helpers =================

String jsonValue(const String& json, const String& key) {
  int k = json.indexOf("\"" + key + "\"");
  if (k < 0) return "";
  int colon = json.indexOf(':', k);
  if (colon < 0) return "";
  int start = colon + 1;
  while (start < (int)json.length() && json[start] == ' ') start++;
  if (json[start] == '"') {
    int end = json.indexOf('"', start + 1);
    return json.substring(start + 1, end);
  }
  int end = start;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}') end++;
  return json.substring(start, end);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // modem sleep stalls the upload long enough to drop I2S audio
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(250);
  if (WiFi.status() == WL_CONNECTED) Serial.println("WIFI:" + WiFi.localIP().toString());
  else Serial.println("WIFI:FAILED");
}

void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

void makeWavHeader(uint8_t* h, uint32_t dataBytes) {
  memcpy(h, "RIFF", 4);      put32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); put32(h + 16, 16);
  put16(h + 20, 1);                 // PCM
  put16(h + 22, 1);                 // mono
  put32(h + 24, SAMPLE_RATE);
  put32(h + 28, SAMPLE_RATE * 2);   // byte rate
  put16(h + 32, 2);                 // block align
  put16(h + 34, 16);                // bits per sample
  memcpy(h + 36, "data", 4); put32(h + 40, dataBytes);
}

// ================= Mic power + I2S =================

void micOff() {
  if (i2sInstalled) {
    i2s_driver_uninstall(I2S_PORT);
    i2sInstalled = false;
  }
  // Release the signal pins BEFORE cutting the mic's ground,
  // so the mic can't be back-powered through SCK/WS.
  pinMode(I2S_SCK, INPUT);
  pinMode(I2S_WS, INPUT);
  pinMode(I2S_SD, INPUT);
  digitalWrite(MIC_PWR_PIN, LOW);
}

bool micOn() {
  digitalWrite(MIC_PWR_PIN, HIGH);   // BJT on -> mic powered
  delay(50);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = MIC_CHANNEL;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 1024;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) { micOff(); return false; }
  i2sInstalled = true;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_SCK;
  pins.ws_io_num    = I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = I2S_SD;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) { micOff(); return false; }

  i2s_zero_dma_buffer(I2S_PORT);

  // Throw away the start-up pop.
  uint32_t toDiscard = SAMPLE_RATE * WARMUP_MS / 1000;
  while (toDiscard > 0) {
    size_t br = 0;
    i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &br, portMAX_DELAY);
    uint32_t got = br / 4;
    toDiscard -= (got < toDiscard) ? got : toDiscard;
  }
  return true;
}

// ================= HTTP =================

int readResponse(WiFiClient& client, String& body) {
  unsigned long start = millis();
  while (client.connected() && !client.available()) {
    if (millis() - start > 10000) return -5;   // server only saves the file, should be quick
    delay(10);
  }
  String status = client.readStringUntil('\n');      // "HTTP/1.1 200 OK"
  int code = status.length() >= 12 ? status.substring(9, 12).toInt() : -6;

  while (client.connected() || client.available()) {  // skip headers
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }
  start = millis();
  while ((client.connected() || client.available()) && millis() - start < 5000) {
    while (client.available()) body += (char)client.read();
    delay(5);
  }
  return code;
}

// Records audio and streams it straight to the server (never all in RAM).
// Returns HTTP status, or a negative number on failure (err says why).
int recordAndSend(int n, String& body, String& err) {
  WiFiClient client;
  if (!client.connect(SERVER_HOST, SERVER_PORT)) { err = "server unreachable"; return -1; }

  Serial.println("MIC:" + String(n) + ":ON");
  if (!micOn()) { err = "i2s start failed"; client.stop(); return -2; }

  const uint32_t dataBytes = RECORD_SECONDS * SAMPLE_RATE * 2;
  client.printf("POST /upload?n=%d HTTP/1.1\r\n", n);
  client.printf("Host: %s:%d\r\n", SERVER_HOST, SERVER_PORT);
  client.print("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)(dataBytes + 44));
  client.print("Connection: close\r\n\r\n");

  uint8_t header[44];
  makeWavHeader(header, dataBytes);
  client.write(header, 44);

  uint32_t sent = 0;
  int32_t peak = 0;
  // Level of the current LOG_EVERY_MS window, so you can see the mic react live.
  const uint32_t windowSamples = SAMPLE_RATE * LOG_EVERY_MS / 1000;
  uint32_t winCount = 0;
  int32_t winPeak = 0;
  double winSumSq = 0;
  unsigned long recStart = millis();
  while (sent < dataBytes) {
    size_t br = 0;
    i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &br, portMAX_DELAY);
    uint32_t samples = br / 4;
    uint32_t remaining = (dataBytes - sent) / 2;
    if (samples > remaining) samples = remaining;

    for (uint32_t i = 0; i < samples; i++) {
      int32_t s = i2sBuf[i] >> GAIN_SHIFT;
      if (s > 32767) s = 32767;
      else if (s < -32768) s = -32768;
      pcmBuf[i] = (int16_t)s;
      if (abs(s) > peak) peak = abs(s);
      if (abs(s) > winPeak) winPeak = abs(s);
      winSumSq += (double)s * s;
      if (++winCount >= windowSamples) {
        int rms = (int)sqrt(winSumSq / winCount);
        Serial.printf("MIC:%d:%lums peak=%ld rms=%d%s\n", n, millis() - recStart,
                      (long)winPeak, rms, winPeak < SILENT_PEAK ? " SILENT" : "");
        winCount = 0; winPeak = 0; winSumSq = 0;
      }
    }
    size_t want = samples * 2;
    if (client.write((uint8_t*)pcmBuf, want) != want) {
      micOff(); client.stop();
      err = "upload failed"; return -3;
    }
    sent += want;
  }

  micOff();   // done recording
  Serial.println("MIC:" + String(n) + ":OFF");
  Serial.println("LEVEL:" + String(n) + ":" + String(peak));
  if (peak < SILENT_PEAK) Serial.println("MIC:" + String(n) + ":WARNING no sound - check wiring / MIC_CHANNEL");
  Serial.println("SENT:" + String(n) + ":" + String(sent + 44) + " bytes");

  int code = readResponse(client, body);
  client.stop();
  if (code < 0) err = "no reply from server";
  return code;
}

// ================= Recording task =================

void recordTask(void* param) {
  int n = (int)(intptr_t)param;
  connectWifi();

  // The server only queues the file; BirdNET runs later on the laptop.
  String line = "QUEUED:" + String(n) + ":";
  if (WiFi.status() != WL_CONNECTED) {
    line += "ERROR:no wifi";
  } else {
    String body, err;
    int code = recordAndSend(n, body, err);

    if (code == 200) {
      line += jsonValue(body, "queued");
    } else if (code < 0) {
      line += "ERROR:" + err;
    } else {
      line += "ERROR:http " + String(code);
    }
  }
  Serial.println(line);

  lastRecordEnd = millis();
  busy = false;
  vTaskDelete(NULL);
}

void startRecording() {
  if (busy) return;
  if (lastRecordEnd != 0 && millis() - lastRecordEnd < COOLDOWN_MS) return;
  busy = true;
  recCount++;
  Serial.println("REC:" + String(recCount));
  xTaskCreate(recordTask, "record", 8192, (void*)(intptr_t)recCount, 1, NULL);
}

// ================= Serial commands =================

void handleCommand(const String& cmd) {
  if (cmd == "BLINK") {
    blinking = true;
    lastToggle = millis();
    ledOn = true;
    digitalWrite(LED_PIN, HIGH);
    Serial.println("BLINKING");
    startRecording();
  } else if (cmd == "DONT_BLINK") {
    blinking = false;
    ledOn = false;
    digitalWrite(LED_PIN, LOW);
    Serial.println("STOPPED");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(MIC_PWR_PIN, OUTPUT);
  micOff();               // mic starts unpowered, I2S pins released
  connectWifi();
  Serial.println("READY");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialLine.trim();
      if (serialLine.length() > 0) handleCommand(serialLine);
      serialLine = "";
    } else {
      serialLine += c;
    }
  }

  if (blinking && millis() - lastToggle >= BLINK_MS) {
    lastToggle = millis();
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
  }
}