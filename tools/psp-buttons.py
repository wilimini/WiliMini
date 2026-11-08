#!/usr/bin/env python3
"""Press PSP buttons through PSPLink's remotejoy (button control only, no screen).

Needs remotejoy.prx loaded on the PSP and usbhostfs_pc running (it relays TCP
port 10004). Protocol: tools/remotejoy/remotejoy.h in psplink (JoyEvent).
Usage: psp-buttons.py [-H host] [-p port] [-t seconds] BUTTON...
  BUTTON is start, select, up, down, left, right, l, r, triangle, circle, cross, square.
"""
import argparse, socket, struct, time

MAGIC, DOWN, UP = 0x909ACCEF, 1, 2
BUTTONS = {"select": 0x1, "start": 0x8, "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
           "l": 0x100, "r": 0x200, "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000}

def main():
    a = argparse.ArgumentParser()
    a.add_argument("-H", "--host", default="127.0.0.1")
    a.add_argument("-p", "--port", type=int, default=10004)
    a.add_argument("-t", "--hold", type=float, default=0.15, help="seconds each press is held")
    a.add_argument("buttons", nargs="+", choices=sorted(BUTTONS))
    args = a.parse_args()
    with socket.create_connection((args.host, args.port), timeout=5) as s:
        for name in args.buttons:
            v = BUTTONS[name]
            s.sendall(struct.pack("<III", MAGIC, DOWN, v))
            time.sleep(args.hold)
            s.sendall(struct.pack("<III", MAGIC, UP, v))
            time.sleep(0.15)

if __name__ == "__main__":
    main()
