#!/usr/bin/env python3
"""sbb_log2csv.py —— 把 SBB 黑匣子的 SD 原始日志镜像转成 CSV。

用法：
    python sbb_log2csv.py <镜像文件> [-o out.csv]

镜像怎么来（任选）：
    · 读卡器直接 dd 整卡： dd if=\\\\.\\PhysicalDriveN of=sdcard.img bs=512
    · 或只导出前若干 MB（日志从 LBA 1 起，留出尾部预留区之外的都有效）
    · 或用本仓库 tools/ 里后续可加的 J-Link/调试读取脚本

格式（与 App/app/app_log.h 对齐）：
    块 0        : 头 magic "SBBLOG01" + 版本 + 记录尺寸 + 记录区起始 LBA
    块 1..N     : 每块 25 条 20 B 记录
    单条记录 20 B:
        +0 u32 ts_ms   +4 u8 type  +5 u8 ch  +6 u8 flags  +7 u8 len  +8 u8[12] payload
    payload: ADC=[0]值 ; CAN=[0..3]id(LE)+[4..]data ; EVENT/OTA=[0..3]v0+[4..7]v1
    类型: 1=ADC 2=CAN_RX 3=CAN_TX 4=EVENT 5=OTA
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from pathlib import Path

MAGIC = b"SBBLOG01"
RECORD_SIZE = 20
RECORDS_PER_BLOCK = 512 // RECORD_SIZE
MAX_LBA_SCAN = 64 * 1024          # 最多扫 32 MB，防止整卡扫太久

TYPE_NAMES = {
    0x01: "ADC",
    0x02: "CAN_RX",
    0x03: "CAN_TX",
    0x04: "EVENT",
    0x05: "OTA",
}


def u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def parse_header(block0: bytes) -> dict:
    if block0[:8] != MAGIC:
        raise ValueError(
            f"块 0 没有找到 magic {MAGIC!r}（读到 {block0[:8]!r}）—— "
            "镜像不是 SBB 日志，或没有从头开始导出"
        )
    return {
        "ver": f"{block0[8]}.{block0[9]}",
        "record_size": block0[10] | (block0[11] << 8),
        "first_lba": u32(block0, 12),
    }


def is_all_ff(block: bytes) -> bool:
    return block == b"\xff" * len(block)


def is_all_zero(block: bytes) -> bool:
    return block == b"\x00" * len(block)


def iter_records(data: bytes, first_lba: int):
    """按块扫描，产出未使用（全 0）的尾部即停止。"""
    total_lba = len(data) // 512
    for lba in range(first_lba, min(total_lba, first_lba + MAX_LBA_SCAN)):
        blk = data[lba * 512:(lba + 1) * 512]
        if is_all_zero(blk) or is_all_ff(blk):
            if is_all_zero(blk):
                break              # 未写过的区域 → 后面也没有了
            continue               # 擦除态（0xFF）跳过
        for i in range(RECORDS_PER_BLOCK):
            rec = blk[i * RECORD_SIZE:(i + 1) * RECORD_SIZE]
            if is_all_zero(rec) or is_all_ff(rec):
                continue
            yield lba, i, rec


def record_to_row(lba: int, idx: int, rec: bytes) -> dict:
    ts_ms = u32(rec, 0)
    rtype = rec[4]
    ch = rec[5]
    flags = rec[6]
    length = min(rec[7], 12)
    payload = rec[8:8 + length]

    row = {
        "lba": lba,
        "idx": idx,
        "ts_ms": ts_ms,
        "type_id": rtype,
        "type": TYPE_NAMES.get(rtype, f"UNKNOWN_{rtype:#04x}"),
        "ch": ch,
        "flags": f"0x{flags:02x}",
        "len": length,
        "payload_hex": payload.hex(),
    }

    if rtype == 0x01 and length >= 1:                      # ADC
        row["value"] = payload[0]
    elif rtype in (0x02, 0x03) and length >= 4:            # CAN
        can_id = u32(payload, 0)
        data = payload[4:]
        row["id"] = can_id
        row["id_hex"] = f"0x{can_id:08X}"
        row["dlc"] = len(data)
        row["ext"] = bool(flags & 0x01)
        row["rtr"] = bool(flags & 0x02)
        row["data_hex"] = data.hex()
    elif rtype in (0x04, 0x05) and length >= 8:            # EVENT / OTA
        row["v0"] = u32(payload, 0)
        row["v1"] = u32(payload, 4)

    return row


def main() -> int:
    ap = argparse.ArgumentParser(description="SBB 原始 SD 日志 → CSV")
    ap.add_argument("image", type=Path, help="整卡/分区镜像文件")
    ap.add_argument("-o", "--out", type=Path, default=None, help="输出 CSV")
    args = ap.parse_args()

    data = args.image.read_bytes()
    if len(data) < 1024:
        print(f"✗ 镜像太小（{len(data)} B），不是有效镜像", file=sys.stderr)
        return 1

    try:
        hdr = parse_header(data[:512])
    except ValueError as exc:
        print(f"✗ {exc}", file=sys.stderr)
        return 2

    print(f"头: 版本={hdr['ver']} 记录尺寸={hdr['record_size']} "
          f"记录区起始 LBA={hdr['first_lba']}")
    if hdr["record_size"] != RECORD_SIZE:
        print(f"✗ 记录尺寸 {hdr['record_size']} != 本脚本支持的 {RECORD_SIZE}",
              file=sys.stderr)
        return 3

    rows = [record_to_row(lba, idx, rec)
            for lba, idx, rec in iter_records(data, hdr["first_lba"])]

    out = args.out or args.image.with_suffix(".csv")
    fieldnames = ["lba", "idx", "ts_ms", "type_id", "type", "ch", "flags", "len",
                  "payload_hex", "value", "id", "id_hex", "dlc", "ext", "rtr",
                  "data_hex", "v0", "v1"]
    with out.open("w", newline="", encoding="utf-8") as fh:
        wr = csv.DictWriter(fh, fieldnames=fieldnames, extrasaction="ignore")
        wr.writeheader()
        wr.writerows(rows)

    counts: dict[str, int] = {}
    for r in rows:
        counts[r["type"]] = counts.get(r["type"], 0) + 1

    print(f"✓ 解析出 {len(rows)} 条记录 → {out}")
    for name, n in sorted(counts.items()):
        print(f"    {name:8s} {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
