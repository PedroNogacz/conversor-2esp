#include <SPI.h>
#include <Ethernet.h>

#include <esp_system.h>
#include <stdio.h>

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

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetServer server(20000); // Listen for PC
EthernetClient outClient;
const unsigned long HEARTBEAT_INTERVAL = 5000; // 5 second LED blink
unsigned long lastBeat = 0;
int ledState = LOW;
unsigned cmdCounter = 0;
unsigned lastCmdId = 0;
uint8_t lastCmdFc = 0;

// Expected Modbus request patterns, shared with the Modbus ESP32.
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

static int identifyCmd(const byte *buf, int len) {
  for (int i = 0; i < NUM_CMDS; i++) {
    if (len == 8 && memcmp(buf, MODBUS_CMDS[i], 8) == 0) {
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

// Format millis() into HH:MM:SS for consistent logging. Each board
// prints timestamps based solely on its own uptime.
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
    Serial.print("Received from Modbus ESP32, length: ");
    Serial.println(len);
    unsigned long rxEnd = micros();
    Serial.print("Time to receive us: ");
    Serial.println(rxEnd - rxStart);
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    printTimestamp();
    Serial.print("DNP3 ESP32 received from Modbus: ");
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
    lastCmdFc = buf[1];
    Serial.print("C");
    Serial.print(lastCmdId);
    Serial.print(": forwarding ");
    Serial.print(cmdDescription(buf[1]));
    if (cmdId) {
      Serial.print(" (example ");
      Serial.print(cmdId);
      Serial.println(") to PC");
    } else {
      Serial.println(" to PC");
    }
    Serial.println("DNP3 ESP32 notifying: attempting to connect to PC");
    printTimestamp();
    Serial.print("Connecting to PC...");
    if (connectWithRetry(outClient, pcIp, 20000)) {
        Serial.println("connected");
        Serial.print("Sending to PC, length: ");
        Serial.println(len);
        unsigned long txStart = micros();
        outClient.write(buf, len);
        outClient.write((const uint8_t*)"ACK", 3);
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
    }
  }

  // Step 3: forward any frames from the PC back to the Modbus ESP32.
  EthernetClient inc = server.available();
  if (inc) {
    byte buf[256];
    int len = 0;
    while (inc.connected() && len < sizeof(buf)) {
      if (inc.available()) {
        buf[len++] = inc.read();
        delay(1);
      } else {
        delay(1);
      }
    }

    cmdCounter++;
    lastCmdId = cmdCounter;
    if (len > 1) lastCmdFc = buf[1];

    printTimestamp();
    Serial.print("C");
    Serial.print(lastCmdId);
    Serial.println(": Command Received");

    printTimestamp();
    Serial.print("[Sender] - IP ");
    Serial.println(inc.remoteIP());

    printTimestamp();
    Serial.print("[Sender] Command C");
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
    Serial.println(cmdDescription(buf[1]));

    printTimestamp();
    Serial.print("[Sender] Send Response R");
    Serial.println(lastCmdId);
    inc.write((const uint8_t*)"ACK", 3);
    Serial.println("[DNP3] 0x41 0x43 0x4B");
    printTimestamp();
    Serial.println("[DNP3] Response Meaning - ACK");
    inc.stop();

    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;

    printTimestamp();
    Serial.println("[DNP3] Send to Conversor Modbus");
    Serial1.write(buf, len);

    txHist[txIndex].len = len;
    memcpy(txHist[txIndex].data, buf, len);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }
  // Step 4: short delay so the watchdog timer keeps running.
  delay(1); // yield to watchdog
}
