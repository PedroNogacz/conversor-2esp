#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <time.h>
#include <avr/wdt.h>
#include <stdio.h>

// Example sketch for an Arduino Uno equipped with a W5500 Ethernet shield.
//
// The sketch sends Modbus commands to the Modbus ESP32 and then wraps the same
// bytes in a minimal DNP3 envelope to send to the DNP3 ESP32.  Five commands are
// transmitted using one protocol, then the next five using the other protocol
// with a five second delay between each.  The loop repeats indefinitely and all
// connection attempts are printed so the user can see whether each frame was
// delivered.  Any responses from the Modbus ESP32 are printed to the serial
// monitor.

// Replace with your network settings
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 50); // Sender address
IPAddress modbusIp(192, 168, 1, 60); // Modbus ESP32 address
IPAddress dnpIp(192, 168, 1, 70);    // DNP3 ESP32 address

EthernetServer server(1502);        // For responses from Modbus ESP32
EthernetServer dnpServer(20000);    // Show any frames from DNP3 ESP32
EthernetUDP udp;
IPAddress ntpServer(129, 6, 15, 28); // time.nist.gov
const unsigned int NTP_PORT = 8888;
byte ntpBuf[48];
unsigned long ntpOffsetMs = 0;

EthernetClient client;
int ledState = LOW;
bool sendModbus = true;           // true -> sending Modbus, false -> sending DNP3
int cycleCount = 0;               // how many frames have been sent in current mode

// Five example Modbus requests used for both protocols
const byte MODBUS_CMDS[][8] = {
  { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B }, // read 2 holding regs @0
  { 0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA }, // read 1 input reg  @1
  { 0x01, 0x03, 0x00, 0x10, 0x00, 0x01, 0x85, 0xCF }, // read 1 holding reg @16
  { 0x01, 0x04, 0x00, 0x20, 0x00, 0x01, 0x30, 0x00 }, // read 1 input reg  @32
  { 0x01, 0x03, 0x00, 0x30, 0x00, 0x02, 0xC4, 0x04 }  // read 2 holding regs @48
};
const int NUM_CMDS = sizeof(MODBUS_CMDS) / sizeof(MODBUS_CMDS[0]);
// Only the first two commands are used in this example. Change the
// ACTIVE_CMDS array to choose different ones.
const int ACTIVE_CMDS = 2;
int chosenCmds[ACTIVE_CMDS] = {0, 1};
int chosenIndex = 0;
uint8_t lastSentFc = 0;
unsigned lastCmdId = 0;
unsigned cmdCounter = 0;

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

static void sendNtpPacket(IPAddress &address) {
  memset(ntpBuf, 0, sizeof(ntpBuf));
  ntpBuf[0] = 0b11100011;
  udp.beginPacket(address, 123);
  udp.write(ntpBuf, sizeof(ntpBuf));
  udp.endPacket();
}

static unsigned long getNtpTime() {
  sendNtpPacket(ntpServer);
  delay(1000);
  if (udp.parsePacket()) {
    udp.read(ntpBuf, sizeof(ntpBuf));
    unsigned long high = word(ntpBuf[40], ntpBuf[41]);
    unsigned long low = word(ntpBuf[42], ntpBuf[43]);
    return ((high << 16) | low) - 2208988800UL;
  }
  return 0;
}

static void syncTime() {
  udp.begin(NTP_PORT);
  for (int i = 0; i < 5; i++) {
    unsigned long t = getNtpTime();
    if (t) {
      ntpOffsetMs = t * 1000UL - millis();
      char ts[12];
      formatTime(currentTimeMs(), ts, sizeof(ts));
      Serial.print("Time synced ");
      Serial.println(ts);
      udp.stop();
      return;
    }
  }
  Serial.println("NTP sync failed");
  udp.stop();
}
static unsigned long currentTimeMs() {
  return millis() + ntpOffsetMs;
}

// Format epoch milliseconds into HH:MM:SS for logging.
static void formatTime(unsigned long ms, char *out, size_t outSize) {
  unsigned long secs = ms / 1000;
  unsigned long h = (secs % 86400UL) / 3600UL;
  unsigned long m = (secs % 3600UL) / 60UL;
  unsigned long s = secs % 60UL;
  snprintf(out, outSize, "%02lu:%02lu:%02lu", h, m, s);
}

// Print the current time in brackets so logs are easier to follow
static bool printedStart = false;
static void printTimestamp() {
  if (printedStart) return;
  char ts[12];
  formatTime(currentTimeMs(), ts, sizeof(ts));
  Serial.print("[");
  Serial.print(ts);
  Serial.print("] ");
  printedStart = true;
}

// Initialize serial output and the Ethernet stack then start listening
// for responses from the Modbus ESP32.
void setup() {
  Serial.begin(115200);
  Serial.print("Reset cause: 0x");
  Serial.println(MCUSR, HEX);
  pinMode(LED_BUILTIN, OUTPUT);
  Ethernet.begin(mac, ip);
  if (Ethernet.localIP() != ip) {
    Serial.print("Sender warning: IP mismatch ");
    Serial.println(Ethernet.localIP());
    Ethernet.begin(mac, ip);
  }
  Serial.print("Sender IP: ");
  Serial.println(Ethernet.localIP());
  syncTime();
  server.begin();
  dnpServer.begin();
  delay(1000);
  printTimestamp();
  Serial.println("Sender started");
}

const unsigned long HEARTBEAT_INTERVAL = 5000; // blink and message every 5 s
const unsigned long sendInterval = 10000;      // transmit command every 10 s
unsigned long lastSend = 0;
unsigned long lastBeat = 0;

// Main control loop.  Handles heartbeats and periodically transmits the next
// command in the current protocol.
void loop() {
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    printTimestamp();
    Serial.println("Sender heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Check for response from Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    byte buf[32];
    int len = 0;
    while (inc.available() && len < (int)sizeof(buf)) {
      buf[len++] = inc.read();
    }
    printTimestamp();
    Serial.println("[MODBUS] Sender received response:");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    if (len == 3 && buf[0] == 'A' && buf[1] == 'C' && buf[2] == 'K') {
      Serial.print("R");
      Serial.print(lastCmdId);
      Serial.print(": ACK for ");
      Serial.println(cmdDescription(lastSentFc));
    }
    inc.stop();
  }

  // Check for response from DNP3 ESP32
  EthernetClient incDnp = dnpServer.available();
  if (incDnp) {
    byte buf[32];
    int len = 0;
    while (incDnp.available() && len < (int)sizeof(buf)) {
      buf[len++] = incDnp.read();
    }
    printTimestamp();
    Serial.println("[DNP3] Sender received response:");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    if (len == 3 && buf[0] == 'A' && buf[1] == 'C' && buf[2] == 'K') {
      Serial.print("R");
      Serial.print(lastCmdId);
      Serial.print(": ACK for ");
      Serial.println(cmdDescription(lastSentFc));
    }
    incDnp.stop();
  }


  // Periodically send frame based on selected mode
  if (millis() - lastSend > sendInterval) {
    const byte *frame = MODBUS_CMDS[chosenCmds[chosenIndex]];
    uint8_t fc = frame[1];
    cmdCounter++;
    lastCmdId = cmdCounter;
    Serial.print("C");
    Serial.print(lastCmdId);
    Serial.print(": ");
    Serial.println(cmdDescription(fc));
    printTimestamp();
    if (sendModbus) {
      Serial.print("[MODBUS] Connecting for frame...");
      if (client.connect(modbusIp, 502)) { // Modbus TCP port
        Serial.println("connected");
        Serial.print("[MODBUS] Sending: ");
        for (int i = 0; i < 8; i++) {
          Serial.print("0x");
          if (frame[i] < 16) Serial.print("0");
          Serial.print(frame[i], HEX);
          Serial.print(" ");
        }
        Serial.println();
        client.write(frame, 8);
        Serial.print("[MODBUS] Sent frame ");
        Serial.print(chosenCmds[chosenIndex] + 1);
        Serial.print(" - ");
        Serial.println(cmdDescription(fc));
        lastSentFc = fc;
        client.stop();
      } else {
        Serial.println("failed");
      }
    } else {
      Serial.print("[DNP3] Connecting for frame...");
      if (client.connect(dnpIp, 20000)) { // DNP3 port
        Serial.println("connected");
        byte dnp[8 + 2];
        dnp[0] = 0x05;
        memcpy(dnp + 1, frame, 8);
        dnp[9] = 0x16;
        Serial.print("[DNP3] Sending: ");
        for (int i = 0; i < 10; i++) {
          byte b = dnp[i];
          Serial.print("0x");
          if (b < 16) Serial.print("0");
          Serial.print(b, HEX);
          Serial.print(" ");
        }
        Serial.println();
        client.write(dnp, sizeof(dnp));
        Serial.print("[DNP3] Sent frame ");
        Serial.print(chosenCmds[chosenIndex] + 1);
        Serial.print(" - ");
        Serial.println(cmdDescription(fc));
        lastSentFc = fc;
        client.stop();
      } else {
        Serial.println("failed");
      }
    }
    chosenIndex = (chosenIndex + 1) % ACTIVE_CMDS;
    cycleCount++;
    if (cycleCount >= 5) {
      cycleCount = 0;
      sendModbus = !sendModbus;
      printTimestamp();
      Serial.print("Switching to ");
      Serial.println(sendModbus ? "[MODBUS]" : "[DNP3]");
    }
    lastSend = millis();
  }
  delay(1); // keep watchdog happy
}
