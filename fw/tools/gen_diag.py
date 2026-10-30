#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  gen_diag.py —— 从"诊断表"生成 ODX + JSON
#
#  为什么先有 JSON：ODX 的 XSD 很重，手写容易漏必填项；把"诊断内容"放在
#  JSON 里（人可读、可 diff、可从 CDD/ODX 反向导入），再由本脚本生成 ODX。
#
#  输出:
#    diag/diag_table.json   —— 机器可读的诊断表（唯一真源）
#    diag/SBB_ECU.odx       —— ODX 2.2.0 结构（well-formed，供 ODXStudio/
#                              CANdelaStudio 导入后微调）
# ─────────────────────────────────────────────────────────────────────────────
import json
import os
import xml.etree.ElementTree as ET
from xml.dom import minidom

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(FW, "diag")

# ── 诊断表：与 App/app/app_uds.c + diag_handlers.c 一一对应 ────────────────
TABLE = {
    "ecu": {
        "name": "SBB-F103C8T6",
        "description": "SBB 单板黑匣子 ECU（ADC/OLED/SD/CAN + UDS on CAN OTA）",
        "tx_id": "0x7E0", "rx_id": "0x7E8",
        "bus": "CAN 11-bit, 1 Mbps", "isotp": {"bs": 1, "stmin_ms": 2,
                                               "max_msg": 520, "block_bytes": 512},
    },
    "sessions": [
        {"id": "0x01", "name": "Default",     "p2_ms": 50,  "p2star_ms": 5000},
        {"id": "0x02", "name": "Programming", "p2_ms": 50,  "p2star_ms": 5000},
        {"id": "0x03", "name": "Extended",    "p2_ms": 50,  "p2star_ms": 5000},
    ],
    "security": [
        {"level": "0x01/0x02", "name": "ProgrammingUnlock",
         "seed_len": 4, "key_len": 4,
         "algorithm": "key = CRC32(seed_be || SECRET)，SECRET='SBB1'（台架级）",
         "max_attempts": 3, "lockout_s": 10,
         "nrc": {"invalid_key": "0x35", "delay_not_expired": "0x37"}},
    ],
    "dids": [
        {"id": "0xF186", "name": "ActiveDiagnosticSession",
         "desc": "当前诊断会话", "len": 1, "type": "u8",
         "encoding": "1=Default 2=Programming 3=Extended"},
        {"id": "0xF187", "name": "BootloaderVersion",
         "desc": "Bootloader 版本（ASCII）", "len": 8, "type": "ascii"},
        {"id": "0xF190", "name": "PartitionInfo",
         "desc": "分区信息 16 B（boot base/size、app base/size、meta base）",
         "len": 16, "type": "bytes"},
        {"id": "0xF1A0", "name": "StagingStatus",
         "desc": "SD 暂存区状态 8 B（magic、size、crc、state）",
         "len": 8, "type": "bytes"},
    ],
    "routines": [
        {"id": "0xFF00", "name": "EraseStaging",
         "desc": "擦除 SD 卡尾暂存区（写 0xFF 到头块）", "type": "start"},
        {"id": "0xFF01", "name": "VerifyStaging",
         "desc": "校验暂存区镜像（magic/size/crc）", "type": "start"},
    ],
    "services": [
        {"sid": "0x10", "name": "DiagnosticSessionControl", "sub": ["0x01", "0x02", "0x03"]},
        {"sid": "0x11", "name": "ECUReset",                 "sub": ["0x01"]},
        {"sid": "0x22", "name": "ReadDataByIdentifier",     "dids": ["0xF186", "0xF187", "0xF190", "0xF1A0"]},
        {"sid": "0x27", "name": "SecurityAccess",           "sub": ["0x01", "0x02"]},
        {"sid": "0x31", "name": "RoutineControl",           "sub": ["0x01"], "routines": ["0xFF00", "0xFF01"]},
        {"sid": "0x34", "name": "RequestDownload",          "note": "max 50 KB，写入 SD 暂存区"},
        {"sid": "0x36", "name": "TransferData",             "note": "每次 512 B（一个扇区）"},
        {"sid": "0x37", "name": "RequestTransferExit",      "note": "校验 CRC 后置升级标志"},
        {"sid": "0x3E", "name": "TesterPresent"},
    ],
    "nrc": ["0x12", "0x13", "0x22", "0x24", "0x31", "0x33", "0x35", "0x37", "0x72", "0x78"],
}


def sn(s):
    """ODX SHORT-NAME 只允许 [A-Za-z][A-Za-z0-9_]*（不能有连字符/点/空格）。"""
    import re as _re
    v = _re.sub(r"[^A-Za-z0-9_]", "_", str(s))
    return v if v and v[0].isalpha() else "X" + v


def build_odx(table):
    """生成 ODX 2.2.0 结构（保留诊断语义；严格 XSD 校验需在 ODXStudio 里补 ADMIN-DATA）。"""
    root = ET.Element("ODX", {
        "MODEL-VERSION": "2.2.0",
        "xmlns:xsi": "http://www.w3.org/2001/XMLSchema-instance",
        "xmlns:xsd": "http://www.w3.org/2001/XMLSchema",
    })
    dlc = ET.SubElement(root, "DIAG-LAYER-CONTAINER", {"ID": "SBB_DLC"})
    ET.SubElement(dlc, "SHORT-NAME").text = sn(table["ecu"]["name"])
    bvs = ET.SubElement(dlc, "BASE-VARIANTS")
    bv = ET.SubElement(bvs, "BASE-VARIANT", {"ID": "SBB_BV"})
    ET.SubElement(bv, "SHORT-NAME").text = "SBB_BASE_VARIANT"
    ET.SubElement(bv, "LONG-NAME").text = table["ecu"]["description"]

    ddd = ET.SubElement(bv, "DIAG-DATA-DICTIONARY-SPEC")

    # ── 数据对象属性（DATA-OBJECT-PROP）──
    dops = ET.SubElement(ddd, "DATA-OBJECT-PROPS")
    for d in table["dids"]:
        dop = ET.SubElement(dops, "DATA-OBJECT-PROP", {"ID": "DOP_" + d["id"][2:]})
        ET.SubElement(dop, "SHORT-NAME").text = d["name"]
        ct = ET.SubElement(dop, "DIAG-CODED-TYPE",
                           {"xsi:type": "StandardLengthType",
                            "BASE-DATA-TYPE": "A_UINT32"})
        ET.SubElement(ct, "BIT-LENGTH").text = str(d["len"] * 8)

    # ── DID ──
    dids = ET.SubElement(ddd, "DATA-IDENTIFIERS")
    for d in table["dids"]:
        el = ET.SubElement(dids, "DATA-IDENTIFIER", {"ID": "DID_" + d["id"][2:]})
        ET.SubElement(el, "SHORT-NAME").text = d["name"]
        ET.SubElement(el, "LONG-NAME").text = d["desc"]
        ct = ET.SubElement(el, "DIAG-CODED-TYPE",
                           {"xsi:type": "StandardLengthType",
                            "BASE-DATA-TYPE": "A_UINT32"})
        ET.SubElement(ct, "BIT-LENGTH").text = str(d["len"] * 8)
        if isinstance(d.get("encoding"), str):
            ET.SubElement(el, "DESC").text = "编码: " + d["encoding"]

    # ── 例程 ──
    rts = ET.SubElement(ddd, "ROUTINES")
    for r in table["routines"]:
        el = ET.SubElement(rts, "ROUTINE", {"ID": "RID_" + r["id"][2:]})
        ET.SubElement(el, "SHORT-NAME").text = r["name"]
        ET.SubElement(el, "LONG-NAME").text = r["desc"]

    # ── 服务 ──
    svcs = ET.SubElement(bv, "DIAG-SERVICES")
    for s in table["services"]:
        sv = ET.SubElement(svcs, "DIAG-SERVICE", {"ID": "SVC_" + s["sid"][2:]})
        ET.SubElement(sv, "SHORT-NAME").text = s["name"]
        req = ET.SubElement(sv, "REQUEST-REF", {"ID-REF": "REQ_" + s["sid"][2:]})
        req.set("SEMANTIC", "REQUEST")

    # ── 通信参数（ISO-TP）──
    comparams = ET.SubElement(bv, "COMPARAM-REFS")
    for k, v in (("CP_CanIdTx", table["ecu"]["tx_id"]),
                 ("CP_CanIdRx", table["ecu"]["rx_id"]),
                 ("CP_ISOTP_BS", str(table["ecu"]["isotp"]["bs"])),
                 ("CP_ISOTP_STmin", str(table["ecu"]["isotp"]["stmin_ms"]))):
        c = ET.SubElement(comparams, "COMPARAM-REF", {"ID-REF": k})
        ET.SubElement(c, "VALUE").text = v

    return root


def main():
    os.makedirs(OUT, exist_ok=True)

    jp = os.path.join(OUT, "diag_table.json")
    with open(jp, "w", encoding="utf-8") as f:
        json.dump(TABLE, f, ensure_ascii=False, indent=2)
    print("  ✓", jp)

    # ★ ODX 不再由本脚本生成：手写 XML 无法通过 Vector schema 校验。
    #   现在改由 tools/odx_build.py 以 CANdelaStudio 导出的模板为骨架生成。
    if os.environ.get("SBB_LEGACY_ODX") != "1":
        print("  (ODX 改由 tools/odx_build.py 生成，本脚本只出 JSON)")
        return 0
    root = build_odx(TABLE)
    raw = ET.tostring(root, encoding="utf-8")
    pretty = minidom.parseString(raw).toprettyxml(indent="  ", encoding="UTF-8")
    op = os.path.join(OUT, "SBB_ECU.odx")
    with open(op, "wb") as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write(pretty.split(b"?>", 1)[-1].lstrip())
    print("  ✓", op)

    # 自校验：XML 可解析 + 关键节点数
    t = minidom.parse(op)
    print("  ✓ XML 解析通过")
    for tag in ("DATA-IDENTIFIER", "ROUTINE", "DIAG-SERVICE"):
        n = len(t.getElementsByTagName(tag))
        print("    {:<15} {} 个".format(tag, n))
    size = os.path.getsize(op)
    print("    ODX 大小        {} B".format(size))


if __name__ == "__main__":
    main()
