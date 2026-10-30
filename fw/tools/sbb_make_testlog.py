#!/usr/bin/env python3
"""sbb_make_testlog.py —— 生成合成的 SBB 日志镜像，用于验证 tools/sbb_log2csv.py。

为什么需要它：真实的 SD 卡日志要等硬件接线后才能产生。解析脚本本身**可以**
先用合成数据验证正确性 —— 这样明天拿到真数据时，唯一的变量只剩硬件。

用法:
    python sbb_make_testlog.py out.img [--blocks 3]
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

MAGIC = b"SBBLOG01"
RECORD_SIZE = 20
RECORDS_PER_BLOCK = 512 // RECORD_SIZE      # 25
FIRST_LBA = 1


def make_record(ts_ms: int, rtype: int, ch: int, flags: int, payload: bytes) -> bytes:
    assert len(payload) <= 12, "payload 超过 12 B"
    return struct.pack("<IBBBB", ts_ms, rtype, ch, flags, len(payload)) + payload.ljust(12, b"\x00")


def make_header() -> bytes:
    hdr = bytearray(512)
    hdr[0:8] = MAGIC
    hdr[8] = 0x01
    hdr[9] = 0x00
    struct.pack_into("<H", hdr, 10, RECORD_SIZE)
    struct.pack_into("<I", hdr, 12, FIRST_LBA)
    return bytes(hdr)


def can_payload(can_id: int, data: bytes) -> bytes:
    return struct.pack("<I", can_id) + data


def build(blocks: int) -> bytes:
    out = bytearray()
    out += make_header()

    ts = 1000
    made = 0
    for _b in range(blocks):
        blk = bytearray(512)
        for i in range(RECORDS_PER_BLOCK):
            ts += 10
            kind = made % 5
            if kind == 0:
                rec = make_record(ts, 0x01, 0, 0, bytes([(made * 7) % 256]))
            elif kind == 1:
                rec = make_record(ts, 0x02, 0, 0, can_payload(0x123, bytes([1, 2, 3, 4, 5, 6, 7, 8])))
            elif kind == 2:
                rec = make_record(ts, 0x03, 0, 0, can_payload(0x18FF50E5, bytes([0xAA, 0xBB])))
            elif kind == 3:
                rec = make_record(ts, 0x04, 0, 0, struct.pack("<II", made, 0xDEADBEEF))
            else:
                rec = make_record(ts, 0x05, 0, 0, struct.pack("<II", 0x08001000, 0x00001234))
            blk[i * RECORD_SIZE:(i + 1) * RECORD_SIZE] = rec
            made += 1
        out += blk

    # 尾部补一块"未写过"（全 0）用于验证解析器会在此停止
    out += bytes(512)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成合成 SBB 日志镜像")
    ap.add_argument("out", type=Path)
    ap.add_argument("--blocks", type=int, default=3)
    args = ap.parse_args()

    data = build(args.blocks)
    args.out.write_bytes(data)
    print(f"✓ 已生成 {args.out}  ({len(data)} B, {args.blocks} 个记录块 "
          f"× {RECORDS_PER_BLOCK} 条 = {args.blocks * RECORDS_PER_BLOCK} 条记录)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
