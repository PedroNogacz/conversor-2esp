#include <SPI.h>
#include <Ethernet.h>

// Replace with your network settings
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 50); // Sender address
IPAddress modbusIp(192, 168, 1, 60); // First ESP32 address

EthernetServer server(1502); // For responses from Modbus ESP32

EthernetClient client;

void setup() {
  Serial.begin(115200);
  Ethernet.begin(mac, ip);
  Serial.print("Sender IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
  delay(1000);
}

unsigned long lastSend = 0;

void loop() {
  // Check for response from Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    Serial.println("Sender received response:");
    while (inc.available()) {
      byte b = inc.read();
      Serial.print("0x");
      if (b < 16) Serial.print("0");
      Serial.print(b, HEX);
      Serial.print(" ");
    }
    Serial.println();
    inc.stop();
  }

  // Periodically send frame to Modbus ESP32
  if (millis() - lastSend > 5000) {
    if (client.connect(modbusIp, 502)) { // Example Modbus TCP port
      byte frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B };
      client.write(frame, sizeof(frame));
      Serial.println("Sender transmitted Modbus frame");
      client.stop();
    }
    lastSend = millis();
  }
}
