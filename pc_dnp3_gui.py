"""Simple Tkinter-based viewer for frames from the Modbus/DNP3 bridge.

The GUI uses a background thread to listen on TCP port 20000.  When a
converter connects and sends data, the script analyses the bytes to see
if they match the small DNP3 wrapper used in this repository.  A short
summary is queued for display in the Tkinter window so the interface
remains responsive.
"""

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

# The converter sends one of the example Modbus frames above.  The GUI
# compares incoming data against them so it can label which command was
# received.

def is_dnp3(data: bytes) -> bool:
    """Return True if *data* appears to be a minimal DNP3 frame."""
    # Check for the start and end bytes of the simple DNP3 envelope.
    return len(data) >= 2 and data[0] == 0x05 and data[-1] == 0x16

def bits_str(data: bytes) -> str:
    """Convert a bytes object to a space separated string of bits."""
    return ' '.join(f'{b:08b}' for b in data)

def server_thread(msg_queue: queue.Queue):
    """Background thread that accepts connections and formats summaries."""
    # Runs forever accepting a connection from the converter, reading all
    # bytes sent and pushing a formatted summary onto the queue for the
    # GUI to display.
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('0.0.0.0', 20000))
    s.listen(1)
    start_time = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    msg_queue.put(f'Listener started at {start_time}\n')
    while True:
        conn, addr = s.accept()  # wait for a connection from the ESP32
        buf = b''
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
        conn.sendall(b'ACK')
        conn.close()
        # Identify whether the bytes contain the simple DNP3 frame
        proto = 'DNP3' if is_dnp3(buf) else 'Unknown'
        payload = buf[1:-1] if proto == 'DNP3' else buf
        # Check which example command we received so it can be shown in
        # the GUI.
        match = 'unknown'
        for i, cmd in enumerate(MODBUS_CMDS, 1):
            if payload == cmd:
                match = f'command {i}'
        bits = bits_str(buf)
        msg = (f'Origin: {addr[0]}\n'
               f'Bytes: {buf.hex(" ")}\n'
               f'Bits: {bits}\n'
               f'Protocol: {proto}\n'
               f'Command: {match}\n\n')
        msg_queue.put(msg)

def main():
    """Start the listening thread and run the Tkinter GUI."""
    # Queue used for passing messages from the network thread to the GUI.
    q = queue.Queue()
    th = threading.Thread(target=server_thread, args=(q,), daemon=True)
    th.start()

    root = tk.Tk()
    root.title('PC DNP3 Listener')
    text = ScrolledText(root, width=80, height=20)
    text.pack(fill=tk.BOTH, expand=True)

    def poll_queue():
        """Update the text widget with any queued messages."""
        # Retrieve all pending messages from the queue without blocking
        # and append them to the text area.
        try:
            while True:
                msg = q.get_nowait()
                text.insert(tk.END, msg)
                text.see(tk.END)
        except queue.Empty:
            pass
        # Check again after a short delay
        root.after(100, poll_queue)

    root.after(100, poll_queue)
    root.mainloop()

if __name__ == '__main__':
    main()
