#include <SPI.h>
#include <Ethernet.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x02 };
IPAddress ip(192, 168, 1, 70);
IPAddress pcIp(192, 168, 1, 80);

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetServer server(20000); // Listen for PC
EthernetClient outClient;

const int HEARTBEAT_PIN = LED_BUILTIN;
unsigned long lastHeartbeat = 0;

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200); // Link with Modbus ESP32
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  Ethernet.begin(mac, ip);
  Serial.print("DNP3 ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
  pinMode(HEARTBEAT_PIN, OUTPUT);
  digitalWrite(HEARTBEAT_PIN, LOW);
  delay(1000);
}

void loop() {
  // From Modbus ESP32 to PC
  if (Serial2.available()) {
    byte buf[256];
    int len = Serial2.readBytes(buf, sizeof(buf));
    Serial.print("DNP3 ESP32 received from Modbus: ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println(" -> sending to PC");
    if (outClient.connect(pcIp, 20000)) {
      Serial.println("Connected to PC");
      outClient.write(buf, len);
      outClient.stop();
    }
  }

  // From PC to Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    Serial.println("DNP3 ESP32 received from PC:");
    Serial.println("Processing data from PC");
    while (inc.connected()) {
      if (inc.available()) {
        byte b = inc.read();
        Serial.print("0x");
        if (b < 16) Serial.print("0");
        Serial.print(b, HEX);
        Serial.print(" ");
        Serial2.write(b);
      } else {
        delay(1); // keep watchdog happy
      }
    }
    Serial.println();
    inc.stop();
  }

  if (millis() - lastHeartbeat > 1000) {
    digitalWrite(HEARTBEAT_PIN, !digitalRead(HEARTBEAT_PIN));
    Serial.println("DNP3 ESP32 heartbeat");
    lastHeartbeat = millis();
  }
}
