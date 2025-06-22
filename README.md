# conversor-2esp

This repository documents the wiring and connection strategy for a two-board system that converts Modbus commands to DNP3 and back again.

## Hardware layout

- **Sender (Arduino Uno + W5500 Ethernet shield)**: Generates Modbus commands. Periodically sends commands via Ethernet to the first ESP32. The shield already contains the W5500 chip, so no separate module or extra wiring is required—just stack the shield onto the Uno.
- **First ESP32-WROOM-32 + W5500**: Receives Modbus commands from the Arduino over the local network. It relays them over a direct serial connection to the NodeMCU board and can also send data back to the sender.
- **NodeMCU ESP8266 + W5500**: Communicates with the ESP32 over that serial link. It forwards Modbus frames to the PC as DNP3 and also accepts DNP3 frames from the PC to be returned to the sender.
- **PC**: Runs the DNP3 master application.
- **TP-Link 4‑port modem**: Provides Ethernet connectivity for all nodes.

### Ethernet port assignment

1. **Port 1 – Sender (Arduino Uno)**
2. **Port 2 – ESP32 (Modbus side)**
3. **Port 3 – NodeMCU ESP8266 (DNP3 side)**
4. **Port 4 – PC**

### Wiring steps

1. **Connect each W5500 Ethernet module to the TP-Link modem** using standard Ethernet cables, matching the port assignments above.
2. **Link the ESP32 and NodeMCU boards** with a direct UART connection. Connect TX2 (GPIO17) on the Modbus ESP32 to RX (GPIO3) on the NodeMCU and connect TX (GPIO1) on the NodeMCU back to RX2 (GPIO16) on the ESP32. This serial link carries the translated command from the ESP32 to the NodeMCU.
3. **Power each ESP device** according to its requirements (typically 3.3&nbsp;V regulated power). Ensure grounds are common if using UART between the ESP32 and NodeMCU.
4. **From the NodeMCU, connect to the PC** via Ethernet over the TP-Link modem. The PC will receive DNP3 messages.

With this arrangement, the Arduino Uno sender places Modbus frames onto the network, the ESP32 relays them via the serial link, and the NodeMCU converts the frames to DNP3 for the PC. Messages from the PC travel the reverse path back to the sender.

### W5500 wiring for each board

Note: The Arduino Uno uses a W5500 Ethernet shield with the SPI lines already wired to pins D10--D13. Simply plug the shield into the Uno—no jumpers are necessary. The table below lists the pins for reference only.

The W5500 Ethernet modules expose their SPI pins with labels like **S1**, **S2**, **SK**, **S0** and **RST**. Typical headers also include terminals in this order: **SCLK**, **GND**, **SCS**, **INT**, **MOSI**, **MISO**, **GND**, **3.3V**, **5V**, and **RST**. The table below maps the key signals to the board pins used in this project. The same mapping applies to the ESP32 and the NodeMCU.

| W5500 label | Purpose | Arduino pin | ESP32 pin | ESP8266 pin |
|-------------|---------|-------------|-----------|--------------|
| S2          | MISO    | D12         | GPIO19    | D6 (GPIO12)  |
| S1          | MOSI    | D11         | GPIO23    | D7 (GPIO13)  |
| SK          | SCK     | D13         | GPIO18    | D5 (GPIO14)  |
| S0          | CS      | D10         | GPIO5     | D8 (GPIO15)  |
| RST         | Reset   | RESET       | GPIO16    | D0 (GPIO16)  |
| INT         | Interrupt | unused    | GPIO4     | D4 (GPIO2)   |

Power each W5500 module from 3.3&nbsp;V and connect grounds to the ESP32 and NodeMCU. The Ethernet jack on every W5500 goes to the TP-Link modem using the port assignments above.

Connect the **RST** terminal so the ESP32 or NodeMCU can reset the W5500 during setup. The example sketches pulse GPIO16 (or D0 on the NodeMCU) low for a short period at startup before bringing it high.
#### Dedicated connections
Each W5500 terminal connects to only one microcontroller pin. Avoid wiring the same W5500 signal to more than one GPIO. The serial link is also one-to-one: connect TX2 on each ESP32 solely to the other board's RX2.


### Component summary per board

| Device             | Components               | Connections                                        |
|--------------------|--------------------------|----------------------------------------------------|
| **Arduino Uno Sender** | Arduino Uno, W5500 shield | Ethernet to port 1 |
| **Modbus ESP32**   | ESP32 board, W5500       | Ethernet to port 2, UART TX2/RX2 to NodeMCU |
| **NodeMCU**        | ESP8266 board, W5500     | Ethernet to port 3, UART TX/RX to Modbus ESP32 |
| **PC**             | PC running DNP3 master   | Ethernet to port 4                                 |

The ESP32 and NodeMCU communicate through a direct 3.3&nbsp;V TTL serial link.
Connect TX2 (GPIO17) on the Modbus board to RX (GPIO3) on the NodeMCU and
TX (GPIO1) on the NodeMCU back to RX2 (GPIO16) on the ESP32. Both sides operate at
115200&nbsp;baud using the default 8N1 format.

### Serial pin reference

The ESP32-WROOM-32 exposes UART2 as **TX2** (GPIO17) and **RX2** (GPIO16). The NodeMCU uses **TX** (GPIO1) and **RX** (GPIO3). Wire these terminals together between the boards as shown below:

| Board            | Pin label | GPIO | Connection to |
|------------------|-----------|------|---------------|
| Modbus ESP32     | TX2       | 17   | NodeMCU RX (GPIO3) |
| Modbus ESP32     | RX2       | 16   | NodeMCU TX (GPIO1) |
| NodeMCU          | TX        | 1    | Modbus ESP32 RX2 |
| NodeMCU          | RX        | 3    | Modbus ESP32 TX2 |

### Translation overview

The Modbus ESP32 includes simple routines that wrap Modbus frames inside a
DNP3-style header before sending them to the NodeMCU. Data received in this
format is unwrapped back to Modbus before being forwarded to the sender. These
examples are placeholders—replace them with real protocol handlers for a
production system.

