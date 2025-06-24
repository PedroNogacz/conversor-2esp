import socket
import threading
import queue
from datetime import datetime
import tkinter as tk
from tkinter.scrolledtext import ScrolledText

MODBUS_CMDS = [
    bytes([0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B]),
    bytes([0x01,0x04,0x00,0x01,0x00,0x01,0x31,0xCA])
]

def is_dnp3(data: bytes) -> bool:
    return len(data) >= 2 and data[0] == 0x05 and data[-1] == 0x16

def bits_str(data: bytes) -> str:
    return ' '.join(f'{b:08b}' for b in data)

def server_thread(msg_queue: queue.Queue):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('0.0.0.0', 20000))
    s.listen(1)
    while True:
        conn, addr = s.accept()
        buf = b''
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
        conn.close()
        proto = 'DNP3' if is_dnp3(buf) else 'Unknown'
        payload = buf[1:-1] if proto == 'DNP3' else buf
        match = 'unknown'
        for i, cmd in enumerate(MODBUS_CMDS, 1):
            if payload == cmd:
                match = f'command {i}'
        bits = bits_str(buf)
        now = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        msg = (f'Time: {now} from {addr[0]}\n'
               f'Bytes: {buf.hex(" ")}\n'
               f'Bits: {bits}\n'
               f'Protocol: {proto}\n'
               f'Identified: {match}\n\n')
        msg_queue.put(msg)

def main():
    q = queue.Queue()
    th = threading.Thread(target=server_thread, args=(q,), daemon=True)
    th.start()

    root = tk.Tk()
    root.title('PC DNP3 Listener')
    text = ScrolledText(root, width=80, height=20)
    text.pack(fill=tk.BOTH, expand=True)

    def poll_queue():
        try:
            while True:
                msg = q.get_nowait()
                text.insert(tk.END, msg)
                text.see(tk.END)
        except queue.Empty:
            pass
        root.after(100, poll_queue)

    root.after(100, poll_queue)
    root.mainloop()

if __name__ == '__main__':
    main()
