import socket
from datetime import datetime

MODBUS_CMDS = [
    bytes([0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B]),
    bytes([0x01,0x04,0x00,0x01,0x00,0x01,0x31,0xCA])
]

def is_dnp3(data: bytes) -> bool:
    return len(data) >= 2 and data[0] == 0x05 and data[-1] == 0x16

def print_bits(data: bytes):
    print('bits:', ' '.join(f'{b:08b}' for b in data))


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('0.0.0.0', 20000))
    s.listen(1)
    print('PC listener ready on port 20000')
    while True:
        conn, addr = s.accept()
        print('Connection from', addr, 'at', datetime.now())
        buf = b''
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
        conn.close()
        print('Received bytes:', buf.hex(' '))
        print_bits(buf)
        if is_dnp3(buf):
            print('Frame appears to be DNP3')
            payload = buf[1:-1]
        else:
            print('Frame is not valid DNP3')
            payload = buf
        match = 'unknown'
        for i, cmd in enumerate(MODBUS_CMDS, 1):
            if payload == cmd:
                match = f'command {i}'
        print('Identified', match)

if __name__ == '__main__':
    main()
