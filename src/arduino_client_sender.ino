#include <SPI.h>
#include <Ethernet.h>
#ifdef ARDUINO_ARCH_AVR
#include <avr/wdt.h>
#endif
#ifdef ESP32
#include <esp_system.h>
#endif
#include <stdio.h>

/*
Step-by-step usage:
1. Connect the W5500 Ethernet shield to the Arduino Uno.
2. Wire the network so the ESP32 converters and PC share the same LAN.
3. Open this sketch in the Arduino IDE and select the Uno board.
4. Upload the sketch and open the Serial Monitor at 115200 baud.
5. The sketch alternates between sending Modbus and DNP3 commands every
   five cycles, printing each step to the console.
*/

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


// Format epoch milliseconds into MM:SS:mmm for logging. The sender
// prints only minutes, seconds and milliseconds as requested.
static void formatTime(unsigned long ms, char *out, size_t outSize) {
  unsigned long total = ms / 1000UL;
  unsigned long m = (total / 60UL) % 60UL;
  unsigned long s = total % 60UL;
  unsigned long msPart = ms % 1000UL;
  snprintf(out, outSize, "%02lu:%02lu:%03lu", m, s, msPart);
}

// Print the current time in brackets so logs are easier to follow
// Print the current time in brackets for easier log correlation
static void printTimestamp() {
  char ts[12];
  formatTime(millis(), ts, sizeof(ts));
  Serial.print("[");
  Serial.print(ts);
  Serial.print("] ");
}

// Initialize serial output and the Ethernet stack then start listening
// for responses from the Modbus ESP32.
void setup() {
  // Step 1: start the serial console for debug messages.
  Serial.begin(115200);
  Serial.print("Reset cause: ");
#ifdef ARDUINO_ARCH_AVR
  Serial.print("0x");
  Serial.println(MCUSR, HEX);
#elif defined(ESP32)
  Serial.println((int)esp_reset_reason());
#else
  Serial.println("N/A");
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  // Step 2: configure the W5500 Ethernet shield.
  Ethernet.begin(mac, ip);
  if (Ethernet.localIP() != ip) {
    Serial.print("Sender warning: IP mismatch ");
    Serial.println(Ethernet.localIP());
    Ethernet.begin(mac, ip);
  }
  Serial.print("Sender IP: ");
  Serial.println(Ethernet.localIP());
  // Step 3: start servers that receive responses from both converters.
  server.begin();
  dnpServer.begin();
  delay(1000);
  printTimestamp();
  Serial.println("Sender started");
  // Step 4: wait before sending the first command.
  delay(10000); // give network components time to settle
}

const unsigned long HEARTBEAT_INTERVAL = 5000; // blink and message every 5 s
const unsigned long sendInterval = 10000;      // transmit command every 10 s
unsigned long lastSend = 0;
unsigned long lastBeat = 0;

// Main control loop.  Handles heartbeats and periodically transmits the next
// command in the current protocol.
void loop() {
  // Step 1: heartbeat and LED blink.
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    printTimestamp();
    Serial.println("Sender heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Step 2: check for responses from the Modbus ESP32.
  EthernetClient inc = server.available();
  if (inc) {
    if (sendModbus) {
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
    }
    inc.stop();
  }

  // Step 3: check for responses from the DNP3 ESP32.
  EthernetClient incDnp = dnpServer.available();
  if (incDnp) {
    if (!sendModbus) {
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
    }
    incDnp.stop();
  }


  // Step 4: periodically send the next command in the chosen protocol.
  if (millis() - lastSend > sendInterval) {
    const byte *frame = MODBUS_CMDS[chosenCmds[chosenIndex]];
    uint8_t fc = frame[1];
    cmdCounter++;
    lastCmdId = cmdCounter;
    printTimestamp();
    Serial.print("C");
    Serial.print(lastCmdId);
    Serial.print(": ");
    Serial.println(cmdDescription(fc));
    if (sendModbus) {
      Serial.println("[MODBUS] Connecting for frame...");
      if (client.connect(modbusIp, 502)) { // Modbus TCP port
        printTimestamp();
        Serial.print("[MODBUS] Connected IP ");
        Serial.println(modbusIp);
        printTimestamp();
        Serial.print("[MODBUS] Sending: ");
        for (int i = 0; i < 8; i++) {
          Serial.print("0x");
          if (frame[i] < 16) Serial.print("0");
          Serial.print(frame[i], HEX);
          Serial.print(" ");
        }
        Serial.println();
        client.write(frame, 8);
        printTimestamp();
        Serial.print("[MODBUS] Sent frame ");
        Serial.print(chosenCmds[chosenIndex] + 1);
        Serial.print(" - ");
        Serial.println(cmdDescription(fc));
        printTimestamp();
        Serial.println("[MODBUS] Waiting Response");
        unsigned long waitStart = millis();
        while (client.connected() && !client.available() &&
               millis() - waitStart < 1000) {
          delay(1);
        }
        if (client.available()) {
          byte resp[16];
          int rlen = 0;
          while (client.available() && rlen < (int)sizeof(resp)) {
            resp[rlen++] = client.read();
          }
          printTimestamp();
          Serial.print("[MODBUS] Response: ");
          for (int i = 0; i < rlen; i++) {
            Serial.print("0x");
            if (resp[i] < 16) Serial.print("0");
            Serial.print(resp[i], HEX);
            Serial.print(" ");
          }
          Serial.println();
          if (rlen >= 2) {
            printTimestamp();
            Serial.print("[MODBUS] Meaning - ");
            Serial.println(cmdDescription(resp[1]));
          }
        } else {
          printTimestamp();
          Serial.println("[MODBUS] No response");
        }
        lastSentFc = fc;
        client.stop();
        Serial.println();
      } else {
        Serial.println("failed");
      }
    } else {
      Serial.println("[DNP3] Connecting for frame...");
      if (client.connect(dnpIp, 20000)) { // DNP3 port
        printTimestamp();
        Serial.print("[DNP3] Connected IP ");
        Serial.println(dnpIp);
        byte dnp[8 + 2];
        dnp[0] = 0x05;
        memcpy(dnp + 1, frame, 8);
        dnp[9] = 0x16;
        printTimestamp();
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
        printTimestamp();
        Serial.print("[DNP3] Sent frame ");
        Serial.print(chosenCmds[chosenIndex] + 1);
        Serial.print(" - ");
        Serial.println(cmdDescription(fc));

        printTimestamp();
        Serial.println("[DNP3] Waiting Response");

        // Wait briefly for an ACK from the DNP3 converter before closing
        unsigned long waitStart = millis();
        while (client.connected() && !client.available() &&
               millis() - waitStart < 1000) {
          delay(1);
        }
        if (client.available()) {
          byte resp[16];
          int rlen = 0;
          while (client.available() && rlen < (int)sizeof(resp)) {
            resp[rlen++] = client.read();
          }
          printTimestamp();
          Serial.print("[DNP3] Response: ");
          for (int i = 0; i < rlen; i++) {
            Serial.print("0x");
            if (resp[i] < 16) Serial.print("0");
            Serial.print(resp[i], HEX);
            Serial.print(" ");
          }
          Serial.println();
          if (rlen == 3 && resp[0] == 'A' && resp[1] == 'C' && resp[2] == 'K') {
            printTimestamp();
            Serial.println("[DNP3] Meaning - ACK");
          } else if (rlen >= 4 && resp[0] == 0x05 && resp[rlen - 1] == 0x16) {
            printTimestamp();
            Serial.print("[DNP3] Meaning - ");
            Serial.println(cmdDescription(resp[2]));
          } else if (rlen >= 2) {
            printTimestamp();
            Serial.print("[DNP3] Meaning - ");
            Serial.println(cmdDescription(resp[1]));
          }
        } else {
          printTimestamp();
          Serial.println("[DNP3] No response");
        }
        
        lastSentFc = fc;
        client.stop();
        Serial.println();
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
  // Step 5: short delay so the watchdog timer is serviced.
  delay(1); // keep watchdog happy
}
