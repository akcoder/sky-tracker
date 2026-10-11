#!/usr/bin/env python3
"""Drive the unit without a finger and read what the screen cost (PERF-19/20).

It opens the unit's serial port ONCE (the USB-serial chip resets the board when a port is opened, so the board
boots as the script starts), then sends the commands of a script one line at a time over UART0's receive pin and
reads the unit's log from its web server (/events), which works whatever the console is set to.

  tools/dbg.py [--port /dev/cu.usbserial-21330] [--baud 921600] [--host 192.168.0.217] [--boot 80] script.txt

Script lines:  wait <seconds>   |   # comment   |   anything else is sent as a command:
  perf | reset | close | open <sun|moon|earth|region|uaf|planet N> | page <sky|list|settings|about|debug>
After each "perf" the new perf / dbg log lines are printed under the line that asked for them.
Needs pyserial (the ESPHome venv has it)."""
import argparse, re, sys, threading, time, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("script")
ap.add_argument("--port", default="/dev/cu.usbserial-21330")
ap.add_argument("--baud", type=int, default=921600)
ap.add_argument("--host", default="192.168.0.217")
ap.add_argument("--boot", type=float, default=80, help="seconds to let the board boot after the port is opened")
ap.add_argument("--no-reset", action="store_true", help="do not wait for a boot (the port is opened but the board is already up)")
a = ap.parse_args()

import serial

lines, lock = [], threading.Lock()
stop = False


def sse():
    while not stop:
        try:
            r = urllib.request.urlopen(f"http://{a.host}/events", timeout=30)
            for raw in r:
                if stop:
                    return
                t = re.sub(r"\x1b\[[0-9;]*m", "", raw.decode("utf-8", "replace").rstrip())
                if t.startswith("data: [") and ("perf" in t or "dbg" in t or "took a long time" in t):
                    with lock:
                        lines.append((time.time(), t[6:]))
        except Exception:
            time.sleep(2)


threading.Thread(target=sse, daemon=True).start()
s = serial.Serial()
s.port, s.baudrate, s.timeout = a.port, a.baud, 0.2
s.dtr = s.rts = False
s.open()
print(f"opened {a.port}; waiting {a.boot:.0f} s for the board to boot", flush=True)
time.sleep(0 if a.no_reset else a.boot)
seen = 0


def new_lines():
    global seen
    with lock:
        out = lines[seen:]
        seen = len(lines)
    return out


new_lines()
for raw in open(a.script):
    cmd = raw.strip()
    if not cmd:
        continue
    if cmd.startswith("#"):
        print(cmd, flush=True)
        continue
    if cmd.startswith("wait "):
        time.sleep(float(cmd.split()[1]))
        for t, l in new_lines():
            if "perf: a refresh took" in l or "took a long time" in l:
                print("    ", l[:200], flush=True)
        continue
    print(f"> {cmd}", flush=True)
    s.write((cmd + "\n").encode())
    s.flush()
    time.sleep(1.5 if cmd == "perf" else 0.6)
    for t, l in new_lines():
        print("    ", l[:230], flush=True)
stop = True
s.close()
