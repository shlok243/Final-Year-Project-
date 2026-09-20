"""
CSI Presence Dashboard — backend
Reads ESP32 CSI serial stream, computes rolling RF energy, and serves
a live dashboard over HTTP (no external deps beyond pyserial).

Install: pip install pyserial
Run:     python server.py --port COM6
Then open: http://localhost:8000
"""

import argparse
import collections
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

# ---------- Shared state (updated by serial-reading thread) ----------
state_lock = threading.Lock()
state = {
    "rf_energy": 0.0,
    "threshold": 5.0,
    "signal_dbm": 0,
    "session_peak": 0.0,
    "trace": collections.deque(maxlen=40),   # last 40 points for the sparkline
    "baseline_std": None,
    "calibrating": False,
}

window = collections.deque(maxlen=50)


def serial_reader(port, baud):
    ser = serial.Serial(port, baud, timeout=1)
    while True:
        try:
            line = ser.readline().decode(errors="ignore").strip()
        except Exception:
            continue
        if not line.startswith("CSI,"):
            continue
        parts = line.split(",")
        if len(parts) != 4:
            continue
        try:
            rssi = int(parts[2])
            amp = float(parts[3])
        except ValueError:
            continue

        window.append(amp)
        if len(window) < 10:
            continue

        mean = sum(window) / len(window)
        variance = sum((x - mean) ** 2 for x in window) / len(window)
        energy = variance ** 0.5  # std dev as "RF energy" reading

        with state_lock:
            state["rf_energy"] = round(energy, 1)
            state["signal_dbm"] = rssi
            state["trace"].append(round(energy, 2))
            if energy > state["session_peak"]:
                state["session_peak"] = round(energy, 1)


def calibrate_baseline(seconds=5):
    with state_lock:
        state["calibrating"] = True
    samples = []
    t0 = time.time()
    while time.time() - t0 < seconds:
        with state_lock:
            samples.append(state["rf_energy"])
        time.sleep(0.1)
    if samples:
        baseline = sum(samples) / len(samples)
        # new_threshold = max(baseline * 3, 3.0)
        new_threshold = max(baseline * 2, 2.0)
        with state_lock:
            state["threshold"] = round(new_threshold, 1)
            state["session_peak"] = 0.0
    with state_lock:
        state["calibrating"] = False


DASHBOARD_HTML = None  # loaded from disk at startup


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silence default logging

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(DASHBOARD_HTML.encode("utf-8"))
        elif self.path == "/data":
            with state_lock:
                payload = {
                    "rf_energy": state["rf_energy"],
                    "threshold": state["threshold"],
                    "signal_dbm": state["signal_dbm"],
                    "session_peak": state["session_peak"],
                    "trace": list(state["trace"]),
                    "calibrating": state["calibrating"],
                }
            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path == "/calibrate":
            threading.Thread(target=calibrate_baseline, daemon=True).start()
            self.send_response(200)
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()


def main():
    global DASHBOARD_HTML
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--http-port", type=int, default=8000)
    args = parser.parse_args()

    with open("dashboard.html", "r", encoding="utf-8") as f:
        DASHBOARD_HTML = f.read()

    t = threading.Thread(target=serial_reader, args=(args.port, args.baud), daemon=True)
    t.start()

    httpd = ThreadingHTTPServer(("0.0.0.0", args.http_port), Handler)
    print(f"Dashboard running at http://localhost:{args.http_port}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
