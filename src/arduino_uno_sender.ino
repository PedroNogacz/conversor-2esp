#include <SPI.h>
#include <Ethernet.h>
#include <avr/wdt.h>

// Example sketch for an Arduino Uno equipped with a W5500 Ethernet shield.
//
// The sketch alternates between sending Modbus frames directly to the Modbus
// ESP32 or wrapping the same bytes in a minimal DNP3 envelope and forwarding
// them to the DNP3 ESP32.  A push button connected to pin 2 toggles which
// destination is used.  Two example commands are transmitted in rotation so
// the PC application can identify them.  Any responses from the Modbus ESP32
// are printed to the serial monitor.

// Replace with your network settings
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 50); // Sender address
IPAddress modbusIp(192, 168, 1, 60); // Modbus ESP32 address
IPAddress dnpIp(192, 168, 1, 70);    // DNP3 ESP32 address

EthernetServer server(1502); // For responses from Modbus ESP32

EthernetClient client;
int ledState = LOW;
const int MODE_BTN = 2;
bool sendModbus = true;
bool lastBtn = HIGH;

// Example Modbus requests (function 3 and 4)
const byte MODBUS_CMDS[][8] = {
  { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B },
  { 0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA }
};
const int NUM_CMDS = sizeof(MODBUS_CMDS) / sizeof(MODBUS_CMDS[0]);
int cmdIndex = 0;

// Initialize serial output and the Ethernet stack then start listening
// for responses from the Modbus ESP32.
void setup() {
  Serial.begin(115200);
  Serial.print("Reset cause: 0x");
  Serial.println(MCUSR, HEX);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(MODE_BTN, INPUT_PULLUP);
  Ethernet.begin(mac, ip);
  if (Ethernet.localIP() != ip) {
    Serial.print("Sender warning: IP mismatch ");
    Serial.println(Ethernet.localIP());
    Ethernet.begin(mac, ip);
  }
  Serial.print("Sender IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
  delay(1000);
}

const unsigned long HEARTBEAT_INTERVAL = 5000; // blink and message every 5 s
const unsigned long SEND_INTERVAL = 5000;    // transmit command every 5 s
unsigned long lastSend = 0;
unsigned long lastBeat = 0;

// Main control loop.  Handles heartbeats, reads the mode button and
// periodically transmits the next command in the current protocol.
void loop() {
  if (millis() - lastBeat > HEARTBEAT_INTERVAL) {
    Serial.println("Sender heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // Check for response from Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    Serial.println("Sender received response:");
    while (inc.available()) {
      byte b = inc.read();
      Serial.print("0x");
      if (b < 16) Serial.print("0");
      Serial.print(b, HEX);
      Serial.print(" ");
      delay(1); // yield while processing
    }
    Serial.println();
    inc.stop();
  }

  // Check mode button
  bool btn = digitalRead(MODE_BTN);
  if (btn == LOW && lastBtn == HIGH) {
    sendModbus = !sendModbus;
    Serial.print("Mode changed to: ");
    Serial.println(sendModbus ? "Modbus" : "DNP3");
    delay(200); // debounce
  }
  lastBtn = btn;

  // Periodically send frame based on selected mode
  if (millis() - lastSend > SEND_INTERVAL) {
    const byte *frame = MODBUS_CMDS[cmdIndex];
    if (sendModbus) {
      if (client.connect(modbusIp, 502)) { // Modbus TCP port
        client.write(frame, 8);
        Serial.print("Sender transmitted Modbus frame ");
        Serial.println(cmdIndex + 1);
        client.stop();
      } else {
        Serial.println("Sender failed to connect for Modbus frame");
      }
    } else {
      if (client.connect(dnpIp, 20000)) { // DNP3 port
        byte dnp[8 + 2];
        dnp[0] = 0x05;
        memcpy(dnp + 1, frame, 8);
        dnp[9] = 0x16;
        client.write(dnp, sizeof(dnp));
        Serial.print("Sender transmitted DNP3 frame ");
        Serial.println(cmdIndex + 1);
        client.stop();
      } else {
        Serial.println("Sender failed to connect for DNP3 frame");
      }
    }
    cmdIndex = (cmdIndex + 1) % NUM_CMDS;
    lastSend = millis();
  }
  delay(1); // keep watchdog happy
}
