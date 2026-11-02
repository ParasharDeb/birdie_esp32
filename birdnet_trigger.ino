// ESP32: bird LED + direct BirdNET link over WiFi.
//
// Same LED behaviour as ../birdnet_trigger/birdnet_trigger.ino (pin 4,
// 500 ms blink, READY / BLINKING / STOPPED replies), plus the BirdNET request.
//
// USB serial (from yolo_bird/bird_detect.py):
//   BLINK       -> start blinking, pick a random clip number and send it to
//                  the BirdNET server: GET http://<laptop>:5000/analyze?n=<n>
//   DONT_BLINK  -> stop blinking, LED off
//
// The BirdNET reply (species + confidence) is kept in lastSpecies /
// lastConfidence and printed on Serial as:
//   BIRDNET:<n>:<common name>:<confidence>    or    BIRDNET:<n>:NONE
//
// Fill in WIFI_SSID / WIFI_PASSWORD. BIRDNET_HOST is the laptop's WiFi IP.

#include <WiFi.h>
#include <HTTPClient.h>

const char* WIFI_SSID     = "Parashar";
const char* WIFI_PASSWORD = "getyourownwifi";
const char* BIRDNET_HOST  = "10.86.178.50";
const int   BIRDNET_PORT  = 5000;

const int LED_PIN = 4;
const unsigned long BLINK_MS = 500;  // Toggle every 500 ms
const int MAX_CLIPS = 64;

int clips[MAX_CLIPS];
int clipCount = 0;

volatile bool blinking = false;
volatile bool birdnetBusy = false;
String lastSpecies = "";
float lastConfidence = 0.0;

bool ledOn = false;
unsigned long lastToggle = 0;
String serialLine = "";

String birdnetUrl(const String& path) {
  return String("http://") + BIRDNET_HOST + ":" + BIRDNET_PORT + path;
}

// Extract the value of "key" from a flat JSON object (string or number).
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
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WIFI:" + WiFi.localIP().toString());
  } else {
    Serial.println("WIFI:FAILED");
  }
}

// Ask the server which clip numbers exist; fall back to 1..10.
void loadClips() {
  clipCount = 0;
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(birdnetUrl("/clips"));
    http.setTimeout(5000);
    if (http.GET() == 200) {
      String body = http.getString();
      int value = -1;
      for (int i = 0; i < (int)body.length() && clipCount < MAX_CLIPS; i++) {
        char c = body[i];
        if (isDigit(c)) {
          value = (value < 0 ? 0 : value * 10) + (c - '0');
        } else if (value >= 0) {
          clips[clipCount++] = value;
          value = -1;
        }
      }
    }
    http.end();
  }
  if (clipCount == 0) {
    for (int i = 1; i <= 10; i++) clips[clipCount++] = i;
    Serial.println("CLIPS:DEFAULT 1-10");
  } else {
    Serial.println("CLIPS:" + String(clipCount));
  }
}

// Runs as its own task so the LED keeps blinking during the ~7 s analysis.
void birdnetTask(void* param) {
  int n = (int)(intptr_t)param;
  connectWifi();

  String line = "BIRDNET:" + String(n) + ":";
  if (WiFi.status() != WL_CONNECTED) {
    line += "ERROR:no wifi";
  } else {
    HTTPClient http;
    http.begin(birdnetUrl("/analyze?n=" + String(n)));
    http.setTimeout(60000);  // Analysis takes ~2-10 s; leave plenty of margin.
    int code = http.GET();
    String body = code > 0 ? http.getString() : "";
    http.end();

    String species = jsonValue(body, "species");
    if (code == 200 && species.length() > 0 && species != "null") {
      lastSpecies = species;
      lastConfidence = jsonValue(body, "confidence").toFloat();
      line += species + ":" + String(lastConfidence, 2);
    } else if (code == 200) {
      lastSpecies = "";
      lastConfidence = 0.0;
      line += "NONE";
    } else {
      line += "ERROR:http " + String(code);
    }
  }
  Serial.println(line);

  birdnetBusy = false;
  vTaskDelete(NULL);
}

void askBirdnet() {
  if (birdnetBusy) return;  // One request at a time.
  birdnetBusy = true;
  int n = clips[random(clipCount)];
  Serial.println("CLIP:" + String(n));
  xTaskCreate(birdnetTask, "birdnet", 8192, (void*)(intptr_t)n, 1, NULL);
}

void handleCommand(const String& cmd) {
  if (cmd == "BLINK") {
    blinking = true;
    lastToggle = millis();
    ledOn = true;
    digitalWrite(LED_PIN, HIGH);
    Serial.println("BLINKING");
    askBirdnet();
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
  randomSeed(esp_random());
  connectWifi();
  loadClips();
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
