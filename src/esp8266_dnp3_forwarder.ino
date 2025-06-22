#include <SPI.h>
#include <Ethernet.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x03 };
IPAddress ip(192, 168, 1, 70);
IPAddress pcIp(192, 168, 1, 80);

const int W5500_RST = 16; // D0 on NodeMCU

EthernetServer server(20000); // Listen for PC
EthernetClient outClient;
unsigned long lastBeat = 0;
int ledState = LOW;

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

void setup() {
  Serial.begin(115200); // Link with Modbus ESP32
  Serial.print("Reset reason: ");
  Serial.println(ESP.getResetReason());
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(50);
  digitalWrite(W5500_RST, HIGH);
  delay(50);
  pinMode(LED_BUILTIN, OUTPUT);
  Ethernet.begin(mac, ip);
  if (Ethernet.localIP() != ip) {
    Serial.print("DNP3 ESP8266 error: IP mismatch ");
    Serial.println(Ethernet.localIP());
    Serial.println("Restarting to reconfigure IP...");
    delay(2000);
    ESP.restart();
  }
  Serial.print("DNP3 ESP8266 IP: ");
  Serial.println(Ethernet.localIP());
  server.begin();
}

void loop() {
  if (millis() - lastBeat > 10000) {
    Serial.println("DNP3 ESP8266 heartbeat");
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
    lastBeat = millis();
  }
  // From Modbus ESP32 to PC
  if (Serial.available()) {
    byte buf[256];
    int len = Serial.readBytes(buf, sizeof(buf));
    delay(1); // yield for watchdog
    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
    Serial.print("DNP3 ESP8266 received from Modbus: ");
    for (int i = 0; i < len; i++) {
      Serial.print("0x");
      if (buf[i] < 16) Serial.print("0");
      Serial.print(buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println(" -> sending to PC");
    Serial.print("Connecting to PC...");
    if (outClient.connect(pcIp, 20000)) {
      Serial.println("connected");
      outClient.write(buf, len);
      outClient.stop();
      txHist[txIndex].len = len;
      memcpy(txHist[txIndex].data, buf, len);
      txIndex = (txIndex + 1) % HIST_SIZE;
      Serial.println("Message sent to PC");
    } else {
      Serial.println("failed to connect");
    }
  }

  // From PC to Modbus ESP32
  EthernetClient inc = server.available();
  if (inc) {
    Serial.println("Connection from PC accepted");
    Serial.println("DNP3 ESP8266 received from PC:");
    byte buf[256];
    int len = 0;
    while (inc.connected() && len < sizeof(buf)) {
      if (inc.available()) {
        byte b = inc.read();
        Serial.print("0x");
        if (b < 16) Serial.print("0");
        Serial.print(b, HEX);
        Serial.print(" ");
        buf[len++] = b;
        Serial.write(b);
      } else {
        delay(1); // keep watchdog fed
      }
    }
    Serial.println();
    inc.stop();

    txHist[txIndex].len = len;
    memcpy(txHist[txIndex].data, buf, len);
    txIndex = (txIndex + 1) % HIST_SIZE;
    Serial.println("Message forwarded to Modbus ESP32");

    rxHist[rxIndex].len = len;
    memcpy(rxHist[rxIndex].data, buf, len);
    rxIndex = (rxIndex + 1) % HIST_SIZE;
  }
  delay(1); // yield to watchdog
}
