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

# Example Modbus requests used by the firmware.  Each entry maps the raw
# bytes to a human readable description.
MODBUS_CMDS = {
    bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B]): "Read Holding Register",
    bytes([0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA]): "Read Input Register",
}

# Mapping of Modbus function codes to command names for frames that do not
# exactly match the examples above.
CMD_NAMES = {
    0x01: "Read Coil",
    0x02: "Read Discrete Input",
    0x03: "Read Holding Register",
    0x04: "Read Input Register",
    0x05: "Write Coil",
    0x06: "Write Register",
    0x0F: "Write Multiple Coils",
    0x10: "Write Multiple Registers",
}

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
    # bytes sent and pushing a formatted summary onto the queue for the GUI.
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("0.0.0.0", 20000))
    s.listen(1)
    start = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    msg_queue.put(("info", f"Listener started at {start}\n"))
    while True:
        conn, addr = s.accept()
        buf = b""
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
        conn.sendall(b"ACK")
        conn.close()

        timestamp = datetime.now().strftime("%H:%M:%S")
        if is_dnp3(buf):
            proto = "DNP3"
            payload = buf[1:-1]
        else:
            proto = "Modbus"
            payload = buf

        # Identify full command name using exact match first.
        cmd_name = MODBUS_CMDS.get(payload)
        if cmd_name is None and len(payload) >= 2:
            cmd_name = CMD_NAMES.get(payload[1], "Unknown")
        if cmd_name is None:
            cmd_name = "Unknown"

        summary = (
            f"Time: {timestamp} From {addr[0]}\n"
            f"Bytes: {buf.hex(' ')}\n"
            f"Bits: {bits_str(buf)}\n"
            f"Command: {cmd_name}\n\n"
        )

        msg_queue.put((proto, summary))

def main():
    """Start the listening thread and run the Tkinter GUI."""
    # Queue used for passing messages from the network thread to the GUI.
    q = queue.Queue()
    th = threading.Thread(target=server_thread, args=(q,), daemon=True)
    th.start()

    root = tk.Tk()
    root.title('Protocol Listener')

    frame_dnp = tk.LabelFrame(root, text='DNP3 Messages')
    frame_dnp.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    text_dnp = ScrolledText(frame_dnp, width=60, height=20)
    text_dnp.pack(fill=tk.BOTH, expand=True)

    frame_mb = tk.LabelFrame(root, text='Modbus Messages')
    frame_mb.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    text_mb = ScrolledText(frame_mb, width=60, height=20)
    text_mb.pack(fill=tk.BOTH, expand=True)

    def poll_queue():
        """Update the text widget with any queued messages."""
        # Retrieve all pending messages from the queue without blocking
        # and append them to the text area.
        try:
            while True:
                proto, msg = q.get_nowait()
                if proto == 'info':
                    for t in (text_dnp, text_mb):
                        t.insert(tk.END, msg)
                        t.see(tk.END)
                    continue
                if proto == 'DNP3':
                    target = text_dnp
                elif proto == 'Modbus':
                    target = text_mb
                else:
                    target = text_dnp
                target.insert(tk.END, msg)
                target.see(tk.END)
        except queue.Empty:
            pass
        # Check again after a short delay
        root.after(100, poll_queue)

    root.after(100, poll_queue)
    root.mainloop()

if __name__ == '__main__':
    main()
