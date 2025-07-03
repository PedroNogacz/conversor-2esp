# conversor-2esp

This repository documents the wiring and connection strategy for a Modbus-to-DNP3 bridge built from one ESP32 board and a second ESP32.  The two boards translate in both directions: Modbus frames from the first ESP32 become DNP3 frames on the second board, while DNP3 frames arriving from that board are translated back to Modbus and forwarded on.

## Hardware layout

- **Sender (Arduino Uno + W5500 Ethernet shield)**: Generates Modbus commands. Periodically sends commands via Ethernet to the first ESP32. The shield already contains the W5500 chip, so no separate module or extra wiring is required—just stack the shield onto the Uno.
- **First ESP32-WROOM-32 + W5500**: Receives Modbus commands from the Arduino over the local network. It relays them over a direct serial connection to the second ESP32 board and can also send data back to the sender.
- **Second ESP32-WROOM-32 + W5500**: Communicates with the first ESP32 over that serial link. Modbus frames received from the first ESP32 are forwarded to the PC as DNP3. DNP3 frames from the PC are relayed back so the first ESP32 can convert them to Modbus before also sending them to the PC.
- **PC**: Runs listeners for both protocols and receives whichever format the converters send.
- **TP-Link 4‑port modem**: Provides Ethernet connectivity for all nodes.

### Ethernet port assignment

1. **Port 1 – Sender (Arduino Uno)**
2. **Port 2 – ESP32 (Modbus side)**
3. **Port 3 – second ESP32 (DNP3 side)**
4. **Port 4 – PC**

### Wiring steps

1. **Connect each W5500 Ethernet module to the TP-Link modem** using standard Ethernet cables, matching the port assignments above.
2. **Link the ESP32 and second ESP32 boards** with a direct UART connection. Connect TX (GPIO22) on the Modbus ESP32 to RX (GPIO21) on the second ESP32 and connect TX (GPIO22) on the second ESP32 back to RX (GPIO21) on the ESP32. This serial link carries the translated command from the ESP32 to the second ESP32.
3. **Power each ESP device** according to its requirements (typically 3.3&nbsp;V regulated power). Ensure grounds are common if using UART between the ESP32 and second ESP32.
4. **From the second ESP32, connect to the PC** via Ethernet over the TP-Link modem. Depending on the direction of travel the PC may receive either protocol.
5. **Wire a mode button to the Arduino Uno**. Connect one side of a push button to digital pin 2 and the other side to GND. The sketch uses the internal pull-up resistor so the default state selects Modbus mode.

With this arrangement, a Modbus command from the sender travels through the Modbus ESP32 then the DNP3 ESP32 before reaching the PC. If a DNP3 command is sent instead, it flows through the DNP3 ESP32, is converted back to Modbus on the first board and then forwarded to the PC as well.

### W5500 wiring for each board

Note: The Arduino Uno uses a W5500 Ethernet shield with the SPI lines already wired to pins D10--D13. Simply plug the shield into the Uno—no jumpers are necessary. The table below lists the pins for reference only.

The W5500 Ethernet modules expose their SPI pins with labels like **S1**, **S2**, **SK**, **S0** and **RST**. Typical headers also include terminals in this order: **SCLK**, **GND**, **SCS**, **INT**, **MOSI**, **MISO**, **GND**, **3.3V**, **5V**, and **RST**. The table below maps the key signals to the board pins used in this project. The same mapping applies to the ESP32 and the second ESP32.

| W5500 label | Purpose | Arduino pin | ESP32 pin |
|-------------|---------|-------------|-----------|
| S2          | MISO    | D12         | GPIO19    |
| S1          | MOSI    | D11         | GPIO23    |
| SK          | SCK     | D13         | GPIO18    |
| S0          | CS      | D10         | GPIO5     |
| RST         | Reset   | RESET       | GPIO16    |
| INT         | Interrupt | unused    | GPIO4     |

Power each W5500 module from 3.3&nbsp;V and connect grounds to the ESP32 and second ESP32. The Ethernet jack on every W5500 goes to the TP-Link modem using the port assignments above.

Connect the **RST** terminal so the ESP32 or second ESP32 can reset the W5500 during setup. The example sketches pulse GPIO16 on each board low for a short period at startup before bringing it high.
#### Dedicated connections
Each W5500 terminal connects to only one microcontroller pin. Avoid wiring the same W5500 signal to more than one GPIO. The serial link is also one-to-one: connect TX on each ESP32 solely to the other board's RX.


### Component summary per board

| Device             | Components               | Connections                                        |
|--------------------|--------------------------|----------------------------------------------------|
| **Arduino Uno Sender** | Arduino Uno, W5500 shield | Ethernet to port 1 |
| **Modbus ESP32**   | ESP32 board, W5500       | Ethernet to port 2, UART TX/RX to second ESP32 |
| **second ESP32**        | ESP32 board, W5500     | Ethernet to port 3, UART TX/RX to Modbus ESP32 |
| **PC**             | PC running DNP3 master   | Ethernet to port 4                                 |

The ESP32 and second ESP32 communicate through a direct 3.3&nbsp;V TTL serial link.
Connect TX (GPIO22) on each ESP32 to the other board's RX (GPIO21). Both sides operate at
115200&nbsp;baud using the default 8N1 format.

### Serial pin reference

Both ESP32 boards use UART1 for the converter link. This exposes **TX** (GPIO22) and **RX** (GPIO21) on each board. Wire these terminals together between the boards as shown below:

| Board            | Pin label | GPIO | Connection to |
|------------------|-----------|------|---------------|
| Modbus ESP32     | TX        | 22   | second ESP32 RX (GPIO21) |
| Modbus ESP32     | RX        | 21   | second ESP32 TX (GPIO22) |
| second ESP32          | TX        | 22   | Modbus ESP32 RX (GPIO21) |
| second ESP32          | RX        | 21   | Modbus ESP32 TX (GPIO22) |

### Viewing debug output

All diagnostic messages from both ESP32 sketches now print to the board's USB
serial port at 115200&nbsp;baud. Open a serial monitor on the ESP32 USB
connection to watch the startup sequence and see any errors or heartbeat
messages. If you only see the ROM boot log repeating, the firmware likely
failed to initialise the Ethernet module and is restarting until it succeeds.
Check the W5500 wiring and power.

### Mode selection button

The Arduino sketch reads a push button on digital pin&nbsp;2 to choose which
protocol it sends. In the unpressed state the Uno transmits Modbus frames to the
Modbus ESP32. Pressing the button toggles to DNP3 mode and the same frames are
wrapped in a minimal DNP3 header and sent to the DNP3 ESP32 every ten seconds.
After startup the sender waits ten seconds before transmitting its first command
so the network can stabilise.
All of the example commands listed in `MODBUS_CMDS` are now sent one after the
other. Edit that array to change the order or remove commands as needed.

Both ESP32 sketches now verify these messages. When either board receives a
frame it prints which example command was recognised or notes that the bytes do
not match the expected format. This helps confirm the converter link is working
and that Modbus frames are preserved inside the DNP3 wrapper.
Whenever a connection is accepted over Ethernet the log now includes the
remote IP address so you can see which host sent the command.

Each board logs the command name based on the table in
`tabela_modbus_dnp3.md` so the meaning of every request is shown.  Commands are
numbered sequentially – the sender prints ``C1`` and each converter
prints the same number when forwarding.  The matching reply is labelled
``R1`` and so on. Only the startup time of the sender and both
converters is printed; later messages omit timestamps.

### Translation overview

The Modbus ESP32 includes simple routines that wrap Modbus frames inside a
DNP3-style header before sending them to the second ESP32. Data received in this
format is unwrapped back to Modbus before being forwarded to the sender. Earlier
revisions only recognised a handful of example requests. The Modbus converter
now identifies commands purely by their Modbus function code and returns the
sample response from the command table, so any request using those function
codes will receive the documented reply. The DNP3 converter continues to match
its example commands byte-for-byte, keeping the DNP3 protocol independent of the
Modbus logic. These sketches remain simplified examples rather than a complete
implementation.

Earlier versions exchanged a short ``ACK`` between the two ESP32 boards
after forwarding a frame. This internal handshake has been removed so
only the converter currently handling the command replies. The Arduino
sender still prints any acknowledgements it receives to help confirm
that the command completed.

Each sketch prints a timestamp based on its own uptime whenever a message is
logged. When a command is sent the sender prints the exact bytes before
transmission and reports ``ACK`` when the converter confirms receipt. This makes
comparing logs across devices easier without relying on network time.

### Handling watchdog resets

If either ESP32 suddenly resets with `Reset reason: 5` (shown in the
boot log as `TG1WDT_SYS_RESET`), the watchdog timer has fired. This usually
means the code spent too long inside a blocking function. Review any long
initialization or read loops and insert `yield()` or short `delay()` calls so the
watchdog can run. Adding `Serial.println` statements around those sections helps
identify where the application gets stuck.

### Guarding against connection hangs

Helper functions in the sketches retry `Ethernet.begin()` and TCP `connect()`
operations. If the W5500 fails to initialise or SPI locks up, the code resets
the module and checks `Ethernet.hardwareStatus()` before trying again. Each
attempt pauses with `delay()`/`yield()` so the watchdog keeps running. After
several unsuccessful tries the board reboots, helping recover from wiring or
network faults.


### PC listener
Run `python_receiver_pc.py` on the PC to capture traffic from both ESP32
converters.  The script listens on **port&nbsp;20000** for DNP3 frames from the
DNP3 ESP32 and **port&nbsp;1502** for Modbus frames from the Modbus ESP32.  A
Tkinter window displays each message in separate panes while the console prints
the same information. The decoded command name is shown so you can verify which
request was recognized. A small graph at the bottom of the window tracks how
many messages were recognized versus marked as **Unknown** for each protocol so
you can gauge the overall health of the converters.

The script also updates a file named `command_archive.txt` in the same
directory. It records how many times each command has been seen for the
Modbus and DNP3 protocols. Every new message refreshes this archive so you
can easily review which commands reached the PC.

#### Requirements

The listener relies solely on Python's standard library. It uses the Tkinter
GUI toolkit which is typically bundled with Python on Windows and macOS. On
Debian/Ubuntu based distributions install it with:

```
sudo apt install python3-tk
```

No additional packages are needed.

### Network setup for Windows and TP-Link modem

1. Connect the Arduino, both ESP32 boards and the PC to the four LAN ports of the TP-Link modem using Ethernet cables.
2. Power on the modem and each device. Make sure the link LEDs on the modem show a connection on each port.
3. Log in to the modem configuration page (usually http://192.168.1.1). If DHCP is enabled, reserve the following addresses or configure each device manually:
   - `192.168.1.50` for the Arduino Uno sender.
   - `192.168.1.60` for the Modbus ESP32 converter.
   - `192.168.1.70` for the DNP3 ESP32 converter.
   - `192.168.1.80` for the Windows PC.
4. Make sure the modem allows traffic on TCP ports **20000** and **1502**. If a firewall is active, create rules to permit these ports so the Python listener can accept connections.
5. On Windows open the network adapter settings and assign the static IP `192.168.1.80` with subnet mask `255.255.255.0` and the modem as the default gateway.
6. Install Python 3 on the PC if it is not already present. Open a command prompt in this repository and run `python python_receiver_pc.py` to start the combined GUI and console listener.
   Detailed steps are provided in `WINDOWS_PY_SETUP.md`.

With these addresses in place the converter boards will reach the PC and the Python scripts will display the traffic.
