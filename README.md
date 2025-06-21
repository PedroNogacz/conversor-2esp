# conversor-2esp

This repository documents the wiring and connection strategy for a dual-ESP system that converts Modbus commands to DNP3 and back again.

## Hardware layout

- **Sender (ESP8266)**: Generates Modbus commands. Periodically sends commands via Ethernet to the first ESP32.
- **First ESP32 + W5500**: Receives Modbus commands from the ESP8266 over the local network. It relays them over a direct serial connection to the second ESP32 and can also send data back to the ESP8266.
- **Second ESP32 + W5500**: Communicates with the first ESP32 over that serial link. It forwards Modbus frames to the PC as DNP3 and also accepts DNP3 frames from the PC to be returned to the sender.
- **PC**: Runs the DNP3 master application.
- **TP-Link 4‑port modem**: Provides Ethernet connectivity for all nodes.

### Ethernet port assignment

1. **Port 1 – Sender (ESP8266)**
2. **Port 2 – First ESP32 (Modbus side)**
3. **Port 3 – Second ESP32 (DNP3 side)**
4. **Port 4 – PC**

### Wiring steps

1. **Connect each W5500 Ethernet module to the TP-Link modem** using standard Ethernet cables, matching the port assignments above.
2. **Link the two ESP32 boards** with a direct UART connection. Connect TX2 (GPIO17) on the Modbus ESP32 to RX2 (GPIO16) on the DNP3 ESP32 and vice versa for the return path. This serial link carries the translated command from the first ESP32 to the second.
3. **Power each ESP device** according to its requirements (typically 3.3&nbsp;V regulated power). Ensure grounds are common if using UART between ESP32 boards.
4. **From the second ESP32, connect to the PC** via Ethernet over the TP-Link modem. The PC will receive DNP3 messages.

With this arrangement, the ESP8266 sender places Modbus frames onto the network, the first ESP32 relays them via the serial link, and the second ESP32 converts the frames to DNP3 for the PC. Messages from the PC travel the reverse path back to the sender.

### W5500 wiring for each board

The W5500 Ethernet modules require standard SPI wiring. The pin numbers below are suggestions and can be adjusted for your hardware:

| Signal | ESP8266 | ESP32 (Modbus) | ESP32 (DNP3) |
| ------ | ------- | -------------- | ------------- |
| MISO   | D6      | 19             | 19            |
| MOSI   | D7      | 23             | 23            |
| SCK    | D5      | 18             | 18            |
| CS     | D8      | 5              | 5             |
| RESET  | RST     | 16             | 16            |
| INT    | unused  | 4              | 4             |

Power each W5500 module from 3.3&nbsp;V and connect grounds to their respective ESP boards. The Ethernet jack on every W5500 goes to the TP-Link modem using the port assignments above.

### Component summary per board

| Device             | Components               | Connections                                        |
|--------------------|--------------------------|----------------------------------------------------|
| **ESP8266 Sender** | ESP8266 board, W5500     | Ethernet to port 1                                 |
| **Modbus ESP32**   | ESP32 board, W5500       | Ethernet to port 2, UART TX2/RX2 to DNP3 ESP32     |
| **DNP3 ESP32**     | ESP32 board, W5500       | Ethernet to port 3, UART TX2/RX2 to Modbus ESP32   |
| **PC**             | PC running DNP3 master   | Ethernet to port 4                                 |

The two ESP32 boards communicate through a direct 3.3&nbsp;V TTL serial link.
Connect TX2 (GPIO17) on the Modbus board to RX2 (GPIO16) on the DNP3 board and
TX2 on the DNP3 board back to RX2 on the Modbus board. Both sides operate at
115200&nbsp;baud using the default 8N1 format.

### Translation overview

The Modbus ESP32 includes simple routines that wrap Modbus frames inside a
DNP3-style header before sending them to the DNP3 ESP32. Data received in this
format is unwrapped back to Modbus before being forwarded to the sender. These
examples are placeholders—replace them with real protocol handlers for a
production system.

