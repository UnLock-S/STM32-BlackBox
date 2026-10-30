#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  mem_analyze.py —— 从 ELF / MAP / BIN 分析内存占用
#
#  三种输入：
#    .elf  -> 段范围(size -A) + 分模块(nm)  —— 信息最全
#    .map  -> 内存区域 + 每个段的起始地址与长度
#    .bin  -> 只能给总大小；配合 .map/.elf 才能算每段占用
#
#  统一输出:
#    { "kind": "elf|map|bin",
#      "regions": [{name, origin, length}],
#      "ranges":  [{name, start, size, end, pct_of_region}],
#      "modules": [...],            # 仅 ELF
#      "totals":  {flash, ram, ...} }
# ─────────────────────────────────────────────────────────────────────────────
import os
import re
import subprocess

SIZE = "arm-none-eabi-size"
NM = "arm-none-eabi-nm"
OBJCOPY = "arm-none-eabi-objcopy"


def _run(*args):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=120).stdout
    except Exception:                                   # noqa: BLE001
        return ""


# ── ELF ─────────────────────────────────────────────────────────────────────
def elf_regions_and_ranges(path):
    """size -A 给每个段的 size 和 addr。"""
    out = _run(SIZE, "-A", path)
    ranges = []
    for line in out.splitlines():
        m = re.match(r"^(\.\S+)\s+(\d+)\s+(\d+)\s*$", line.strip())
        if not m:
            continue
        name, size, addr = m.group(1), int(m.group(2)), int(m.group(3))
        if size == 0 or addr == 0 or name.startswith((".debug", ".comment",
                                                     ".ARM.attributes")):
            continue
        ranges.append({"name": name, "start": addr, "size": size})
    return ranges


def elf_modules(path):
    """按符号前缀归模块（与 size_report.py 同一套规则）。"""
    try:
        import size_report as SR
        rows = SR.symbols(path)
        return SR.build_modules(rows), rows
    except Exception:                                   # noqa: BLE001
        return [], []


def guess_regions(ranges):
    """没有 map 时，按地址粗略还原 FLASH / RAM 两个区域。"""
    fl = [r for r in ranges if 0x08000000 <= r["start"] < 0x20000000]
    rm = [r for r in ranges if r["start"] >= 0x20000000]
    regs = []
    if fl:
        lo = min(r["start"] for r in fl)
        hi = max(r["start"] + r["size"] for r in fl)
        regs.append({"name": "FLASH", "origin": lo, "length": hi - lo, "used": hi - lo})
    if rm:
        lo = min(r["start"] for r in rm)
        hi = max(r["start"] + r["size"] for r in rm)
        regs.append({"name": "RAM", "origin": lo, "length": hi - lo, "used": hi - lo})
    return regs


def analyze_elf(path):
    ranges = elf_regions_and_ranges(path)
    mods, rows = elf_modules(path)
    t = d = b = 0
    out = _run(SIZE, path)
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit():
            t, d, b = int(p[0]), int(p[1]), int(p[2])
            break
    return {
        "kind": "elf", "file": os.path.basename(path),
        "totals": {"text": t, "data": d, "bss": b,
                   "flash": t + d, "ram": d + b},
        "regions": guess_regions(ranges),
        "ranges": sorted(ranges, key=lambda r: r["start"]),
        "modules": mods,
        "ram_symbols": [{"name": n, "size": s, "type": ty}
                        for n, s, ty in sorted(rows, key=lambda r: -r[1])
                        if ty in ("b", "B", "d", "D") and s >= 128][:16],
    }


def elf_to_bin(elf, dst):
    """把 ELF 的 loadable 段抽成 raw bin（OTA 用）。"""
    _run(OBJCOPY, "-O", "binary", "-R", ".sbb_version" if False else ".comment", elf, dst)
    if not os.path.isfile(dst) or os.path.getsize(dst) == 0:
        _run(OBJCOPY, "-O", "binary", elf, dst)
    return os.path.isfile(dst) and os.path.getsize(dst) > 0


# ── MAP ─────────────────────────────────────────────────────────────────────
RE_REGION = re.compile(r"^(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")
RE_SECTION = re.compile(r"^(\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")


def analyze_map(path):
    """解析 GNU ld 的 .map：内存区域 + 每个输出段的地址/大小。

    关键：输出段在【列 0】，输入段是缩进的（且常为 0x0），
          只认列 0 才能拿到真实地址。
    """
    regions, ranges = [], []
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except Exception:                                   # noqa: BLE001
        return {"kind": "map", "regions": [], "ranges": [], "totals": {}}

    re_region = re.compile(r"^(\w+)\s+0x([0-9a-fA-F]{8})\s+0x([0-9a-fA-F]{8})\s+[xrw]+")
    re_out = re.compile(r"^(\.\S+)\s+0x([0-9a-fA-F]{8})\s+0x([0-9a-fA-F]+)")

    for line in txt.splitlines():
        if not line or line[0] in (" ", "	"):
            continue                       # 缩进 = 输入段/明细，跳过
        m = re_region.match(line)          # 内存区域表
        if m and m.group(1) not in ("Name",):
            regions.append({"name": m.group(1),
                            "origin": int(m.group(2), 16),
                            "length": int(m.group(3), 16)})
            continue
        m = re_out.match(line)             # 输出段
        if m:
            name, addr, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            skip = (addr == 0 or name.startswith((".debug", ".comment", ".ARM.attributes")))
            if size and not skip and not any(r["name"] == name for r in ranges):
                ranges.append({"name": name, "start": addr, "size": size})

    ranges.sort(key=lambda r: r["start"])
    for r in ranges:
        r["end"] = r["start"] + r["size"]
        for reg in regions:
            if reg["origin"] <= r["start"] < reg["origin"] + reg["length"]:
                reg["used"] = reg.get("used", 0) + r["size"]
                r["region"] = reg["name"]
                r["pct_of_region"] = round(100.0 * r["size"] / reg["length"], 2)                     if reg["length"] else 0.0
                break

    flash = next((r for r in regions if r["name"].upper().startswith("FLASH")), None)
    ram = next((r for r in regions if r["name"].upper().startswith("RAM")), None)
    totals = {}
    if flash:
        totals["flash"] = flash.get("used", 0)
        totals["flash_budget"] = flash["length"]
    if ram:
        totals["ram"] = ram.get("used", 0)
        totals["ram_budget"] = ram["length"]
    return {"kind": "map", "file": os.path.basename(path),
            "regions": regions, "ranges": ranges, "totals": totals}


# ── BIN 结合 ranges ─────────────────────────────────────────────────────────
def bin_coverage(bin_path, ranges, base=None):
    """给定段范围，检查 bin 是否覆盖、并算出每段在 bin 里的偏移。"""
    if not os.path.isfile(bin_path):
        return {}
    n = os.path.getsize(bin_path)
    if not ranges:
        return {"bytes": n, "covered": None}
    # 只看 FLASH 段：RAM 段在 0x2000_0000，混进来会把跨度算成几百 MB
    ranges = [r for r in ranges if r.get("region", "FLASH") == "FLASH"
              or r["start"] < 0x20000000]
    lo = min(r["start"] for r in ranges)
    hi = max(r["start"] + r["size"] for r in ranges)
    base = lo
    out = []
    for r in ranges:
        off = r["start"] - base
        out.append({"name": r["name"], "start": r["start"], "size": r["size"],
                    "bin_off": off,
                    "in_bin": 0 <= off and off + r["size"] <= n})
    return {"bytes": n, "base": base, "span": hi - base, "ranges": out,
            "covered": (hi - base) <= n}


def analyze(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".elf":
        return analyze_elf(path)
    if ext == ".map":
        return analyze_map(path)
    return {"kind": "bin", "file": os.path.basename(path),
            "bytes": os.path.getsize(path) if os.path.isfile(path) else 0}


def fmt(n):
    return "{:,}".format(n)


if __name__ == "__main__":
    import json
    import sys
    if len(sys.argv) < 2:
        print("用法: mem_analyze.py <file.elf|file.map|file.bin> [map]")
        sys.exit(1)
    r = analyze(sys.argv[1])
    if len(sys.argv) > 2:
        m = analyze_map(sys.argv[2])
        r["coverage"] = bin_coverage(sys.argv[1], m["ranges"])
        r["ranges"] = m["ranges"]
        r["regions"] = m["regions"]
    print(json.dumps(r, ensure_ascii=False, indent=2)[:3000])
