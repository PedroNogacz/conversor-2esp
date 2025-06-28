"""Combined GUI and console listener for Modbus/DNP3 frames.

This script merges the functionality of ``pc_dnp3_gui.py`` and
``pc_dnp3_listener.py``. It opens TCP port 20000 and displays
all frames received from the converter both in a Tkinter window
and on the command line.

Steps to run on Windows:
1. Install Python 3 from https://python.org.
2. Open ``cmd`` and ``cd`` into this repository.
3. Run ``python python_receiver_pc.py``.
4. Allow access through the firewall when Windows asks.
5. The script prints connection info in the console and opens a
   window with two panes (one for DNP3 and one for Modbus).

Messages are separated into Modbus and DNP3 panes in the GUI while
the console output mirrors the information printed in the original
listener.
"""

import socket
import threading
import queue
from datetime import datetime
import tkinter as tk
from tkinter.scrolledtext import ScrolledText

# Example Modbus requests used by the firmware. Each entry maps the
# raw bytes to a human readable description.
MODBUS_CMDS = {
    bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B]): "Read Holding Register",
    bytes([0x01, 0x04, 0x00, 0x01, 0x00, 0x01, 0x31, 0xCA]): "Read Input Register",
}

# Mapping of Modbus function codes to command names for frames that do
# not exactly match the examples above.
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
    # DNP3 frames begin with 0x05 and end with 0x16 in this
    # simplified example. Any data matching that pattern is
    # treated as DNP3 rather than raw Modbus.
    return len(data) >= 2 and data[0] == 0x05 and data[-1] == 0x16


def bits_str(data: bytes) -> str:
    """Convert bytes to a space separated bit string."""
    # Useful for debugging to see the individual bits of a frame.
    return " ".join(f"{b:08b}" for b in data)


def server_thread(msg_queue: queue.Queue) -> None:
    """Accept connections and queue summaries for the GUI."""
    # Step 1: create a TCP socket and listen on port 20000.
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("0.0.0.0", 20000))  # bind on all interfaces
    s.listen(1)

    # Step 2: notify the GUI that we started listening.
    start = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    info = f"Listener started at {start}\n"
    msg_queue.put(("info", info))
    print(info.strip())

    # Step 3: wait for incoming connections forever.
    while True:
        conn, addr = s.accept()
        print(f"Connection from {addr[0]}")
        buf = b""
        # Step 4: read all data sent by the client.
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buf += chunk
        # Step 5: acknowledge and close the connection.
        conn.sendall(b"ACK")
        conn.close()

        # Step 6: decode the received bytes.
        timestamp = datetime.now().strftime("%H:%M:%S")
        if is_dnp3(buf):
            proto = "DNP3"
            payload = buf[1:-1]
            print("Frame appears to be DNP3")
        else:
            proto = "Modbus"
            payload = buf
            print("Frame is not valid DNP3")

        # Step 7: look up a friendly name for the command.
        cmd_name = MODBUS_CMDS.get(payload)
        if cmd_name is None and len(payload) >= 2:
            cmd_name = CMD_NAMES.get(payload[1], "Unknown")
        if cmd_name is None:
            cmd_name = "Unknown"

        # Step 8: build a message that will appear in the GUI and console.
        summary = (
            f"Time: {timestamp} From {addr[0]}\n"
            f"Bytes: {buf.hex(' ')}\n"
            f"Bits: {bits_str(buf)}\n"
            f"Command: {cmd_name}\n\n"
        )

        msg_queue.put((proto, summary))  # send to GUI
        print(summary, end="")          # echo to console


def main() -> None:
    """Start the listener thread and run the Tkinter GUI."""
    # Create a queue that the network thread will use to pass
    # messages to the GUI.
    q: queue.Queue = queue.Queue()
    # Launch the background thread that listens for incoming frames.
    th = threading.Thread(target=server_thread, args=(q,), daemon=True)
    th.start()

    # Step 1: create the main Tk window.
    root = tk.Tk()
    root.title("Protocol Listener")

    # Step 2: build the DNP3 text area on the left.
    frame_dnp = tk.LabelFrame(root, text="DNP3 Messages")
    frame_dnp.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    text_dnp = ScrolledText(frame_dnp, width=60, height=20)
    text_dnp.pack(fill=tk.BOTH, expand=True)

    # Step 3: build the Modbus text area on the right.
    frame_mb = tk.LabelFrame(root, text="Modbus Messages")
    frame_mb.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    text_mb = ScrolledText(frame_mb, width=60, height=20)
    text_mb.pack(fill=tk.BOTH, expand=True)

    def poll_queue() -> None:
        """Update text widgets with any queued messages."""
        try:
            while True:
                proto, msg = q.get_nowait()
                if proto == "info":
                    for t in (text_dnp, text_mb):
                        t.insert(tk.END, msg)
                        t.see(tk.END)
                    continue
                if proto == "DNP3":
                    target = text_dnp
                else:
                    target = text_mb
                target.insert(tk.END, msg)
                target.see(tk.END)
        except queue.Empty:
            pass
        root.after(100, poll_queue)

    root.after(100, poll_queue)
    root.mainloop()


if __name__ == "__main__":
    main()
