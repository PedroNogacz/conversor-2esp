#include <SPI.h>
#include <Ethernet.h>

#include <esp_system.h>
#include <stdio.h>

// Short acknowledgement sent back to the Arduino sender and the PC
static const uint8_t ACK_BYTES[] = {'A', 'C', 'K'};
static const int ACK_LEN = sizeof(ACK_BYTES);

// Example DNP3 responses returned to the Arduino sender
static const byte DNP3_RESP_BIN_INPUT[]    = {0x05,0x80,0x81,0x01,0x02,0x00,0x01,0x01,0x16};
static const int  DNP3_RESP_BIN_INPUT_LEN  = sizeof(DNP3_RESP_BIN_INPUT);
static const byte DNP3_RESP_ANALOG_INPUT[] = {0x05,0x80,0x81,0x30,0x02,0x00,0x01,0x0A,0x00,0x16};
static const int  DNP3_RESP_ANALOG_INPUT_LEN = sizeof(DNP3_RESP_ANALOG_INPUT);
static const byte DNP3_RESP_CROB[]         = {0x05,0x81,0x00,0x16};
static const int  DNP3_RESP_CROB_LEN       = sizeof(DNP3_RESP_CROB);

/*
Step-by-step usage:
1. Wire the ESP32 to the W5500 as listed below and connect the UART link
   to the Modbus ESP32.
2. Open this sketch with the Arduino IDE and select an ESP32 board.
3. Upload the firmware and open the Serial Monitor at 115200 baud.
4. Connect the PC running the listener to the same network.
5. The sketch prints all traffic and forwards frames between the PC and
   the Modbus ESP32.
*/

// ESP32 sketch that forwards Modbus data to a PC as DNP3 frames and
// routes any PC responses back to the Modbus ESP32 over a serial link.
// Includes extra debug prints so every connection attempt as well as
// each send and receive operation is logged for analysis.
//
// Wiring summary for this board:
//   W5500  -> ESP32 SPI pins
//     MISO  - GPIO19
//     MOSI  - GPIO23
//     SCK   - GPIO18
//     CS    - GPIO5
//     RST   - GPIO16 (see W5500_RST)
//   Serial link to Modbus ESP32 using UART1
//     RX  (GPIO21) <- Modbus ESP32 TX (GPIO22)
//     TX  (GPIO22) -> Modbus ESP32 RX (GPIO21)
//   The built-in LED on GPIO2 blinks every 5 seconds as a heartbeat.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

const int LINK_TX = 22; // UART1 TX pin to Modbus ESP32
const int LINK_RX = 21; // UART1 RX pin from Modbus ESP32

// Helper that converts the ESP reset reason enum to human readable text.
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

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x03 };
IPAddress ip(192, 168, 1, 70);
IPAddress senderIp(192, 168, 1, 50); // Arduino Uno sender address
IPAddress pcIp(192, 168, 1, 80);
IPAddress modbusIp(192, 168, 1, 60); // Modbus ESP32 address

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetServer server(20000); // Listen for PC
EthernetClient outClient;
const unsigned long HEARTBEAT_INTERVAL = 5000; // 5 second LED blink
unsigned long lastBeat = 0;
int ledState = LOW;
unsigned cmdCounter = 0;
unsigned lastCmdId = 0;
uint8_t lastCmdFc = 0;

// Expected Modbus request patterns shared with the Modbus ESP32
static const byte MB_CMD_READ_HOLD[]        = {0x01,0x03,0x00,0x0A,0x00,0x02,0xE4,0x09};
static const byte MB_CMD_READ_INPUT_REG[]   = {0x01,0x04,0x00,0x0A,0x00,0x02,0x51,0xC9};
static const byte MB_CMD_WRITE_COIL[]       = {0x01,0x05,0x00,0x13,0xFF,0x00,0x7D,0xFF};
static const byte MB_CMD_READ_INPUT_STATUS[]= {0x01,0x02,0x00,0x00,0x00,0x08,0x79,0xCC};
static const byte MB_CMD_WRITE_MULTI_REGS[] = {0x01,0x10,0x00,0x01,0x00,0x02,0x04,0x00,0x0A,0x00,0x14,0x12,0x6E};

struct CmdPattern {
  const byte *data;
  int len;
};

static const CmdPattern MODBUS_CMDS[] = {
  {MB_CMD_READ_HOLD,         sizeof(MB_CMD_READ_HOLD)},
  {MB_CMD_READ_INPUT_REG,    sizeof(MB_CMD_READ_INPUT_REG)},
  {MB_CMD_WRITE_COIL,        sizeof(MB_CMD_WRITE_COIL)},
  {MB_CMD_READ_INPUT_STATUS, sizeof(MB_CMD_READ_INPUT_STATUS)},
  {MB_CMD_WRITE_MULTI_REGS,  sizeof(MB_CMD_WRITE_MULTI_REGS)}
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

static int identifyCmd(const byte *buf, int len) {
  for (int i = 0; i < NUM_CMDS; i++) {
    if (len == MODBUS_CMDS[i].len &&
        memcmp(buf, MODBUS_CMDS[i].data, MODBUS_CMDS[i].len) == 0) {
      return i + 1;
    }
  }
  return 0;
}

static bool isDnp3(const byte *buf, int len) {
  return len >= 2 && buf[0] == 0x05 && buf[len - 1] == 0x16;
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

// Read all data from *cli* until no bytes arrive for a short period.
static int readClient(EthernetClient &cli, byte *buf, int bufSize,
                      unsigned long timeoutMs = 50) {
  int len = 0;
  unsigned long last = millis();
  while (cli.connected() && len < bufSize) {
    while (cli.available() && len < bufSize) {
      buf[len++] = cli.read();
      last = millis();
    }
    if (millis() - last >= timeoutMs) {
      break;
    }
    delay(1);
  }
  return len;
}

// Format millis() into MM:SS.mmm for consistent logging. Each board
// prints timestamps based solely on its own uptime.
static void printTimestamp() {
  unsigned long ms = millis();
  unsigned long totalSecs = ms / 1000;
  unsigned long m = (totalSecs / 60) % 60;
  unsigned long s = totalSecs % 60;
  unsigned long msRem = ms % 1000;
  char ts[16];
  snprintf(ts, sizeof(ts), "%02lu:%02lu.%03lu", m, s, msRem);
  Serial.print("[");
  Serial.print(ts);
  Serial.print("] ");
}

// Attempt to bring up the Ethernet interface while keeping the watchdog fed.
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

// Connect to a peer with small yields between attempts.
static bool connectWithRetry(EthernetClient &cli, IPAddress addr, uint16_t port)
{
  for (int attempt = 0; attempt < 3; attempt++) {
    if (cli.connect(addr, port)) {
      return true;
    }
    Serial.println("connect failed, retrying");
    for (int i = 0; i < 20; i++) {
      delay(10);
      yield();
    }
  }
  return false;
}

// Configure the Ethernet interface and serial link to the Modbus ESP32.
void setup() {
  // Step 1: start debug output over USB serial.
  Serial.begin(115200);
  // Step 2: open the UART link to the Modbus ESP32.
  Serial1.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);
  // Step 3: display why the board reset.
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  Serial.print(resetReasonToString(reason));
  Serial.print(" (");
  Serial.print((int)reason);
  Serial.println(")");
  // Step 4: initialise the W5500 Ethernet module.
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  pinMode(LED_BUILTIN, OUTPUT);
  // Step 5: start Ethernet and abort on failure.
  if (!startEthernet()) {
    Serial.println("DNP3 ESP32 error: unable to start Ethernet");
    delay(2000);
    ESP.restart();
  }
  Serial.print("DNP3 ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  // Step 6: listen for TCP connections from the PC.
  server.begin();
  printTimestamp();
  Serial.println("DNP3 ESP32 started");
}

// Main loop: forwards data between the Modbus ESP32 and the PC while
// printing diagnostic timing information.
void loop() {
  // Step 1: emit a heartbeat and toggle the LED every few seconds.
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    printTimestamp();
    Serial.println("DNP3 ESP32 heartbeat");
    Serial.println();
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Step 2: forward any frames from the Modbus ESP32 to the PC.
  if (Serial1.available()) {
    unsigned long rxStart = micros();
    byte buf[256];
    int len = 0;
    while (Serial1.available() && len < (int)sizeof(buf)) {
      buf[len++] = Serial1.read();
      yield();
    }
    if (len >= 3 && buf[len-3]=='A' && buf[len-2]=='C' && buf[len-1]=='K') {
      len -= 3;
    }
    printTimestamp();
    Serial.print("[DNP3] Received from Modbus ESP32, length: ");
    Serial.println(len);
    unsigned long rxEnd = micros();
    Serial.print("Time to receive us: ");
    Serial.println(rxEnd - rxStart);
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    printTimestamp();
    Serial.print("[DNP3] ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
      delay(1); // feed watchdog during long prints
    }
    Serial.println();
    int cmdId = 0;
    if (isDnp3(buf, len)) {
      cmdId = identifyCmd(buf + 1, len - 2);
    }
    cmdCounter++;
    lastCmdId = cmdCounter;
    if (len > 2) {
      lastCmdFc = buf[2];
    }

    printTimestamp();
    Serial.print("COMMAND C");
    Serial.println(lastCmdId);

    Serial.print("[DNP3] ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      if (i < len - 1) Serial.print(" ");
    }
    Serial.println();

    printTimestamp();
    Serial.print("[DNP3] Command Meaning - ");
    if (len > 2) {
      Serial.println(cmdDescription(buf[2]));
    } else {
      Serial.println("Unknown");
    }

    printTimestamp();
    Serial.print("[DNP3] Forwarding C");
    Serial.print(lastCmdId);
    Serial.print(" - PC: IP ");
    Serial.println(pcIp);

    Serial.print("Connecting to PC...");
    if (connectWithRetry(outClient, pcIp, 20000)) {
        Serial.println("connected");
        Serial.print("Sending to PC, length: ");
        Serial.println(len);
        unsigned long txStart = micros();
        outClient.write(buf, len);
        outClient.write(ACK_BYTES, ACK_LEN);
        outClient.stop();
        Serial.print("Send time us: ");
        Serial.println(micros() - txStart);
        txHist[txIndex].len = len;
        memcpy(txHist[txIndex].data, buf, len);
        txIndex = (txIndex + 1) % HIST_SIZE;
        Serial.print("C");
        Serial.print(lastCmdId);
        Serial.println(" sent to PC");
    } else {
      Serial.println("failed to connect");
      Serial.println();
    }
  }

  // Step 3: forward any frames from the PC back to the Modbus ESP32.
  EthernetClient inc = server.available();
  if (inc) {
    byte buf[256];
    int len = readClient(inc, buf, sizeof(buf));

    cmdCounter++;
    lastCmdId = cmdCounter;
    if (len > 2) lastCmdFc = buf[2];

    printTimestamp();
    Serial.print("COMMAND C");
    Serial.println(lastCmdId);

    Serial.print("[DNP3] ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      if (i < len - 1) Serial.print(" ");
    }
    Serial.println();

    printTimestamp();
    Serial.print("[DNP3] Command Meaning - ");
    if (len > 2) {
      Serial.println(cmdDescription(buf[2]));
    } else {
      Serial.println("Unknown");
    }

    printTimestamp();
    Serial.println("[DNP3] Send Response to Sender");
    const byte *resp = DNP3_RESP_BIN_INPUT;
    int respLen = DNP3_RESP_BIN_INPUT_LEN;
    if (buf[2] == 0x01 && buf[3] == 0x30) {
      resp = DNP3_RESP_ANALOG_INPUT;
      respLen = DNP3_RESP_ANALOG_INPUT_LEN;
    } else if (buf[2] == 0x05) {
      resp = DNP3_RESP_CROB;
      respLen = DNP3_RESP_CROB_LEN;
    }
    inc.write(resp, respLen);
    Serial.print("[DNP3] ");
    for (int i = 0; i < respLen; i++) {
      Serial.print("0x");
      if (resp[i] < 16) Serial.print("0");
      Serial.print(resp[i], HEX);
      if (i < respLen - 1) Serial.print(" ");
    }
    Serial.println();
    printTimestamp();
    Serial.print("[DNP3] Response Meaning - ");
    if (respLen > 2) {
      Serial.println(cmdDescription(resp[2]));
    } else {
      Serial.println("ACK");
    }
    inc.stop();

    byte mbBuf[260];
    int modLen = dnp3ToModbus(buf, len, mbBuf, sizeof(mbBuf));

    printTimestamp();
    Serial.println("[DNP3] Translated command to Modbus");
    printTimestamp();
    Serial.print("Command C");
    Serial.print(lastCmdId);
    Serial.println(" in Modbus");

    Serial.print("[MODBUS] ");
    for (int i = 0; i < modLen; i++) {
      Serial.print("0x");
      if (mbBuf[i] < 16) Serial.print("0");
      Serial.print(mbBuf[i], HEX);
      if (i < modLen - 1) Serial.print(" ");
    }
    Serial.println();

    printTimestamp();
    Serial.print("[MODBUS] Command Meaning - ");
    if (modLen > 1) {
      Serial.println(cmdDescription(mbBuf[1]));
    } else {
      Serial.println("Unknown");
    }

    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;

    printTimestamp();
    Serial.print("[DNP3] Forwarding C");
    Serial.print(lastCmdId);
    Serial.println(" - Conversor Modbus");
    Serial1.write(buf, len);

    txHist[txIndex].len = len;
    memcpy(txHist[txIndex].data, buf, len);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }
  // Step 4: short delay so the watchdog timer keeps running.
  delay(1); // yield to watchdog
}
