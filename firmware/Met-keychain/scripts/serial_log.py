#!/usr/bin/env python3
"""Reconnect-tolerant serial logger for reboot-loop diagnosis.

Keeps reopening the port so a board that resets (USB CDC re-enumerates)
still yields boot logs, including reset_reas from setup()/loop().
Usage: python scripts/serial_log.py [PORT] [SECONDS]
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM10"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

start = time.time()
while time.time() - start < DURATION:
    try:
        with serial.Serial(PORT, 115200, timeout=1) as s:
            print(f"--- opened {PORT} ---", flush=True)
            while time.time() - start < DURATION:
                try:
                    line = s.readline().decode("utf-8", "replace").rstrip()
                    if line:
                        print(line, flush=True)
                except serial.SerialException as e:
                    print(f"--- drop: {e} ---", flush=True)
                    break
    except serial.SerialException as e:
        print(f"--- open failed: {e}; retry ---", flush=True)
        time.sleep(0.3)
