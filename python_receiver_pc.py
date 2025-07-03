"""Combined GUI and console listener for Modbus/DNP3 frames.

This script merges the functionality of the old ``pc_dnp3_gui.py`` and
``pc_dnp3_listener.py`` utilities.  It now listens on two ports so it can
receive DNP3 data from the DNP3 ESP32 and Modbus data from the Modbus
ESP32 simultaneously.  Port ``20000`` is used for DNP3 while port ``1502``
captures Modbus frames.  All traffic is displayed both in a Tkinter window
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
from tkinter import ttk
from tkinter.scrolledtext import ScrolledText

# Example Modbus requests used by the firmware. Each entry maps the
# raw bytes to a human readable description.
MODBUS_CMDS = {
    bytes([0x01,0x03,0x00,0x0A,0x00,0x02,0xE4,0x09]): "Read Holding Registers",
    bytes([0x01,0x04,0x00,0x0A,0x00,0x02,0x51,0xC9]): "Read Input Registers",
    bytes([0x01,0x05,0x00,0x13,0xFF,0x00,0x7D,0xFF]): "Write Single Coil",
    bytes([0x01,0x02,0x00,0x00,0x00,0x08,0x79,0xCC]): "Read Input Status",
    bytes([0x01,0x10,0x00,0x01,0x00,0x02,0x04,0x00,0x0A,0x00,0x14,0x12,0x6E]): "Write Multiple Registers",
}


DNP3_CMDS = {
    bytes([0xC0,0x01,0x01,0x02,0x00,0x00]): "Read Binary Inputs",
    bytes([0xC0,0x01,0x30,0x02,0x00,0x00]): "Read Analog Inputs",
    bytes([0xC1,0x05,0x0C,0x01,0x17,0x00,0x00]): "Control Relay Output Block",
    bytes([0xC1,0x05,0x29,0x01,0x00,0x64,0x00,0x00]): "Operate Analog Output",
    # Responses returned by the ESP32 DNP3 converter
    bytes([0x80,0x81,0x01,0x02,0x00,0x01,0x01]): "Binary Inputs Response",
    bytes([0x80,0x81,0x30,0x02,0x00,0x01,0x0A,0x00]): "Analog Inputs Response",
    bytes([0x81,0x00]): "Control Relay Response",
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


# Counters used to track how many messages were recognized versus
# reported as "Unknown" for each protocol.  These are updated by the
# server threads and read by the Tkinter GUI.
stats = {
    "DNP3": {"total": 0, "unknown": 0},
    "Modbus": {"total": 0, "unknown": 0},
}
stats_lock = threading.Lock()

# Keep a running tally of how many times each command was seen for
# both protocols.  This allows writing a summary to disk so the user
# can review the overall traffic after the listener has been running.
command_counts = {
    "DNP3": {},
    "Modbus": {},
}


def write_archive() -> None:
    """Write the current command counts to ``command_archive.txt``."""
    lines: list[str] = []
    for proto, counts in command_counts.items():
        lines.append(f"{proto} commands:\n")
        for name, count in sorted(counts.items()):
            lines.append(f"  {name}: {count}\n")
        lines.append("\n")
    with open("command_archive.txt", "w", encoding="utf-8") as fh:
        fh.writelines(lines)


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




def server_thread(port: int, proto: str, msg_queue: queue.Queue) -> None:
    """Accept connections on *port* and queue summaries for the GUI."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("0.0.0.0", port))
    s.listen(1)

    # Step 2: notify the GUI that we started listening.
    start = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    info = f"Listening on port {port} ({proto}) at {start}\n"
    msg_queue.put(("info", info))
    print(info.strip())

    # Step 3: wait for incoming connections forever.
    while True:
        conn, addr = s.accept()
        print(f"Connection on {port} from {addr[0]}")
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
        if proto == "DNP3":
            if is_dnp3(buf):
                payload = buf[1:-1]
                print("Frame appears to be DNP3")
            else:
                payload = buf
                print("Frame missing DNP3 markers")
        else:  # Modbus port
            payload = buf

        # Step 7: look up a friendly name for the command.
        if proto == "DNP3":
            cmd_name = DNP3_CMDS.get(payload)
        else:
            cmd_name = MODBUS_CMDS.get(payload)
        if cmd_name is None and len(payload) >= 2:
            cmd_name = CMD_NAMES.get(payload[1], "Unknown")
        if cmd_name is None:
            cmd_name = "Unknown"

        # Update statistics and command counts for this protocol.
        with stats_lock:
            stats[proto]["total"] += 1
            if cmd_name == "Unknown":
                stats[proto]["unknown"] += 1
            counts = command_counts[proto]
            counts[cmd_name] = counts.get(cmd_name, 0) + 1
            write_archive()

        # Step 8: build a message that will appear in the GUI and console.
        summary = (
            f"Time: {timestamp} From {addr[0]} Port {port}\n"
            f"Bytes: {buf.hex(' ')}\n"
            f"Bits: {bits_str(buf)}\n"
            f"Command: {cmd_name}\n\n"
        )

        msg_queue.put((proto, summary))  # send to GUI
        msg_queue.put(("stats", None))  # update GUI statistics
        print(summary, end="")          # echo to console


def main() -> None:
    """Start the listener thread and run the Tkinter GUI."""
    # Create a queue that the network thread will use to pass
    # messages to the GUI.
    q: queue.Queue = queue.Queue()
    # Launch background threads that listen on the Modbus and DNP3 ports.
    for port, proto in ((20000, "DNP3"), (1502, "Modbus")):
        th = threading.Thread(target=server_thread, args=(port, proto, q), daemon=True)
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

    # Frame at the bottom showing recognition statistics for each protocol.
    frame_stats = tk.LabelFrame(root, text="Recognition Stats")
    frame_stats.pack(side=tk.BOTTOM, fill=tk.X)

    label_dnp = tk.Label(frame_stats, text="DNP3 recognized: 0/0 (0% OK)")
    label_dnp.pack(fill=tk.X, padx=5)
    progress_dnp = ttk.Progressbar(frame_stats, maximum=100)
    progress_dnp.pack(fill=tk.X, padx=5)

    label_mb = tk.Label(frame_stats, text="Modbus recognized: 0/0 (0% OK)")
    label_mb.pack(fill=tk.X, padx=5, pady=(5,0))
    progress_mb = ttk.Progressbar(frame_stats, maximum=100)
    progress_mb.pack(fill=tk.X, padx=5)

    def update_stats() -> None:
        """Refresh progress bars and labels with current statistics."""
        with stats_lock:
            d_total = stats["DNP3"]["total"]
            d_unknown = stats["DNP3"]["unknown"]
            m_total = stats["Modbus"]["total"]
            m_unknown = stats["Modbus"]["unknown"]

        def calc_text(total: int, unknown: int, proto: str) -> tuple[str, float]:
            if total:
                ok = total - unknown
                pct = ok / total * 100
            else:
                ok = 0
                pct = 0.0
            text = f"{proto} recognized: {ok}/{total} ({pct:.0f}% OK)"
            return text, pct

        text, pct = calc_text(d_total, d_unknown, "DNP3")
        label_dnp.config(text=text)
        progress_dnp['value'] = pct

        text, pct = calc_text(m_total, m_unknown, "Modbus")
        label_mb.config(text=text)
        progress_mb['value'] = pct

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
                if proto == "stats":
                    update_stats()
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

    update_stats()
    root.after(100, poll_queue)
    root.mainloop()


if __name__ == "__main__":
    main()
