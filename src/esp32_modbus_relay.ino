#include <SPI.h>
#include <Ethernet.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress ip(192, 168, 1, 60);
IPAddress senderIp(192, 168, 1, 50); // ESP8266 address

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetClient outClient;

EthernetServer server(502); // Listen for Modbus TCP

const int HEARTBEAT_PIN = LED_BUILTIN;
unsigned long lastHeartbeat = 0;

// Simple placeholder translation routines -----------------------------
int modbusToDnp3(const byte *in, int len, byte *out, int outSize) {
  if (outSize < len + 2) return 0;
  out[0] = 0x05;                // pretend DNP3 start
  memcpy(out + 1, in, len);
  out[len + 1] = 0x16;          // pretend DNP3 end
  return len + 2;
}

int dnp3ToModbus(const byte *in, int len, byte *out, int outSize) {
  if (len < 2) return 0;        // too short
  int count = len - 2;
  if (count > outSize) count = outSize;
  memcpy(out, in + 1, count);   // strip start/end bytes
  return count;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200); // Link to second ESP32
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  Ethernet.begin(mac, ip);
  Serial.print("Modbus ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
  pinMode(HEARTBEAT_PIN, OUTPUT);
  digitalWrite(HEARTBEAT_PIN, LOW);
}

void loop() {
  // Data from ESP8266 to DNP3 ESP32
  EthernetClient client = server.available();
  if (client) {
    Serial.println("Modbus ESP32 new client");
    byte mbBuf[256];
    int mbLen = 0;
    Serial.println("Modbus ESP32 received from sender:");
    while (client.connected() && mbLen < sizeof(mbBuf)) {
      if (client.available()) {
        byte b = client.read();
        Serial.print("0x");
        if (b < 16) Serial.print("0");
        Serial.print(b, HEX);
        Serial.print(" ");
        mbBuf[mbLen++] = b;
      }
    }
    Serial.println();
    client.stop();

    byte dnpBuf[260];
    int outLen = modbusToDnp3(mbBuf, mbLen, dnpBuf, sizeof(dnpBuf));
    Serial.println(" -> sending to DNP3 board");
    Serial.print("Forward length: ");
    Serial.println(outLen);
    Serial2.write(dnpBuf, outLen);
  }

  // Data from DNP3 ESP32 back to sender
  if (Serial2.available()) {
    Serial.println("Reading from DNP3 board");
    byte inBuf[256];
    int len = Serial2.readBytes(inBuf, sizeof(inBuf));
    Serial.print("Modbus ESP32 received DNP3: ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (inBuf[i] < 16) Serial.print("0");
      Serial.print(inBuf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();

    byte mbBuf[260];
    int outLen = dnp3ToModbus(inBuf, len, mbBuf, sizeof(mbBuf));
    Serial.println(" -> forwarding to sender");
    if (outClient.connect(senderIp, 1502)) {
      Serial.println("Connected to sender");
      outClient.write(mbBuf, outLen);
      outClient.stop();
    }
  }

  if (millis() - lastHeartbeat > 1000) {
    digitalWrite(HEARTBEAT_PIN, !digitalRead(HEARTBEAT_PIN));
    Serial.println("Modbus ESP32 heartbeat");
    lastHeartbeat = millis();
  }
}
