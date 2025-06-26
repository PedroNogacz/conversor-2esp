"""Command-line listener for frames from the Modbus/DNP3 bridge.

This small utility opens TCP port 20000 (the port used by the ESP32
converters) and prints every frame received.  It displays the raw byte
sequence, a bit string and whether the payload matches one of the
example Modbus commands wrapped inside the DNP3 envelope used by this
project.
"""

import socket
from datetime import datetime

MODBUS_CMDS = [
    bytes([0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B]),
    bytes([0x01,0x04,0x00,0x01,0x00,0x01,0x31,0xCA])
]

# Frames sent by the Arduino match one of the two entries above.  The
# listener checks incoming data against these patterns so it can print
# which example command was observed.

def is_dnp3(data: bytes) -> bool:
    """Return True if *data* looks like a DNP3 frame."""
    # The minimal wrapper used by the ESP32 converters starts with
    # 0x05 and ends with 0x16.  A proper implementation would perform
    # CRC checks and more validation but this is sufficient for the
    # example.
    return len(data) >= 2 and data[0] == 0x05 and data[-1] == 0x16

def print_bits(data: bytes):
    """Print a binary representation of the received bytes."""
    # Helpful for confirming the byte order and content when debugging
    # the converter firmware.
    print('bits:', ' '.join(f'{b:08b}' for b in data))


def main():
    """Run the blocking command-line listener."""
    # Listen for a single connection at a time on all network interfaces.
    # Each connection is read in full and then closed before the next one
    # is accepted.
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('0.0.0.0', 20000))
    s.listen(1)
    print('PC listener ready on port 20000')
    while True:
        conn, addr = s.accept()  # wait for the converter to connect
        print('Connection from', addr, 'at', datetime.now())
        buf = b''
        # Read until the peer closes the connection
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
        conn.close()
        print('Received bytes:', buf.hex(' '))
        print_bits(buf)
        # Determine whether the bytes include the simple DNP3 framing
        # used by this project.  If so, strip the first and last byte
        # to obtain the Modbus payload.
        if is_dnp3(buf):
            print('Frame appears to be DNP3')
            payload = buf[1:-1]
        else:
            print('Frame is not valid DNP3')
            payload = buf
        # Compare against our known example commands so we can print
        # which one was received.
        match = 'unknown'
        for i, cmd in enumerate(MODBUS_CMDS, 1):
            if payload == cmd:
                match = f'command {i}'
        print('Identified', match)

if __name__ == '__main__':
    main()
