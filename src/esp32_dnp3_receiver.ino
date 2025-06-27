#include <SPI.h>
#include <Ethernet.h>
#include <esp_system.h>

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
//   Serial link to Modbus ESP32 using UART0
//     RX  (GPIO3)  <- Modbus ESP32 TX (GPIO1)
//     TX  (GPIO1)  -> Modbus ESP32 RX (GPIO3)
//   The built-in LED on GPIO2 blinks every 5 seconds as a heartbeat.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

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
IPAddress pcIp(192, 168, 1, 80);

const int W5500_RST = 16; // GPIO used to reset the Ethernet module

EthernetServer server(20000); // Listen for PC
EthernetClient outClient;
const unsigned long HEARTBEAT_INTERVAL = 5000; // 5 second LED blink
unsigned long lastBeat = 0;
int ledState = LOW;

// Expected Modbus request patterns, shared with the Modbus ESP32.
const byte MODBUS_CMDS[][8] = {
  { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B },
  { 0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA }
};
const int NUM_CMDS = sizeof(MODBUS_CMDS) / sizeof(MODBUS_CMDS[0]);

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

// Attempt to bring up the Ethernet interface while keeping the watchdog fed.
static bool startEthernet()
{
  Serial2.println("Starting Ethernet");
  SPI.begin(18, 19, 23, 5); // explicit SPI pins for W5500
  Ethernet.init(5);          // chip select pin
  for (int attempt = 0; attempt < 3; attempt++) {
    Ethernet.begin(mac, ip);
    delay(100);
    if (Ethernet.hardwareStatus() != EthernetNoHardware &&
        Ethernet.localIP() == ip) {
      Serial2.println("Ethernet ready");
      return true;
    }
    Serial2.println("Ethernet failed, resetting W5500...");
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
    Serial2.println("connect failed, retrying");
    for (int i = 0; i < 20; i++) {
      delay(10);
      yield();
    }
  }
  return false;
}

// Configure the Ethernet interface and serial link to the Modbus ESP32.
void setup() {
  Serial2.begin(115200);
  Serial.begin(115200); // Link to Modbus ESP32
  esp_reset_reason_t reason = esp_reset_reason();
  Serial2.print("Reset reason: ");
  Serial2.print(resetReasonToString(reason));
  Serial2.print(" (");
  Serial2.print((int)reason);
  Serial2.println(")");
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  pinMode(LED_BUILTIN, OUTPUT);
  if (!startEthernet()) {
    Serial2.println("DNP3 ESP32 error: unable to start Ethernet");
    delay(2000);
    ESP.restart();
  }
  Serial2.print("DNP3 ESP32 IP: ");
  Serial2.println(Ethernet.localIP());
  server.begin();
}

// Main loop: forwards data between the Modbus ESP32 and the PC while
// printing diagnostic timing information.
void loop() {
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    Serial2.println("DNP3 ESP32 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // From Modbus ESP32 to PC
  if (Serial.available()) {
    unsigned long rxStart = micros();
    byte buf[256];
    int len = 0;
    while (Serial.available() && len < (int)sizeof(buf)) {
      buf[len++] = Serial.read();
      yield();
    }
    Serial2.print("Received from Modbus ESP32, length: ");
    Serial2.println(len);
    unsigned long rxEnd = micros();
    Serial2.print("Time to receive us: ");
    Serial2.println(rxEnd - rxStart);
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    Serial2.print("DNP3 ESP32 received from Modbus: ");
    for (int i = 0; i < len; i++) {
      Serial2.print("0x");
      if (buf[i] < 16) Serial2.print("0");
      Serial2.print(buf[i], HEX);
      Serial2.print(" ");
      delay(1); // feed watchdog during long prints
    }
    Serial2.println();
    int cmdId = 0;
    if (isDnp3(buf, len)) {
      cmdId = identifyCmd(buf + 1, len - 2);
      Serial2.print("Valid DNP3 payload command ");
      Serial2.println(cmdId ? cmdId : 0);
    } else {
      Serial2.println("Invalid DNP3 frame");
    }
    Serial2.println(" -> sending to PC");
    Serial2.print("Forwarding command ");
    Serial2.print(cmdId);
    Serial2.println(" to PC");
    Serial2.println("DNP3 ESP32 notifying: attempting to connect to PC");
    Serial2.print("Connecting to PC...");
    if (connectWithRetry(outClient, pcIp, 20000)) {
        Serial2.println("connected");
        Serial2.print("Sending to PC, length: ");
        Serial2.println(len);
        unsigned long txStart = micros();
        outClient.write(buf, len);
        outClient.write((const uint8_t*)"ACK", 3);
        outClient.stop();
        Serial2.print("Send time us: ");
        Serial2.println(micros() - txStart);
        txHist[txIndex].len = len;
        memcpy(txHist[txIndex].data, buf, len);
        txIndex = (txIndex + 1) % HIST_SIZE;
        Serial2.println("Message sent to PC");
        Serial.write((const uint8_t*)"ACK", 3);
    } else {
      Serial2.println("failed to connect");
    }
  }

  // From PC to Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    Serial2.println("Connection from PC accepted");
      unsigned long txStart = micros();
    Serial2.println("DNP3 ESP32 received from PC:");
    byte buf[256];
    int len = 0;
    while (inc.connected() && len < sizeof(buf)) {
      if (inc.available()) {
        byte b = inc.read();
        Serial2.print("0x");
        if (b < 16) Serial2.print("0");
        Serial2.print(b, HEX);
        Serial2.print(" ");
        buf[len++] = b;
        Serial.write(b);
        delay(1); // prevent watchdog during prints
      } else {
        delay(1); // keep watchdog fed
      }
    }
    // Let the PC know we received the frame
    inc.write((const uint8_t*)"ACK", 3);
    Serial2.print("Forwarding to Modbus ESP32, length: ");
    Serial2.println(len);
    Serial2.println();
    int cmdId2 = 0;
    if (isDnp3(buf, len)) {
      cmdId2 = identifyCmd(buf + 1, len - 2);
      Serial2.print("Valid DNP3 payload command ");
      Serial2.println(cmdId2 ? cmdId2 : 0);
    } else {
      Serial2.println("Invalid DNP3 frame");
    }
    inc.stop();
    Serial2.print("Send to Modbus us: ");
    Serial2.println(micros() - txStart);
    Serial2.print("Forwarded command ");
    Serial2.print(cmdId2);
    Serial2.println(" to Modbus ESP32");

    txHist[txIndex].len = len;
    memcpy(txHist[txIndex].data, buf, len);
    txIndex = (txIndex + 1) % HIST_SIZE;
    Serial2.println("Message forwarded to Modbus ESP32");

    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
  }
  delay(1); // yield to watchdog
}
