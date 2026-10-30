#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  make_ota_image.py —— 把 bin/sbb_app.bin 打包成可放到 SD 卡上的 SBB_APP.BIN
# ─────────────────────────────────────────────────────────────────────────────
#  镜像格式（boot/boot_update.c 按此解析）:
#    0   : "SBBIMG\0\0"
#    8   : u32 version        （仅记录，可用 --version 指定）
#    12  : u32 payload_size
#    16  : u32 payload_crc32  （zlib.crc32，与 boot_crc32 一致）
#    20  : u32 app_base       （必须等于 BOOT_APP_ADDR = 0x08003800）
#    24  : u32 header_crc32   （覆盖 0..23）
#    28  : u32 reserved
#    32  : payload（就是 sbb_app.bin 的原始内容）
#
#  用法:
#    python3 tools/make_ota_image.py                    # 输出 out/SBB_APP.BIN
#    python3 tools/make_ota_image.py --version 12
#    python3 tools/make_ota_image.py --check out/SBB_APP.BIN   # 校验一个已有镜像
# ─────────────────────────────────────────────────────────────────────────────
import argparse
import os
import struct
import sys
import zlib

APP_BASE = 0x08003800

# 镜像内软件指纹（App/memmap/sbb_version.c，由 sbb_app.ld 固定在 app+0x200）
FP_OFFSET = 0x200
FP_MAGIC = b"SBBF"


def read_fingerprint(data):
    if len(data) < FP_OFFSET + 40:
        return None
    b = data[FP_OFFSET:FP_OFFSET + 40]
    if b[:4] != FP_MAGIC:
        return None
    label = b[16:36].split(bytes(1))[0].decode("ascii", "replace")
    return {"major": b[4], "minor": b[5], "patch": b[6], "build": b[7],
            "semver": "{}.{}.{}".format(b[4], b[5], b[6]), "label": label,
            "date": int.from_bytes(b[8:12], "little")}
APP_MAX = 50 * 1024
HDR_SIZE = 32
MAGIC = b"SBBIMG\x00\x00"


def build(src: str, dst: str, version: int) -> int:
    data = open(src, "rb").read()
    if not data:
        print("✗ 源文件为空: " + src)
        return 1
    if len(data) > APP_MAX:
        print("✗ 固件 {} B 超过 App 区上限 {} B".format(len(data), APP_MAX))
        return 1
    if len(data) & 1:
        data += b"\xFF"                       # 补齐到偶数（半字编程要求）

    crc = zlib.crc32(data) & 0xFFFFFFFF
    fp = read_fingerprint(data)
    if version == 0 and fp:
        version = fp["major"] * 10000 + fp["minor"] * 100 + fp["patch"]
        print("   指纹: v{}  ({})".format(fp["semver"], fp["label"]))
    head = MAGIC + struct.pack("<IIIII", version, len(data), crc, APP_BASE, 0)
    head = head[:24] + struct.pack("<I", zlib.crc32(head[:24]) & 0xFFFFFFFF) + b"\x00" * 4
    assert len(head) == HDR_SIZE, len(head)

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(head)
        f.write(data)

    print("✓ 已生成 {}".format(dst))
    print("   payload = {} B   crc32 = 0x{:08X}   version = {}".format(len(data), crc, version))
    print("   文件总大小 = {} B".format(HDR_SIZE + len(data)))
    print("   app_base = 0x{:08X}  (上限 {} B)".format(APP_BASE, APP_MAX))
    return 0


def check(path: str) -> int:
    raw = open(path, "rb").read()
    if len(raw) < HDR_SIZE:
        print("✗ 文件太短")
        return 1
    magic = raw[:8]
    version, size, crc, base, hdr_crc = struct.unpack("<IIIII", raw[8:28])
    calc_hdr = zlib.crc32(raw[:24]) & 0xFFFFFFFF
    payload = raw[HDR_SIZE:HDR_SIZE + size]
    calc_crc = zlib.crc32(payload) & 0xFFFFFFFF

    print("magic      = {!r}  {}".format(magic, "✓" if magic == MAGIC else "✗"))
    print("version    = {}".format(version))
    print("size       = {} B".format(size))
    print("app_base   = 0x{:08X}  {}".format(base, "✓" if base == APP_BASE else "✗"))
    print("header crc = 0x{:08X} vs 0x{:08X}  {}".format(hdr_crc, calc_hdr, "✓" if hdr_crc == calc_hdr else "✗"))
    print("payload crc= 0x{:08X} vs 0x{:08X}  {}".format(crc, calc_crc, "✓" if crc == calc_crc else "✗"))
    print("实际负载   = {} B  {}".format(len(payload), "✓" if len(payload) == size else "✗ 被截断"))
    ok = (magic == MAGIC and base == APP_BASE and hdr_crc == calc_hdr
          and crc == calc_crc and len(payload) == size)
    print("\n{}".format("✓ 镜像完好，可以直接放到 SD 卡根目录" if ok else "✗ 镜像有问题"))
    return 0 if ok else 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="bin/sbb_app.bin")
    ap.add_argument("--dst", default="out/SBB_APP.BIN")
    ap.add_argument("--version", type=int, default=0)
    ap.add_argument("--check")
    a = ap.parse_args()
    if a.check:
        sys.exit(check(a.check))
    sys.exit(build(a.src, a.dst, a.version))


if __name__ == "__main__":
    main()
