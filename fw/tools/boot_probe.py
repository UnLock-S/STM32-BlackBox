#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  boot_probe.py —— 抓取【复位瞬间】开始的完整启动序列
# ─────────────────────────────────────────────────────────────────────────────
#  为什么需要它：
#    bootloader 的横幅只在复位后约 200 ms 内打印**一次**。
#    如果先复位、再开串口，横幅一定被错过 ✗（这不是固件问题）。
#    本工具**先打开串口开始接收**，再触发复位，因此能抓到 [boot] → App 的完整交接 ✓
#
#  用法:
#    python3 tools/boot_probe.py                # 默认 RESETPIN 复位，抓 8 秒
#    python3 tools/boot_probe.py --secs 12
#    python3 tools/boot_probe.py --port COM21 --no-reset   # 只监听，不复位
# ─────────────────────────────────────────────────────────────────────────────
import argparse
import os
import subprocess
import sys
import tempfile
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：pip install pyserial")

JLINK = r"C:\Program Files\SEGGER\JLink_V812g\JLink.exe"
BIN = r"C:\Users\zh244\Documents\UserProject\leasure\fw\bin\sbb_boot.bin"


def reset_via_jlink(use_loadbin=True):
    """复位板子。use_loadbin=True 时用『烧写式』调用，确保真的拉低 RST 脚。"""
    tmp = os.path.join(tempfile.gettempdir(), "probe_reset.jlink")
    lines = [
        "si SWD",
        "speed 4000",
        "device STM32F103C8",
        "exec SetResetType = 2",
        "connect",
    ]
    if use_loadbin:
        lines.append("loadbin {},0x08000000".format(BIN))
    lines += ["r", "g", "qc"]
    with open(tmp, "w") as f:
        f.write("\n".join(lines))
    try:
        out = subprocess.run([JLINK, "-CommanderScript", tmp],
                             capture_output=True, text=True, timeout=180).stdout
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM21")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    buf = bytearray()
    lock = threading.Lock()
    stop = threading.Event()

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    ser.reset_input_buffer()

    def reader():
        while not stop.is_set():
            try:
                data = ser.read(256)
            except Exception:
                break
            if data:
                with lock:
                    buf.extend(data)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    time.sleep(0.2)                      # 确保读线程先跑起来 ✓
    if not args.no_reset:
        reset_via_jlink()

    t_end = time.time() + args.secs
    while time.time() < t_end:
        time.sleep(0.05)

    stop.set()
    th.join(timeout=1.0)
    ser.close()

    with lock:
        raw = bytes(buf)

    print("── 抓到 {} 字节 ──".format(len(raw)))
    text = raw.decode("utf-8", "replace")
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if line.strip():
            print("  | " + line)


if __name__ == "__main__":
    main()
