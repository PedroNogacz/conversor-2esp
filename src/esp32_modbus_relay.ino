#include <SPI.h>
#include <Ethernet.h>
#include <esp_system.h>

// ESP32 sketch that receives Modbus frames from the Arduino sender,
// converts them into a rudimentary DNP3 format and passes them to the
// second ESP32. Responses travel in the reverse direction.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// Helper to translate the ESP reset reason code into text for logging.
static const char *resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress ip(192, 168, 1, 60);
IPAddress senderIp(192, 168, 1, 50); // Arduino Uno sender address

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetClient outClient;

EthernetServer server(502); // Listen for Modbus TCP
const unsigned long HEARTBEAT_INTERVAL = 5000; // 5 second LED blink
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

// Wrap a Modbus frame with dummy DNP3 start/end bytes.
int modbusToDnp3(const byte *in, int len, byte *out, int outSize) {
  if (outSize < len + 2) return 0;
  out[0] = 0x05;                // pretend DNP3 start
  memcpy(out + 1, in, len);
  out[len + 1] = 0x16;          // pretend DNP3 end
  return len + 2;
}

// Remove the fake DNP3 bytes and recover the original Modbus frame.
int dnp3ToModbus(const byte *in, int len, byte *out, int outSize) {
  if (len < 2) return 0;        // too short
  int count = len - 2;
  if (count > outSize) count = outSize;
  memcpy(out, in + 1, count);   // strip start/end bytes
  return count;
}

// Initialize Ethernet, serial ports and heartbeat timer.
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200); // Link to DNP3 ESP32
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  Serial.print(resetReasonToString(reason));
  Serial.print(" (");
  Serial.print((int)reason);
  Serial.println(")");
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

// Handle traffic between the Arduino sender and the DNP3 ESP32 while
// producing diagnostic output.
void loop() {
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    Serial.println("Modbus ESP32 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Data from Arduino Uno to DNP3 ESP32
  EthernetClient client = server.available();
  if (client) {
    Serial.println("Connection from sender accepted");
    unsigned long rxStart = micros();
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

        yield(); // avoid watchdog reset while printing

      } else {
        yield(); // yield to watchdog
      }
    }
    unsigned long rxEnd = micros();
    Serial.print("Time to receive us: ");
    Serial.println(rxEnd - rxStart);
    Serial.println();
    client.stop();

    // store received message history
    rxHist[rxIndex].len = mbLen;
    memcpy(rxHist[rxIndex].data, mbBuf, mbLen);
    rxIndex = (rxIndex + 1) % HIST_SIZE;

    byte dnpBuf[260];
    unsigned long transStart = micros();
    int outLen = modbusToDnp3(mbBuf, mbLen, dnpBuf, sizeof(dnpBuf));
    unsigned long transEnd = micros();
    Serial.print("Translation us: ");
    Serial.println(transEnd - transStart);
    Serial.print("Translated to DNP3: ");
    for (int i = 0; i < outLen; i++) {
      Serial.print("0x");
      if (dnpBuf[i] < 16) Serial.print("0");
      Serial.print(dnpBuf[i], HEX);
      Serial.print(" ");

      yield(); // feed watchdog during long prints

    }
    Serial.println();
    Serial.println(" -> sending to DNP3 ESP32");
    unsigned long txStart = micros();
    Serial2.write(dnpBuf, outLen);
    Serial.print("Send time us: ");
    Serial.println(micros() - txStart);
    // store history of transmitted messages
    txHist[txIndex].len = outLen;
    memcpy(txHist[txIndex].data, dnpBuf, outLen);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }

  // Data from DNP3 ESP32 back to sender
  if (Serial2.available()) {
    byte inBuf[256];
    int len = 0;
    while (Serial2.available() && len < (int)sizeof(inBuf)) {
      inBuf[len++] = Serial2.read();
      yield();
    }
    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, inBuf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    Serial.print("Modbus ESP32 received DNP3: ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (inBuf[i] < 16) Serial.print("0");
      Serial.print(inBuf[i], HEX);
      Serial.print(" ");

      yield(); // feed watchdog during prints

    }
    Serial.println();

    byte mbBuf[260];
    unsigned long trans2Start = micros();
    int outLen = dnp3ToModbus(inBuf, len, mbBuf, sizeof(mbBuf));
    unsigned long trans2End = micros();
    Serial.print("Translation us: ");
    Serial.println(trans2End - trans2Start);
    Serial.print("Translated to Modbus: ");
    for (int i = 0; i < outLen; i++) {
      Serial.print("0x");
      if (mbBuf[i] < 16) Serial.print("0");
      Serial.print(mbBuf[i], HEX);
      Serial.print(" ");

      yield(); // keep watchdog alive during print

    }
    Serial.println();
    Serial.println(" -> forwarding to sender");
    Serial.print("Connecting to sender...");
    if (outClient.connect(senderIp, 1502)) {
      Serial.println("connected");
        unsigned long tx2Start = micros();
        outClient.write(mbBuf, outLen);
        outClient.stop();
        Serial.print("Send time us: ");
        Serial.println(micros() - tx2Start);
      // store history of transmitted messages
      txHist[txIndex].len = outLen;
      memcpy(txHist[txIndex].data, mbBuf, outLen);
      txIndex = (txIndex + 1) % HIST_SIZE;
      Serial.println("Message forwarded");
    } else {
      Serial.println("failed to connect");
    }
  }
  yield(); // yield to keep watchdog happy
}
