#include <SPI.h>
#include <Ethernet.h>
#include <esp_system.h>
#include <stdio.h>

/*
Step-by-step usage:
1. Wire the W5500 module and the UART link to the second ESP32 using
   the pins listed below.
2. Open this sketch in the Arduino IDE and choose the correct ESP32
   board type.
3. Upload, then open the Serial Monitor at 115200 baud to watch the log.
4. Connect the Arduino sender and PC to the same network.
5. The sketch relays Modbus frames to the second ESP32 and forwards the
   replies back to the PC.
*/

// ESP32 sketch that receives Modbus frames from the Arduino sender,
// converts them into a rudimentary DNP3 format and passes them to the
// second ESP32. When DNP3 frames arrive from that board they are
// translated back to Modbus and forwarded to the PC instead of the
// original sender.
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
IPAddress pcIp(192, 168, 1, 80);    // PC address for Modbus frames

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetClient outClient;

EthernetServer server(502); // Listen for Modbus TCP
const unsigned long HEARTBEAT_INTERVAL = 5000; // 5 second LED blink
unsigned long lastBeat = 0;
int ledState = LOW;
unsigned cmdCounter = 0;
unsigned lastCmdId = 0;
uint8_t lastCmdFc = 0;

// Example Modbus commands we expect. Command 1 reads two holding
// registers starting at address 0. Command 2 reads a single input
// register at address 1.
const byte MODBUS_CMDS[][8] = {
  { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B },
  { 0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA }
};
const int NUM_CMDS = sizeof(MODBUS_CMDS) / sizeof(MODBUS_CMDS[0]);

static const char *cmdDescription(uint8_t fc) {
  switch (fc) {
    case 0x01: return "Read Coil";
    case 0x02: return "Read Discrete Input";
    case 0x03: return "Read Holding Register";
    case 0x04: return "Read Input Register";
    case 0x05: return "Write Coil";
    case 0x06: return "Write Register";
    case 0x0F: return "Write Multiple Coils";
    case 0x10: return "Write Multiple Registers";
    default:   return "Unknown";
  }
}

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

// Format millis() into HH:MM:SS for logging. Timestamps are based on the
// device's uptime so all boards can be compared without network time.
static void printTimestamp() {
  unsigned long secs = millis() / 1000;
  unsigned long h = (secs / 3600) % 24;
  unsigned long m = (secs / 60) % 60;
  unsigned long s = secs % 60;
  char ts[12];
  snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu", h, m, s);
  Serial.print("[");
  Serial.print(ts);
  Serial.print("] ");
}

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
  // Step 1: start serial output.
  Serial.begin(115200);
  // Step 2: open the UART link to the second ESP32.
  Serial1.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);
  // Step 3: show the last reset reason.
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  Serial.print(resetReasonToString(reason));
  Serial.print(" (");
  Serial.print((int)reason);
  Serial.println(")");
  // Step 4: reset and enable the W5500 module.
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  pinMode(LED_BUILTIN, OUTPUT);
  // Step 5: bring up Ethernet and reboot on failure.
  if (!startEthernet()) {
    Serial.println("Modbus ESP32 error: unable to start Ethernet");
    delay(2000);
    ESP.restart();
  }
  Serial.print("Modbus ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  // Step 6: listen for incoming Modbus TCP commands.
  server.begin();
  printTimestamp();
  Serial.println("Modbus ESP32 started");
}

// Handle traffic between the Arduino sender and the DNP3 ESP32 while
// producing diagnostic output.
void loop() {
  // Step 1: heartbeat message every few seconds.
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    printTimestamp();
    Serial.println("Modbus ESP32 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Step 2: handle Modbus TCP from the Arduino sender.
  EthernetClient client = server.available();
  if (client) {
    byte mbBuf[256];
    int mbLen = 0;
    while (client.connected() && mbLen < sizeof(mbBuf)) {
      if (client.available()) {
        mbBuf[mbLen++] = client.read();
        delay(1);
      } else {
        delay(1);
      }
    }

    cmdCounter++;
    lastCmdId = cmdCounter;
    lastCmdFc = mbBuf[1];

    printTimestamp();
    Serial.print("C");
    Serial.print(lastCmdId);
    Serial.println(": Command Received");

    printTimestamp();
    Serial.print("[Sender] - IP ");
    Serial.println(client.remoteIP());

    printTimestamp();
    Serial.print("[Sender] Command C");
    Serial.println(lastCmdId);

    Serial.print("[MODBUS] ");
    for (int i = 0; i < mbLen; i++) {
      Serial.print("0x");
      if (mbBuf[i] < 16) Serial.print("0");
      Serial.print(mbBuf[i], HEX);
      if (i < mbLen - 1) Serial.print(" ");
    }
    Serial.println();

    printTimestamp();
    Serial.print("[MODBUS] Command Meaning - ");
    Serial.println(cmdDescription(mbBuf[1]));

    printTimestamp();
    Serial.print("[MODBUS] [Sender] Send Response R");
    Serial.print(lastCmdId);
    Serial.println(" to [Sender]");
    client.write((const uint8_t*)"ACK", 3);

    Serial.println("[MODBUS] 0x41 0x43 0x4B");
    printTimestamp();
    Serial.println("[MODBUS] Response Meaning - ACK");
    client.stop();

    rxHist[rxIndex].len = mbLen;
    memcpy(rxHist[rxIndex].data, mbBuf, mbLen);
    rxIndex = (rxIndex + 1) % HIST_SIZE;

    byte dnpBuf[260];
    int outLen = modbusToDnp3(mbBuf, mbLen, dnpBuf, sizeof(dnpBuf));

    printTimestamp();
    Serial.println("[MODBUS] Translated command to DNP3");
    printTimestamp();
    Serial.print("Command C");
    Serial.print(lastCmdId);
    Serial.println(" in DNP3");

    Serial.print("[DNP3] ");
    for (int i = 0; i < outLen; i++) {
      Serial.print("0x");
      if (dnpBuf[i] < 16) Serial.print("0");
      Serial.print(dnpBuf[i], HEX);
      if (i < outLen - 1) Serial.print(" ");
    }
    Serial.println();

    printTimestamp();
    Serial.print("[DNP3] Command Meaning - ");
    if (outLen > 2) {
      Serial.println(cmdDescription(dnpBuf[2]));
    } else {
      Serial.println("Unknown");
    }

    printTimestamp();
    Serial.println("[DNP3] Send to Conversor DNP3");
    Serial1.write(dnpBuf, outLen);

    txHist[txIndex].len = outLen;
    memcpy(txHist[txIndex].data, dnpBuf, outLen);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }

  // Step 3: relay responses from the DNP3 ESP32 to the PC.
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
    int id = 0;
    if (isDnp3(inBuf, len)) {
      id = identifyCmd(inBuf + 1, len - 2);
    }
    Serial.print("R");
    Serial.print(lastCmdId);
    Serial.print(": ");
    Serial.print(cmdDescription(inBuf[1]));
    if (id) {
      Serial.print(" (example ");
      Serial.print(id);
      Serial.println(") from DNP3 ESP32");
    } else {
      Serial.println(" from DNP3 ESP32");
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
    Serial.print("Forwarding response R");
    Serial.print(lastCmdId);
    Serial.print(": ");
    Serial.print(cmdDescription(mbBuf[1]));
    if (cmdId2) {
      Serial.print(" (example ");
      Serial.print(cmdId2);
      Serial.println(") to PC");
    } else {
      Serial.println(" to PC");
    }
    Serial.print("Connecting to PC...");
    if (connectWithRetry(outClient, pcIp, 1502)) {
      Serial.println("connected");
        unsigned long tx2Start = micros();
        outClient.write(mbBuf, outLen);
        // let the PC know we handled the request
        outClient.write((const uint8_t*)"ACK", 3);
        outClient.stop();
        Serial.print("Send time us: ");
        Serial.println(micros() - tx2Start);
      // store history of transmitted messages
      txHist[txIndex].len = outLen;
      memcpy(txHist[txIndex].data, mbBuf, outLen);
      txIndex = (txIndex + 1) % HIST_SIZE;
      Serial.print("Response R");
      Serial.print(lastCmdId);
      Serial.println(" forwarded");
    } else {
      Serial.println("failed to connect");
    }
  }
  // Step 4: small delay so the watchdog remains satisfied.
  delay(1); // yield to keep watchdog happy
}
