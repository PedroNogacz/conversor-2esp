# Windows and Python Setup Guide

This short guide explains how to configure a Windows PC and run the listener
script included in this repository.

## Network configuration
1. Connect the Arduino, both ESP32 boards and the PC to the TP-Link modem
   using Ethernet cables.
2. Power on all devices and confirm the modem link LEDs are active.
3. Log in to the modem (`http://192.168.1.1`) and assign static addresses:
   - `192.168.1.50` – Arduino Uno sender
   - `192.168.1.60` – Modbus ESP32
   - `192.168.1.70` – DNP3 ESP32
   - `192.168.1.80` – Windows PC
4. Ensure TCP port `20000` is allowed through the modem firewall.
5. On Windows set the PC adapter to `192.168.1.80` with subnet mask
   `255.255.255.0` and the modem as the default gateway.

## Running the listener
1. Install Python 3 if it is not already present.
2. Open a command prompt in this repository directory.
3. Launch the GUI listener with:
   ```
   python pc_dnp3_gui.py
   ```
   Two panes will show incoming Modbus and DNP3 messages separately.
