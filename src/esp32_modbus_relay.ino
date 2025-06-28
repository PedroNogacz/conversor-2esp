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
//   Serial link to second ESP32 using UART1 by default
//     TX  (GPIO22) -> second ESP32 RX (GPIO21)
//     RX  (GPIO21) <- second ESP32 TX (GPIO22)
//   Edit LINK_PORT/LINK_TX/LINK_RX below to use other pins or UARTs
//   The built-in LED on GPIO2 blinks every 5 seconds as a heartbeat.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// Serial port used for the converter link. Set to Serial to use UART0 or
// Serial2 for UART2. Pins can be overridden with LINK_TX/LINK_RX.
#ifndef LINK_PORT
#define LINK_PORT Serial1
#endif

#ifndef LINK_TX
#define LINK_TX 22
#endif

#ifndef LINK_RX
#define LINK_RX 21
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

// Connect with small delays so the watchdog stays fed if the remote side
// is unreachable. Returns true when the connection succeeds.
static bool connectWithRetry(EthernetClient &cli, IPAddress addr, uint16_t port)
{
  for (int attempt = 0; attempt < 3; attempt++) {
    if (cli.connect(addr, port)) {
      return true;
    }
    Serial2.println("connect failed, retrying");
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
  Serial2.begin(115200);
  LINK_PORT.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX); // Link to DNP3 ESP32
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
    Serial2.println("Modbus ESP32 error: unable to start Ethernet");
    delay(2000);
    ESP.restart();
  }
  Serial2.print("Modbus ESP32 IP: ");
  Serial2.println(Ethernet.localIP());
  server.begin();
}

// Handle traffic between the Arduino sender and the DNP3 ESP32 while
// producing diagnostic output.
void loop() {
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    Serial2.println("Modbus ESP32 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Data from Arduino Uno to DNP3 ESP32
  EthernetClient client = server.available();
  if (client) {
    Serial2.println("Connection from sender accepted");
    unsigned long rxStart = micros();
    byte mbBuf[256];
    int mbLen = 0;
    Serial2.println("Modbus ESP32 received from sender:");
    while (client.connected() && mbLen < sizeof(mbBuf)) {
      if (client.available()) {
        byte b = client.read();
        Serial2.print("0x");
        if (b < 16) Serial2.print("0");
        Serial2.print(b, HEX);
        Serial2.print(" ");
        mbBuf[mbLen++] = b;
        delay(1); // avoid watchdog reset while printing
      } else {
        delay(1); // yield to watchdog
      }
    }
    unsigned long rxEnd = micros();
    Serial2.print("Time to receive us: ");
    Serial2.println(rxEnd - rxStart);
    Serial2.println();
    int cmd = identifyCmd(mbBuf, mbLen);
    if (cmd) {
      Serial2.print("Identified Modbus command ");
      Serial2.println(cmd);
    } else {
      Serial2.println("Unknown Modbus command");
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
    Serial2.print("Translation us: ");
    Serial2.println(transEnd - transStart);
    Serial2.print("Translated to DNP3: ");
    for (int i = 0; i < outLen; i++) {
      Serial2.print("0x");
      if (dnpBuf[i] < 16) Serial2.print("0");
      Serial2.print(dnpBuf[i], HEX);
      Serial2.print(" ");
      delay(1); // feed watchdog during long prints
    }
    Serial2.println();
    int cmdId = 0;
    if (isDnp3(dnpBuf, outLen)) {
      cmdId = identifyCmd(dnpBuf + 1, outLen - 2);
      Serial2.print("Valid DNP3 payload command ");
      Serial2.println(cmdId ? cmdId : 0);
    } else {
      Serial2.println("Invalid DNP3 frame");
    }
    Serial2.println(" -> sending to DNP3 ESP32");
    Serial2.print("Forwarding command ");
    Serial2.print(cmdId);
    Serial2.println(" to DNP3 ESP32");
    Serial2.print("Sending to DNP3 ESP32, length: ");
    Serial2.println(outLen);
    unsigned long txStart = micros();
    LINK_PORT.write(dnpBuf, outLen);
    LINK_PORT.write((const uint8_t*)"ACK", 3);
    Serial2.print("Send time us: ");
    Serial2.println(micros() - txStart);
    // store history of transmitted messages
    txHist[txIndex].len = outLen;
    memcpy(txHist[txIndex].data, dnpBuf, outLen);
    txIndex = (txIndex + 1) % HIST_SIZE;
  }

  // Data from DNP3 ESP32 back to sender
  if (LINK_PORT.available()) {
    byte inBuf[256];
    int len = 0;
    while (LINK_PORT.available() && len < (int)sizeof(inBuf)) {
      inBuf[len++] = LINK_PORT.read();
      yield();
    }
    Serial2.print("Received from DNP3 ESP32, length: ");
    Serial2.println(len);
    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, inBuf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    Serial2.print("Modbus ESP32 received DNP3: ");
    for (int i = 0; i < len; i++) {
      Serial2.print("0x");
      if (inBuf[i] < 16) Serial2.print("0");
      Serial2.print(inBuf[i], HEX);
      Serial2.print(" ");
      delay(1); // feed watchdog during prints
    }
    Serial2.println();
    if (isDnp3(inBuf, len)) {
      int id = identifyCmd(inBuf + 1, len - 2);
      Serial2.print("DNP3 frame with command ");
      Serial2.println(id ? id : 0);
    } else {
      Serial2.println("Invalid DNP3 frame");
    }

    byte mbBuf[260];
    unsigned long trans2Start = micros();
    int outLen = dnp3ToModbus(inBuf, len, mbBuf, sizeof(mbBuf));
    unsigned long trans2End = micros();
    Serial2.print("Translation us: ");
    Serial2.println(trans2End - trans2Start);
    Serial2.print("Translated to Modbus: ");
    for (int i = 0; i < outLen; i++) {
      Serial2.print("0x");
      if (mbBuf[i] < 16) Serial2.print("0");
      Serial2.print(mbBuf[i], HEX);
      Serial2.print(" ");
      delay(1); // keep watchdog alive during print
    }
    Serial2.println();
    int cmdId2 = identifyCmd(mbBuf, outLen);
    Serial2.print("Identified Modbus command ");
    Serial2.println(cmdId2 ? cmdId2 : 0);
    Serial2.println(" -> forwarding to sender");
    Serial2.print("Forwarding command ");
    Serial2.print(cmdId2);
    Serial2.println(" to sender");
    Serial2.print("Connecting to sender...");
    if (connectWithRetry(outClient, senderIp, 1502)) {
      Serial2.println("connected");
        unsigned long tx2Start = micros();
        outClient.write(mbBuf, outLen);
        // let the sender know we handled the request
        outClient.write((const uint8_t*)"ACK", 3);
        outClient.stop();
        Serial2.print("Send time us: ");
        Serial2.println(micros() - tx2Start);
      // store history of transmitted messages
      txHist[txIndex].len = outLen;
      memcpy(txHist[txIndex].data, mbBuf, outLen);
      txIndex = (txIndex + 1) % HIST_SIZE;
      Serial2.println("Message forwarded");
      LINK_PORT.write((const uint8_t*)"ACK", 3);
    } else {
      Serial2.println("failed to connect");
    }
  }
  delay(1); // yield to keep watchdog happy
}
