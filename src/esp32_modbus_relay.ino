#include <SPI.h>
#include <Ethernet.h>
#include <esp_system.h>

// ESP32 sketch that receives Modbus frames from the Arduino sender,
// converts them into a rudimentary DNP3 format and passes them to the
// second ESP32. Responses travel in the reverse direction.
// Additional debug prints log every connection attempt and when frames
// are sent or received as requested for troubleshooting.
//
// Wiring summary for this board:
//   W5500  -> ESP32 SPI pins
//     MISO  - GPIO19
//     MOSI  - GPIO23
//     SCK   - GPIO18
//     CS    - GPIO5
//     RST   - GPIO16 (see W5500_RST)
//   Serial link to second ESP32 using UART1
//     TX  (GPIO22) -> second ESP32 RX (GPIO21)
//     RX  (GPIO21) <- second ESP32 TX (GPIO22)
//   The built-in LED on GPIO2 blinks every 5 seconds as a heartbeat.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

const int LINK_TX = 22; // UART1 TX pin to DNP3 ESP32
const int LINK_RX = 21; // UART1 RX pin from DNP3 ESP32

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

// Example Modbus commands we expect. Command 1 reads two holding
// registers starting at address 0. Command 2 reads a single input
// register at address 1.
const byte MODBUS_CMDS[][8] = {
  { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B },
  { 0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA }
};
const int NUM_CMDS = sizeof(MODBUS_CMDS) / sizeof(MODBUS_CMDS[0]);

// Return the command index (1-based) if *buf* matches one of the example
// Modbus requests. Returns 0 when no match is found.
static int identifyCmd(const byte *buf, int len) {
  for (int i = 0; i < NUM_CMDS; i++) {
    if (len == 8 && memcmp(buf, MODBUS_CMDS[i], 8) == 0) {
      return i + 1;
    }
  }
  return 0;
}

// Check for the minimal DNP3 framing used in these examples.
static bool isDnp3(const byte *buf, int len) {
  return len >= 2 && buf[0] == 0x05 && buf[len - 1] == 0x16;
}

// Attempt to start the Ethernet interface with a few retries. This guards
// against the W5500 not responding on the first try which can otherwise hang
// the sketch inside Ethernet.begin().
static bool startEthernet()
{
  Serial.println("Starting Ethernet");
  SPI.begin(18, 19, 23, 5); // explicit SPI pins for W5500
  Ethernet.init(5);          // chip select pin
  for (int attempt = 0; attempt < 3; attempt++) {
    Ethernet.begin(mac, ip);
    delay(100);
    if (Ethernet.hardwareStatus() != EthernetNoHardware &&
        Ethernet.localIP() == ip) {
      Serial.println("Ethernet ready");
      return true;
    }
    Serial.println("Ethernet failed, resetting W5500...");
    digitalWrite(W5500_RST, LOW);
    delay(50);
    digitalWrite(W5500_RST, HIGH);
    for (int i = 0; i < 20; i++) { // ~200 ms delay while yielding
      delay(10);
      yield();
    }
  }
  return false;
}

// Connect with small delays so the watchdog stays fed if the remote side
// is unreachable. Returns true when the connection succeeds.
static bool connectWithRetry(EthernetClient &cli, IPAddress addr, uint16_t port)
{
  for (int attempt = 0; attempt < 3; attempt++) {
    if (cli.connect(addr, port)) {
      return true;
    }
    Serial.println("connect failed, retrying");
    for (int i = 0; i < 20; i++) { // ~200 ms between attempts
      delay(10);
      yield();
    }
  }
  return false;
}

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
  Serial1.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX); // Link to DNP3 ESP32
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
  if (!startEthernet()) {
    Serial.println("Modbus ESP32 error: unable to start Ethernet");
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
        delay(1); // avoid watchdog reset while printing
      } else {
        delay(1); // yield to watchdog
      }
    }
    unsigned long rxEnd = micros();
    Serial.print("Time to receive us: ");
    Serial.println(rxEnd - rxStart);
    Serial.println();
    int cmd = identifyCmd(mbBuf, mbLen);
    if (cmd) {
      Serial.print("Identified Modbus command ");
      Serial.println(cmd);
    } else {
      Serial.println("Unknown Modbus command");
    }
    // Respond to the sender so the Arduino knows the frame was handled.
    client.write((const uint8_t*)"ACK", 3);
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
      delay(1); // feed watchdog during long prints
    }
    Serial.println();
    int cmdId = 0;
    if (isDnp3(dnpBuf, outLen)) {
      cmdId = identifyCmd(dnpBuf + 1, outLen - 2);
      Serial.print("Valid DNP3 payload command ");
      Serial.println(cmdId ? cmdId : 0);
    } else {
      Serial.println("Invalid DNP3 frame");
    }
    Serial.println(" -> sending to DNP3 ESP32");
    Serial.print("Forwarding command ");
    Serial.print(cmdId);
    Serial.println(" to DNP3 ESP32");
    Serial.print("Sending to DNP3 ESP32, length: ");
    Serial.println(outLen);
    unsigned long txStart = micros();
    Serial1.write(dnpBuf, outLen);
    Serial1.write((const uint8_t*)"ACK", 3);
    Serial.print("Send time us: ");
    Serial.println(micros() - txStart);
    // store history of transmitted messages
    txHist[txIndex].len = outLen;
    memcpy(txHist[txIndex].data, dnpBuf, outLen);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }

  // Data from DNP3 ESP32 back to sender
  if (Serial1.available()) {
    byte inBuf[256];
    int len = 0;
    while (Serial1.available() && len < (int)sizeof(inBuf)) {
      inBuf[len++] = Serial1.read();
      yield();
    }
    Serial.print("Received from DNP3 ESP32, length: ");
    Serial.println(len);
    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, inBuf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    Serial.print("Modbus ESP32 received DNP3: ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (inBuf[i] < 16) Serial.print("0");
      Serial.print(inBuf[i], HEX);
      Serial.print(" ");
      delay(1); // feed watchdog during prints
    }
    Serial.println();
    if (isDnp3(inBuf, len)) {
      int id = identifyCmd(inBuf + 1, len - 2);
      Serial.print("DNP3 frame with command ");
      Serial.println(id ? id : 0);
    } else {
      Serial.println("Invalid DNP3 frame");
    }

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
      delay(1); // keep watchdog alive during print
    }
    Serial.println();
    int cmdId2 = identifyCmd(mbBuf, outLen);
    Serial.print("Identified Modbus command ");
    Serial.println(cmdId2 ? cmdId2 : 0);
    Serial.println(" -> forwarding to sender");
    Serial.print("Forwarding command ");
    Serial.print(cmdId2);
    Serial.println(" to sender");
    Serial.print("Connecting to sender...");
    if (connectWithRetry(outClient, senderIp, 1502)) {
      Serial.println("connected");
        unsigned long tx2Start = micros();
        outClient.write(mbBuf, outLen);
        // let the sender know we handled the request
        outClient.write((const uint8_t*)"ACK", 3);
        outClient.stop();
        Serial.print("Send time us: ");
        Serial.println(micros() - tx2Start);
      // store history of transmitted messages
      txHist[txIndex].len = outLen;
      memcpy(txHist[txIndex].data, mbBuf, outLen);
      txIndex = (txIndex + 1) % HIST_SIZE;
      Serial.println("Message forwarded");
      Serial1.write((const uint8_t*)"ACK", 3);
    } else {
      Serial.println("failed to connect");
    }
  }
  delay(1); // yield to keep watchdog happy
}
