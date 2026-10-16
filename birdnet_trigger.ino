#define LED_PIN 4

const unsigned long BLINK_INTERVAL = 500;  // Toggle every 500 ms

bool shouldBlink = false;
bool ledState = false;

unsigned long lastToggle = 0;

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(20);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("READY");
}

void loop() {
  // Process commands from Python over USB Serial.
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command == "BLINK") {
      shouldBlink = true;
      lastToggle = millis();
      ledState = true;
      digitalWrite(LED_PIN, HIGH);
      Serial.println("BLINKING");
    }
    else if (command == "DONT_BLINK") {
      shouldBlink = false;
      ledState = false;
      digitalWrite(LED_PIN, LOW);
      Serial.println("STOPPED");
    }
  }

  // Keep blinking for as long as shouldBlink is true.
  if (shouldBlink && millis() - lastToggle >= BLINK_INTERVAL) {
    lastToggle = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
}
