#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  odx_build.py —— 以 Vector 导出的 UDS 模板为骨架，注入本项目的 DID/例程
#
#  核心思路：不"从零生成 ODX"，而是【克隆模板里已有的行】再改值。
#  克隆能保证元素顺序、类型、引用关系全部符合 schema —— 手写永远差一点。
#
#  输入:  diag/_ref/template_UDS14.odx-d   (CANdelaStudio 用 UDS 14.0 模板导出)
#          diag/diag_table.json             (诊断唯一真源)
#  输出:  diag/SBB_ECU.odx-d
# ─────────────────────────────────────────────────────────────────────────────
import copy
import json
import os
import sys
import xml.etree.ElementTree as ET

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TPL = os.path.join(FW, "diag", "_ref", "template_UDS14.odx-d")
OUT = os.path.join(FW, "diag", "SBB_ECU.odx-d")
ECU_NAME = "SBB_F103C8T6"

NS = "{http://www.w3.org/2001/XMLSchema-instance}"


def die(msg):
    print("  ✗", msg)
    sys.exit(1)


def sn_of(row):
    e = row.find("SHORT-NAME")
    return e.text if e is not None else ""


def key_of(row):
    k = row.find("KEY")
    if k is None or not k.text:
        return None
    try:
        return int(k.text, 16) if not k.text.strip().isdigit() else int(k.text)
    except ValueError:
        return None


def find(root, tag, sid):
    for e in root.iter(tag):
        if e.get("ID") == sid:
            return e
    return None


def main():
    if not os.path.isfile(TPL):
        die("缺少模板: " + TPL + "（需先从 CANdelaStudio 导出 .odx-d 放到 diag/_ref/）")

    table = json.load(open(os.path.join(FW, "diag", "diag_table.json"), encoding="utf-8"))
    tree = ET.parse(TPL)
    root = tree.getroot()

    # ── ① 改名：ECU 用合法 SHORT-NAME（[A-Za-z][A-Za-z0-9_]*）──
    renamed = 0
    for tag in ("DIAG-LAYER-CONTAINER", "BASE-VARIANT"):
        for el in root.iter(tag):
            s = el.find("SHORT-NAME")
            if s is not None and s.text and s.text.startswith("New_ECU"):
                s.text = ECU_NAME if tag == "BASE-VARIANT" else ECU_NAME + "_DLC"
                renamed += 1
    # ①-b LONG-NAME 也一起改（ODXStudio 的树里显示的是 LONG-NAME，不是 SHORT-NAME）
    renamed_ln = 0
    for el in root.iter("LONG-NAME"):
        if el.text and "New ECU" in el.text:
            el.text = "SBB F103C8T6  bench ECU (ADC/OLED/SD/CAN + UDS on CAN OTA)"
            renamed_ln += 1
    print("  ① 改名 SHORT-NAME {} 处 / LONG-NAME {} 处 -> {}".format(renamed, renamed_ln, ECU_NAME))

    # ── ② DID 表：找到含 F190 的那张表，克隆一行注入我们的 DID ──
    did_table = None
    for tb in root.iter("TABLE"):
        for rw in tb.findall(".//TABLE-ROW"):
            if key_of(rw) == 0xF190:
                did_table = tb
                break
        if did_table is not None:
            break
    if did_table is None:
        die("模板里找不到 DID 表（含 0xF190 行）")

    template_row = None
    have = set()
    for rw in did_table.findall(".//TABLE-ROW"):
        k = key_of(rw)
        if k is not None:
            have.add(k)
        if k == 0xF190:
            template_row = rw
    print("  ② DID 表 {} 现有 {} 行: {}".format(
        did_table.get("ID"), len(have), " ".join("%04X" % k for k in sorted(have))))

    rows_parent = None
    for rw in did_table.findall(".//TABLE-ROW"):
        rows_parent = None  # 找 TABLE-ROWS 容器
    rows_container = did_table          # TABLE-ROW 是 TABLE 的直接子元素

    added = []
    for d in table["dids"]:
        did = int(d["id"], 16)
        if did in have:
            print("     = {:04X} {} 已存在，跳过".format(did, d["name"]))
            continue
        new_row = copy.deepcopy(template_row)
        new_row.set("ID", "_9{:03d}".format(did & 0xFFF))     # 唯一 ID
        for r in new_row.iter():
            if r.tag == "KEY":
                r.text = str(did)                              # KEY 是十进制
            if r.tag == "SHORT-NAME":
                r.text = "DID_" + d["name"]
            if r.tag == "LONG-NAME":
                r.text = d["desc"]
        rows_container.append(new_row)
        added.append("%04X %s" % (did, d["name"]))
    print("     新增 {} 行: {}".format(len(added), ", ".join(added) if added else "无"))

    # ── ③ 写出 ──
    ET.register_namespace("xsi", "http://www.w3.org/2001/XMLSchema-instance")
    tree.write(OUT, encoding="utf-8", xml_declaration=True)
    print("  ③ 写出 {} ({} KB)".format(OUT, os.path.getsize(OUT) // 1024))

    # ── ④ 自检 ──
    try:
        ET.parse(OUT)
        print("  ④ XML 解析 ✓")
    except Exception as e:
        die("XML 解析失败: " + str(e))

    try:
        from odxtools.database import Database
        db = Database()
        db.add_odx_file(OUT)
        print("  ⑤ odxtools 载入 ✓  diag_layers = {}".format(len(db.diag_layers)))
    except Exception as e:
        print("  ⑤ odxtools 载入失败（仅供参考，不影响 Vector 工具）: {}".format(str(e)[:120]))


if __name__ == "__main__":
    main()
