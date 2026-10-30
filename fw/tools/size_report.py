#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  size_report.py —— 按模块统计 Flash / RAM 占用（从 ELF 符号表算）
#
#  为什么不看 .o：本工程的构建不保留中间对象文件，ELF 才是唯一权威。
#  nm -S 给出每个符号的 size，按符号名前缀归到模块，得到真实的分布。
#
#  输出: out/size_report.json（网页读） + 控制台表格
# ─────────────────────────────────────────────────────────────────────────────
import json
import os
import re
import subprocess
import sys

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(FW, "bin", "sbb_app.elf")
BOOT_ELF = os.path.join(FW, "bin", "sbb_boot.elf")
OUT = os.path.join(FW, "out", "size_report.json")

FLASH_BUDGET = 50 * 1024
RAM_BUDGET = 20 * 1024
BOOT_FLASH_BUDGET = 14 * 1024

NM = "arm-none-eabi-nm"
SIZE = "arm-none-eabi-size"

RTOS_PREFIX = ("vTask", "xTask", "uxTask", "prv", "pxReady", "xQueue", "uxQueue",
               "xEvent", "vEvent", "xStream", "xTimer", "vPort", "xPort",
               "osThread", "osMessage", "osSemaphore", "osMutex", "osTimer",
               "osKernel", "osDelay", "osSignal", "SysTick_Handler_x",
               "xPortPendSVHandler", "vPortSVCHandler", "xPortSysTickHandler",
               "pxCurrentTCB", "uxCurrentNumberOfTasks", "xTickCount")


def tool(*args):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=90).stdout
    except Exception:                                   # noqa: BLE001
        return ""


def classify(name):
    """符号名 -> (分组, 模块名)"""
    n = name

    # ★ 任务栈实例（静态分配，名字固定）
    m = re.match(r"^g_([a-z0-9_]+)_stack$", n)
    if m:
        return ("task", m.group(1))

    # 我们自己写的：app_xxx_* / bsp_xxx_* / diag_* / ssd1306_* / sched_*
    m = re.match(r"^(app|bsp)_([a-z0-9]+)_", n)
    if m:
        return (m.group(1), m.group(2))
    if n.startswith("diag_"):
        return ("app", "diag")
    if n.startswith("uds_"):
        return ("app", "uds")
    if n.startswith("cmd_"):
        return ("app", "console")
    if n.startswith("ssd1306") or n.startswith("oled_"):
        return ("drv", "ssd1306")
    if n.startswith("sched_"):
        return ("sched", "scheduler")
    if n.startswith("g_sbb_fingerprint") or n.startswith("sbb_fingerprint"):
        return ("memmap", "version")

    # newlib / 编译器运行时
    if n.startswith("__") or n in ("_sbrk", "_write", "_read", "_close"):
        return ("libc", "newlib")
    if n.startswith(("printf", "sprintf", "snprintf", "vsnprintf", "memcpy",
                     "memset", "memmove", "strlen", "strcmp", "strcpy",
                     "strncpy", "strstr", "atoi", "strtol", "malloc", "free",
                     "calloc", "realloc")):
        return ("libc", "newlib")

    # HAL：HAL_I2C_xxx -> hal/i2c
    m = re.match(r"^HAL_([A-Z0-9]+)_", n)
    if m:
        return ("hal", m.group(1).lower())

    # CubeMX 生成
    if n.startswith("MX_") or n in ("main", "SystemInit", "SystemCoreClockUpdate",
                                    "HAL_MspInit", "HAL_MspDeInit",
                                    "HAL_InitTick", "Error_Handler",
                                    "HAL_IncTick", "HAL_GetTick", "HAL_Delay"):
        return ("cube", "init")
    if n.startswith(("gpio_", "i2c_", "spi_", "usart_", "can_", "dma_", "rcc_",
                     "tim_", "adc_", "iwdg_", "pwr_", "flash_", "exti_")):
        return ("cube", "periph")
    if n.endswith("_IRQHandler") or n.endswith("_Handler") and n[0].isupper():
        return ("cube", "vectors")

    # FreeRTOS
    for p in RTOS_PREFIX:
        if n.startswith(p):
            return ("freertos", "kernel")

    # printf / libc
    if n.startswith(("_printf", "printf", "vsnprintf", "memcpy", "memset",
                     "strlen", "strcmp", "strcpy", "__aeabi", "_sbrk", "_write",
                     "_read", "_close", "_lseek", "_fstat", "_isatty", "_exit",
                     "_kill", "_getpid", "_malloc_r", "_free_r")):
        return ("libc", "libc")

    return ("other", "other")


def symbols(path):
    """[(name, size, type)]，type 用原 ELF 类型字母"""
    out = tool(NM, "-S", path)
    rows = []
    for line in out.splitlines():
        p = line.split()
        if len(p) < 3:
            continue
        # nm -S 有两种行：
        #   "addr size type name"   (有 size)
        #   "addr      type name"   (无 size -> 记为 0，绝不能把地址当 size)
        if len(p) >= 4:
            size_s, typ, name = p[1], p[2], " ".join(p[3:])
        elif len(p) == 3:
            size_s, typ, name = "0", p[1], p[2]
        else:
            continue
        try:
            sz = int(size_s, 16)
        except ValueError:
            continue
        if typ in ("U", "w", "W", "N", "n", "?"):
            continue
        rows.append((name, sz, typ))
    return rows


def elf_totals(path):
    out = tool(SIZE, path)
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit():
            return int(p[0]), int(p[1]), int(p[2])
    return 0, 0, 0


def build_modules(rows):
    mods = {}
    for name, sz, typ in rows:
        if typ in ("T", "t"):
            kind = "text"
        elif typ in ("R", "r"):
            kind = "rodata"
        elif typ in ("D", "d"):
            kind = "data"
        elif typ in ("B", "b"):
            kind = "bss"
        else:
            continue
        g, sub = classify(name)
        key = "{}/{}".format(g, sub)
        m = mods.setdefault(key, {"module": key, "group": g,
                                  "text": 0, "rodata": 0, "data": 0, "bss": 0,
                                  "symbols": 0})
        m[kind] += sz
        m["symbols"] += 1
    return sorted(mods.values(), key=lambda m: -(m["text"] + m["rodata"]))


def task_stacks():
    stacks = []
    for root, _d, files in os.walk(os.path.join(FW, "App")):
        for fn in files:
            if not fn.endswith(".c"):
                continue
            txt = open(os.path.join(root, fn), encoding="utf-8",
                       errors="replace").read()
            for m in re.finditer(r"#define\s+(\w*TASK_STACK)\s+(\d+)u?", txt):
                stacks.append({"macro": m.group(1), "words": int(m.group(2)),
                               "bytes": int(m.group(2)) * 4, "file": fn})
    return stacks


def stack_symbols(rows):
    """任务栈实例（静态数组，在 .bss 里，名字形如 xx.stack）"""
    out = []
    for name, sz, typ in rows:
        if typ in ("b", "B") and (".stack" in name or "Stack" in name) and sz >= 64:
            out.append({"name": name, "size": sz})
    return sorted(out, key=lambda s: -s["size"])[:20]


def stack_heap():
    """从链接脚本读栈/堆预算（RAM 预算的另一半）。"""
    out = {"stack": 1024, "heap": 512}
    try:
        ld = open(os.path.join(FW, "App", "sbb_app.ld"), encoding="utf-8").read()
        m = re.search(r"_Min_Stack_Size\s*=\s*(0x[0-9A-Fa-f]+|\d+)", ld)
        if m:
            v = m.group(1)
            out["stack"] = int(v, 16) if v.lower().startswith("0x") else int(v)
        m = re.search(r"_Min_Heap_Size\s*=\s*(0x[0-9A-Fa-f]+|\d+)", ld)
        if m:
            v = m.group(1)
            out["heap"] = int(v, 16) if v.lower().startswith("0x") else int(v)
    except Exception:                                   # noqa: BLE001
        pass
    return out


def main():
    if not os.path.isfile(ELF):
        print("找不到", ELF, "—— 先 build"); return 1

    rows = symbols(ELF)
    mods = build_modules(rows)
    t, d, b = elf_totals(ELF)
    bt, bd, bb = elf_totals(BOOT_ELF) if os.path.isfile(BOOT_ELF) else (0, 0, 0)

    groups = {}
    for m in mods:
        g = groups.setdefault(m["group"], {"group": m["group"], "text": 0,
                                           "rodata": 0, "data": 0, "bss": 0})
        for k in ("text", "rodata", "data", "bss"):
            g[k] += m[k]

    big_ram = [{"name": n, "size": s, "type": ty}
               for n, s, ty in sorted(rows, key=lambda r: -r[1])
               if ty in ("b", "B", "d", "D") and s >= 128][:20]

    report = {
        "elf": os.path.basename(ELF),
        "app": {"text": t, "data": d, "bss": b,
                "flash": t + d, "ram": d + b,
                "flash_budget": FLASH_BUDGET, "ram_budget": RAM_BUDGET,
                "flash_pct": round(100.0 * (t + d) / FLASH_BUDGET, 1),
                "ram_pct": round(100.0 * (d + b) / RAM_BUDGET, 1)},
        "boot": {"text": bt, "data": bd, "bss": bb,
                 "flash": bt + bd, "ram": bd + bb,
                 "flash_budget": BOOT_FLASH_BUDGET,
                 "flash_pct": round(100.0 * (bt + bd) / BOOT_FLASH_BUDGET, 1)},
        "modules": mods,
        "groups": sorted(groups.values(), key=lambda g: -(g["text"] + g["rodata"])),
        "ram_symbols": big_ram,
        "task_stacks": task_stacks(),
        "stack_instances": stack_symbols(rows),
        "image_bytes": os.path.getsize(os.path.join(FW, "bin", "sbb_app.bin")),
        "stack_heap": stack_heap(),
    }

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    print("{:<26} {:>7} {:>7} {:>7} {:>7} {:>8} {:>5}".format(
        "模块", "text", "rodata", "data", "bss", "FLASH", "符号"))
    print("-" * 74)
    for m in mods:
        print("{:<26} {:>7} {:>7} {:>7} {:>7} {:>8} {:>5}".format(
            m["module"], m["text"], m["rodata"], m["data"], m["bss"],
            m["text"] + m["rodata"] + m["data"], m["symbols"]))
    print("-" * 74)
    print("APP  FLASH {:>6} / {} B ({:.1f}%)   RAM {:>6} / {} B ({:.1f}%)".format(
        t + d, FLASH_BUDGET, 100.0 * (t + d) / FLASH_BUDGET,
        d + b, RAM_BUDGET, 100.0 * (d + b) / RAM_BUDGET))
    print("BOOT FLASH {:>6} / {} B ({:.1f}%)".format(
        bt + bd, BOOT_FLASH_BUDGET, 100.0 * (bt + bd) / BOOT_FLASH_BUDGET))
    print("镜像 {:>6} B   →  {}".format(report["image_bytes"], OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
