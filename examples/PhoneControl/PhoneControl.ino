// Control the Nano's LED from a serial terminal (phone or PC).
// Commands: on, off, blink, status. Sends a heartbeat every 2 seconds.

String mode = "blink";
unsigned long lastToggle = 0;
unsigned long lastHeartbeat = 0;
bool ledState = false;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  Serial.println("Nano ready. Commands: on, off, blink, status");
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    if (cmd == "on" || cmd == "off" || cmd == "blink") {
      mode = cmd;
      Serial.println("OK mode=" + mode);
    } else if (cmd == "status") {
      Serial.println("mode=" + mode + " uptime=" + String(millis() / 1000) + "s");
    } else if (cmd.length() > 0) {
      Serial.println("Unknown command: " + cmd);
    }
  }

  unsigned long now = millis();
  if (mode == "on") {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (mode == "off") {
    digitalWrite(LED_BUILTIN, LOW);
  } else if (now - lastToggle >= 500) {
    lastToggle = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
  }

  if (now - lastHeartbeat >= 2000) {
    lastHeartbeat = now;
    Serial.println("alive " + String(now / 1000) + "s mode=" + mode);
  }
}
