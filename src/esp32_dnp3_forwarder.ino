#include <SPI.h>
#include <Ethernet.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x02 };
IPAddress ip(192, 168, 1, 70);
IPAddress pcIp(192, 168, 1, 80);

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetServer server(20000); // Listen for PC
EthernetClient outClient;
unsigned long lastBeat = 0;
int ledState = LOW;

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200); // Link with Modbus ESP32
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  pinMode(LED_BUILTIN, OUTPUT);
  Ethernet.begin(mac, ip);
  if (Ethernet.localIP() != ip) {
    Serial.print("DNP3 ESP32 warning: IP mismatch ");
    Serial.println(Ethernet.localIP());
    Ethernet.begin(mac, ip);
  }
  Serial.print("DNP3 ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
  delay(1000);
}

void loop() {
  if (millis() - lastBeat > 10000) {
    Serial.println("DNP3 ESP32 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
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
      outClient.write(buf, len);
      outClient.stop();
    }
  }

  // From PC to Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    Serial.println("DNP3 ESP32 received from PC:");
    while (inc.connected()) {
      if (inc.available()) {
        byte b = inc.read();
        Serial.print("0x");
        if (b < 16) Serial.print("0");
        Serial.print(b, HEX);
        Serial.print(" ");
        Serial2.write(b);
      }
    }
    Serial.println();
    inc.stop();
  }
}
