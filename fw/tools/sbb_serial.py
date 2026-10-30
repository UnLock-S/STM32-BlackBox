#!/usr/bin/env python3
"""sbb_serial.py —— 驱动 / 抓取 SBB 控制台（USB-UART 转接板）

为什么需要它：现场调试时想"把控制台输出抓下来 + 批量发命令 + 结果可复现"，
比人手在 Docklight 里点更不容易漏，也便于留档。

⚠ 关键设计：**逐字节慢发**
   SBB 固件的控制台是每 20 ms 轮询一次 USART1 RXNE（刻意不动 CubeMX 的 it.c），
   整行一次性发送会在 20 ms 内到达十几个字节，硬件寄存器来不及读就被覆盖 → 丢字符。
   人类手动打字没问题（一次一个字节），但脚本/粘贴必须慢发。
   默认 --gap 30ms（≈33 字符/秒）。等固件改成 RX 中断后可以用 --gap 0。

用法：
    # 只抓 8 秒输出（看 [mon] 心跳行）
    python sbb_serial.py --port COM19 --read 8

    # 抓 2 秒 → 依次发 help / status / test，各抓 3 秒
    python sbb_serial.py --port COM19 --cmd help --cmd status --cmd test

    # 复位后从头抓启动横幅（需要 J-Link；见 tools/verify-runtime.jlink）
    python sbb_serial.py --port COM19 --read 12
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("✗ 需要 pyserial：pip install pyserial")


def dump(tag: str, buf: bytes) -> None:
    print(f"\n── {tag}：收到 {len(buf)} 字节 ──")
    if not buf:
        print("   （无数据）")
        return
    print("   HEX :", buf[:256].hex(" "))
    text = buf.decode("utf-8", "replace").replace("\r", "")
    for line in text.split("\n"):
        if line.strip():
            print("   |", line.rstrip())


def read_for(port, seconds: float) -> bytes:
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        n = port.in_waiting
        if n:
            buf += port.read(n)
        else:
            time.sleep(0.02)
    return bytes(buf)


def send_slow(port, text: str, gap: float) -> None:
    for ch in text:
        port.write(ch.encode())
        port.flush()
        if gap > 0:
            time.sleep(gap)


def main() -> int:
    ap = argparse.ArgumentParser(description="SBB 串口控制台驱动/抓取")
    ap.add_argument("--port", default="COM19", help="串口号（默认 COM19）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--read", type=float, default=0.0, help="只读 N 秒后退出")
    ap.add_argument("--cmd", action="append", default=[], help="要发的命令（可多次）")
    ap.add_argument("--intro", type=float, default=2.0, help="发命令前先读 N 秒（默认 2）")
    ap.add_argument("--post", type=float, default=3.0, help="每条命令后读 N 秒（默认 3）")
    ap.add_argument("--gap", type=float, default=0.0,
                    help="逐字节间隔秒。固件已改 RX 中断模式，默认 0（快发）；"
                         "若用旧固件（20ms 轮询）需设 0.03")
    ap.add_argument("--settle", type=float, default=0.4,
                    help="打开端口后先等 N 秒（默认 0.4）")
    args = ap.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.3)
    except Exception as exc:                       # noqa: BLE001
        print(f"✗ 打开 {args.port} 失败：{exc}", file=sys.stderr)
        print("  若 Docklight / 串口助手 正打开该端口，先关掉（串口独占）",
              file=sys.stderr)
        return 1

    print(f"✅ 已打开 {args.port} {args.baud} 8N1  逐字节间隔 {args.gap*1000:.0f} ms")
    try:
        # ★ 实测（2026-10-14）：打开端口后若立刻发命令，**第一条命令整条丢失**
        #   （连回显都没有）。Windows 串口刚打开时 TX 通路尚未稳定。
        #   对策：先静置 settle 秒，再发一个空回车把通路"唤醒"并丢掉它的响应。
        time.sleep(args.settle)
        send_slow(port, "\r", args.gap)
        time.sleep(0.3)
        read_for(port, 0.3)                        # 丢弃唤醒回车产生的响应

        if args.read > 0 and not args.cmd:
            dump(f"读取 {args.read}s", read_for(port, args.read))
            return 0

        dump(f"发命令前先读 {args.intro}s", read_for(port, args.intro))

        for cmd in args.cmd:
            print(f"\n>> 发送命令 (慢发): {cmd!r}")
            send_slow(port, cmd + "\r", args.gap)
            dump(f"'{cmd}' 的响应（读 {args.post}s）", read_for(port, args.post))
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
