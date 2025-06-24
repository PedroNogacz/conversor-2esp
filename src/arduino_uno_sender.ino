#include <SPI.h>
#include <Ethernet.h>
#include <avr/wdt.h>

/*
  Device: **Arduino Uno** with W5500 Ethernet shield (port 1 on the modem)

  This sketch generates a simple test Modbus frame every five seconds and
  sends it to the Modbus ESP32.  When the push button on pin 2 is pressed it
  instead wraps the frame in a very small DNP3 envelope and sends it to the
  second ESP32.  The Ethernet shield already wires the W5500 pins to the Uno
  as follows:
      CS   -> D10
      MOSI -> D11
      MISO -> D12
      SCK  -> D13

  The RJ45 connector on the shield links to the TP-Link modem so the ESP32
  boards can receive the traffic.
*/

// Replace with your network settings
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 50); // Sender address
IPAddress modbusIp(192, 168, 1, 60); // Modbus ESP32 address
IPAddress dnpIp(192, 168, 1, 70);    // DNP3 ESP32 address

EthernetServer server(1502); // For responses from Modbus ESP32

EthernetClient client;
int ledState = LOW;
const int MODE_BTN = 2; // connect one side of the push button here and the
                        // other side to GND
bool sendModbus = true;
bool lastBtn = HIGH;

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

unsigned long lastSend = 0;
unsigned long lastBeat = 0;

void loop() {
  if (millis() - lastBeat > 10000) {
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
  if (millis() - lastSend > 5000) {
    if (sendModbus) {
      if (client.connect(modbusIp, 502)) { // Modbus TCP port
        byte frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B };
        client.write(frame, sizeof(frame));
        Serial.println("Sender transmitted Modbus frame");
        client.stop();
      }
    } else {
      if (client.connect(dnpIp, 20000)) { // DNP3 port
        byte frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B };
        byte dnp[sizeof(frame) + 2];
        dnp[0] = 0x05;
        memcpy(dnp + 1, frame, sizeof(frame));
        dnp[sizeof(frame) + 1] = 0x16;
        client.write(dnp, sizeof(dnp));
        Serial.println("Sender transmitted DNP3 frame");
        client.stop();
      }
    }
    lastSend = millis();
  }
  delay(1); // keep watchdog happy
}
