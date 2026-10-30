#!/usr/bin/env python3
"""长时间轮询 C0832C 双通道读数并记录成 CSV。

用途：「拧旋钮看曲线」这类需要人手配合的动态验证 —— 后台跑，人同时操作。

用法:
    python3 adc_sweep_log.py [秒数=60] [间隔秒=1.0] [输出 csv=adc_sweep.csv]

注意:
- 用 python3 / py 运行（Hermes 自带 venv 的 python 没装 pyserial）
- 串口自动按 FTDI VID:PID(0403:6001) 查找，插不同 USB 口换了口号也能用
- 命令逐字节慢发（30 ms/字节）并用 \\r 唤醒：打开端口后的第一条命令会被整条丢掉，
  且 USB-UART 突发会把多字节合并送出（F1 的 USART 只有 1 字节接收寄存器）
"""
import sys
import re
import time

import serial
from serial.tools import list_ports


def find_ftdi_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) == (0x0403, 0x6001):
            return p.device
    return None


def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    iv = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    out = sys.argv[3] if len(sys.argv) > 3 else "adc_sweep.csv"

    port = find_ftdi_port()
    if port is None:
        print("找不到 FTDI 串口（VID 0403:PID 6001）")
        return 1

    p = serial.Serial(port, 115200, timeout=0.1)
    print(f"已打开 {port}，采样 {dur:.0f}s，间隔 {iv:.2f}s -> {out}", flush=True)
    time.sleep(0.5)
    p.write(b"\r")
    p.flush()
    time.sleep(0.6)
    p.read(p.in_waiting)

    t0 = time.time()
    n = 0
    with open(out, "w", encoding="utf-8") as f:
        f.write("t_s,ch0,ch1\n")
        while time.time() - t0 < dur:
            p.read(p.in_waiting)
            for ch in b"adc\r":
                p.write(bytes([ch]))
                p.flush()
                time.sleep(0.03)

            tb = time.time()
            buf = bytearray()
            # 读窗口给足 3 秒：命令本身很快，但显示任务(I2C 刷屏)会抢占 console 任务，
            # 响应可能被推迟；窗口太短会误判成"无响应"（本项目踩过）。
            while time.time() - tb < 3.0:
                k = p.in_waiting
                if k:
                    buf += p.read(k)
                time.sleep(0.02)

            txt = buf.decode("utf-8", "replace")
            m = re.search(r"CH0=(\d+)\s+CH1=(\d+)", txt)
            ts = time.time() - t0
            if m:
                f.write(f"{ts:.1f},{m.group(1)},{m.group(2)}\n")
                f.flush()
                print(f"t={ts:6.1f}s  CH0={m.group(1):>3}  CH1={m.group(2):>3}", flush=True)
                n += 1
            else:
                # 把原始回显打出来，避免"无响应"这种无法诊断的输出（本项目踩过）
                raw = " | ".join(l.strip() for l in txt.replace("\r", "").split("\n") if l.strip())
                print(f"t={ts:6.1f}s  ✗ 无匹配  raw={raw[:120]!r}", flush=True)

            time.sleep(max(0.0, iv - (time.time() - tb)))

    p.close()
    print(f"\n完成: {n} 个样本 -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
