#include <SPI.h>
#include <Ethernet.h>
#include <esp_system.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress ip(192, 168, 1, 60);
IPAddress senderIp(192, 168, 1, 50); // Arduino Uno sender address

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetClient outClient;

EthernetServer server(502); // Listen for Modbus TCP
unsigned long lastBeat = 0;
int ledState = LOW;

// Buffers to keep history of sent/received messages
#define HIST_SIZE 5
struct Msg {
  int len;
  byte data[256];
};
Msg txHist[HIST_SIZE];
Msg rxHist[HIST_SIZE];
int txIndex = 0;
int rxIndex = 0;

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
  Serial2.begin(115200); // Link to NodeMCU
  Serial.print("Reset reason: ");
  Serial.println((int)esp_reset_reason());
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  pinMode(LED_BUILTIN, OUTPUT);
  Ethernet.begin(mac, ip);
  if (Ethernet.localIP() != ip) {
    Serial.print("Modbus ESP32 error: IP mismatch ");
    Serial.println(Ethernet.localIP());
    Serial.println("Restarting to reconfigure IP...");
    delay(2000);
    ESP.restart();
  }
  Serial.print("Modbus ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
}

void loop() {
  if (millis() - lastBeat > 10000) {
    Serial.println("Modbus ESP32 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Data from Arduino Uno to NodeMCU
  EthernetClient client = server.available();
  if (client) {
    Serial.println("Connection from sender accepted");
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
      } else {
        delay(1); // yield to watchdog
      }
    }
    Serial.println();
    client.stop();

    // store received message history
    rxHist[rxIndex].len = mbLen;
    memcpy(rxHist[rxIndex].data, mbBuf, mbLen);
    rxIndex = (rxIndex + 1) % HIST_SIZE;

    byte dnpBuf[260];
    int outLen = modbusToDnp3(mbBuf, mbLen, dnpBuf, sizeof(dnpBuf));
    Serial.print("Translated to DNP3: ");
    for (int i = 0; i < outLen; i++) {
      Serial.print("0x");
      if (dnpBuf[i] < 16) Serial.print("0");
      Serial.print(dnpBuf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    Serial.println(" -> sending to NodeMCU");
    Serial2.write(dnpBuf, outLen);
    // store history of transmitted messages
    txHist[txIndex].len = outLen;
    memcpy(txHist[txIndex].data, dnpBuf, outLen);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }

  // Data from NodeMCU back to sender
  if (Serial2.available()) {
    byte inBuf[256];
    int len = Serial2.readBytes(inBuf, sizeof(inBuf));
    delay(1); // yield for watchdog
    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, inBuf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
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
    Serial.print("Translated to Modbus: ");
    for (int i = 0; i < outLen; i++) {
      Serial.print("0x");
      if (mbBuf[i] < 16) Serial.print("0");
      Serial.print(mbBuf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    Serial.println(" -> forwarding to sender");
    Serial.print("Connecting to sender...");
    if (outClient.connect(senderIp, 1502)) {
      Serial.println("connected");
      outClient.write(mbBuf, outLen);
      outClient.stop();
      // store history of transmitted messages
      txHist[txIndex].len = outLen;
      memcpy(txHist[txIndex].data, mbBuf, outLen);
      txIndex = (txIndex + 1) % HIST_SIZE;
      Serial.println("Message forwarded");
    } else {
      Serial.println("failed to connect");
    }
  }
  delay(1); // yield to keep watchdog happy
}
