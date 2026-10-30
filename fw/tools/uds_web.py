#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  uds_web.py —— UDS on CAN 刷写控制台
#    浏览器打开 http://127.0.0.1:8080
#    单文件、零第三方依赖（http.server + 手写 HTML/CSS/JS）
#
#  功能:
#    · 手动选择 / 拖拽上传 BIN（自动校验 SBBIMG 头）
#    · 流程可视化：七步流程图 + 每步耗时 + 实时进度 + 分级日志
# ─────────────────────────────────────────────────────────────────────────────
import json
import os
import re
import shutil
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import can
from uds_tester import IsoTp, Uds, neg, nrc_text, key_from_seed
import mem_analyze as MA

PORT = 8080
OUT_DIR = os.path.join(os.path.dirname(HERE), "out")
UP_DIR = os.path.join(OUT_DIR, "uploads")
DEFAULT_BIN = os.path.join(OUT_DIR, "SBB_APP.BIN")

STEPS = [
    ("进入编程会话", "10 02"),
    ("安全访问",     "27 01/02"),
    ("擦除暂存区",   "31 01 FF00"),
    ("请求下载",     "34"),
    ("传输固件",     "36 xN"),
    ("校验并提交",   "37"),
    ("复位进 Boot",  "11 01"),
]

STATE = {
    "phase": "idle",
    "message": "就绪 —— 选择镜像后点「开始刷写」",
    "sent": 0, "total": 0, "percent": 0.0,
    "speed_bps": 0.0, "eta_s": None, "elapsed": 0.0,
    "step": -1, "step_ok": [], "step_ms": {},
    "image": None, "selected": None, "analysis": None,
    "log": [],
    "started": None,
}
LOCK = threading.Lock()


def log(msg, level="info"):
    with LOCK:
        STATE["log"].append({"t": time.strftime("%H:%M:%S"), "m": msg, "lvl": level})
        if len(STATE["log"]) > 500:
            STATE["log"] = STATE["log"][-400:]


def phase(p, msg=""):
    with LOCK:
        STATE["phase"] = p
        if msg:
            STATE["message"] = msg


def step_begin(i):
    with LOCK:
        STATE["step"] = i
        STATE["_t0"] = time.time()


def step_done(i):
    with LOCK:
        if i not in STATE["step_ok"]:
            STATE["step_ok"].append(i)
        t0 = STATE.pop("_t0", None)
        if t0:
            STATE["step_ms"][str(i)] = int((time.time() - t0) * 1000)


# ── 镜像管理 ────────────────────────────────────────────────────────────────
def probe_image(path):
    """读 SBBIMG 头；返回元数据或 None。也接受裸 bin（无头）。"""
    try:
        size = os.path.getsize(path)
        raw = open(path, "rb").read(64)
        meta = {"name": os.path.basename(path), "path": path, "bytes": size,
                "mtime": int(os.path.getmtime(path))}
        if len(raw) >= 32 and raw.startswith(b"SBBIMG"):
            ver, psize, crc, base, hcrc = struct.unpack("<IIIII", raw[8:28])
            head = open(path, "rb").read(24)
            payload = open(path, "rb").read()[32:32 + psize]
            fp = read_fingerprint(payload)
            meta.update({"kind": "sbbimg", "version": ver, "size": psize,
                         "crc": "0x%08X" % crc, "app_base": "0x%08X" % base,
                         "ok": (zlib.crc32(head) & 0xFFFFFFFF) == hcrc,
                         "fp": fp,
                         "semver": fp["semver"] if fp else None})
        else:
            # 裸 bin 没有 SBBIMG 头 -> bootloader 会直接拒收，刷了也是白等
            meta.update({"kind": "raw", "version": None, "size": size,
                         "crc": None, "app_base": None, "ok": False,
                         "why": "缺 SBBIMG 头，先用 make_ota_image.py 打包"})
        return meta
    except Exception:                                   # noqa: BLE001
        return None


def refresh_image(path=None):
    p = path or STATE.get("selected") or DEFAULT_BIN
    img = probe_image(p) if os.path.isfile(p) else None
    with LOCK:
        STATE["image"] = img
        STATE["selected"] = p if img else None
    return img


def list_images():
    out = []
    for d in (OUT_DIR, UP_DIR):
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.lower().endswith((".bin", ".hex"[:0] or ".bin")):
                p = os.path.join(d, fn)
                if os.path.isfile(p):
                    m = probe_image(p)
                    if m:
                        out.append(m)
    out.sort(key=lambda m: m["mtime"], reverse=True)
    return out


MAGIC = b"SBBIMG" + bytes(2)

# ── 镜像内软件指纹（App/memmap/sbb_version.c + sbb_app.ld）──────────────
FP_OFFSET = 0x200          # 相对 app 基址 0x08003800
FP_SIZE = 40
FP_MAGIC = b"SBBF"


def read_fingerprint(raw):
    """在镜像前 8 KB 内搜索 SBBF 指纹（不假设固定偏移，并做合理性校验）。

    为什么不用固定 0x200：boot 镜像、旧镜像、非 app 镜像在 0x200 处是代码，
    硬读会得到 17925 这类垃圾版本号。搜索 + 校验才能保证要么对、要么没有。
    """
    i = raw.find(FP_MAGIC, 0, 8192)
    if i < 0 or i + FP_SIZE > len(raw):
        return None
    b = raw[i:i + FP_SIZE]
    major, minor, patch, build = b[4], b[5], b[6], b[7]
    if not (0 < major < 100 and minor < 100 and patch < 100):
        return None
    try:
        label = b[16:36].split(bytes(1))[0].decode("ascii")
    except Exception:
        return None
    if not label or not all(32 <= ord(c) < 127 for c in label):
        return None
    return {
        "offset": i,
        "major": major, "minor": minor, "patch": patch, "build": build,
        "semver": "{}.{}.{}".format(major, minor, patch),
        "date": struct.unpack("<I", b[8:12])[0],
        "label": label,
        "sw_id": struct.unpack("<H", b[36:38])[0],
    }
APP_BASE = 0x08003800
APP_MAX = 50 * 1024


def package(raw, version=0):
    """把裸 app bin 包成 SBBIMG 镜像（与 make_ota_image.py 完全一致）。"""
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    head = MAGIC + struct.pack("<IIIII", int(version), len(raw), crc, APP_BASE, 0)
    head = head[:24] + struct.pack("<I", zlib.crc32(head[:24]) & 0xFFFFFFFF) + bytes(4)
    return head + raw


def looks_like_app(raw):
    """裸 bin 必须像一份 STM32 app：SP 落在 RAM，复位向量落在 app 分区。"""
    if len(raw) < 1024 or (len(raw) % 4) != 0:
        return False
    sp, rv = struct.unpack("<II", raw[:8])
    if not (0x20000000 <= sp <= 0x20005000):      # SRAM 20 KB
        return False
    if not (0x08003800 <= (rv & ~1) < 0x0800FC00):  # app 分区
        return False
    return (rv & 1) == 1                           # Thumb 位


def guess_version(fn):
    m = re.search(r"(\d{1,5})", fn or "")
    return int(m.group(1)) if m else 0


def next_version():
    """建议的版本号 = 现有镜像最大值 + 1（没有就从 1 开始）。"""
    vs = [int(m.get("version") or 0) for m in list_images() if m.get("version")]
    return (max(vs) if vs else 0) + 1


def safe_name(fn):
    fn = os.path.basename(fn or "upload.bin")
    fn = re.sub(r"[^A-Za-z0-9._-]", "_", fn)
    if not fn.lower().endswith((".bin", ".elf", ".map")):
        fn += ".bin"          # 无扩展名才补 .bin（不能把 .map 改成 .map.bin）
    return fn[:64]


# ── 刷写线程 ────────────────────────────────────────────────────────────────
def worker():
    bus = None
    try:
        with LOCK:
            path = STATE.get("selected") or DEFAULT_BIN
        img = probe_image(path)
        if not img or not img.get("ok"):
            log("镜像无效：" + str(path), "err"); phase("error", "镜像无效"); return
        raw = open(path, "rb").read()
        total = len(raw)
        with LOCK:
            STATE.update({"sent": 0, "total": total, "percent": 0.0, "speed_bps": 0.0,
                          "eta_s": None, "elapsed": 0.0, "step": -1, "step_ok": [],
                          "step_ms": {}, "log": [], "started": time.time()})
        log("镜像 {}  {} B  {}{}".format(
            img["name"], total,
            "crc=" + img["crc"] if img["crc"] else "裸 bin",
            "  v%d" % img["version"] if img["version"] is not None else ""))

        phase("running", "打开 VN1610 ...")
        bus = can.Bus(interface="vector", channel=0, bitrate=1000000, serial=744342,
                      app_name="sbb_web", receive_own_messages=False)
        tp = IsoTp(bus)
        uds = Uds(tp)

        def run(i, fn):
            step_begin(i)
            log("→ {} ({})".format(STEPS[i][0], STEPS[i][1]))
            r = fn()
            if r is None or neg(r):
                why = ("NRC " + nrc_text(r[2])) if (r and neg(r)) else "无响应"
                log("✗ {} 失败：{}".format(STEPS[i][0], why), "err")
                phase("error", "{} 失败 {}".format(STEPS[i][0], why))
                return None
            log("✓ {} -> {}".format(STEPS[i][0], r.hex(" ")), "ok")
            step_done(i)
            return r

        if run(0, lambda: uds.session(0x02)) is None: return
        r = uds.security_seed()
        if neg(r):
            log("✗ 安全访问被拒 " + nrc_text(r[2]), "err"); phase("error", "安全访问被拒"); return
        key = key_from_seed(r[2:])
        if run(1, lambda: uds.security_key(key)) is None: return
        if run(2, lambda: uds.routine(0x01, 0xFF00)) is None: return
        r = uds.request_download(0, total)
        if neg(r):
            log("✗ 请求下载被拒 " + nrc_text(r[2]), "err"); phase("error", "请求下载被拒"); return
        log("✓ 请求下载 -> {}".format(r.hex(" ")), "ok"); step_done(3)

        step_begin(4)
        phase("running", "传输固件中")
        off, seq, t0 = 0, 1, time.time()
        retries = 0
        while off < total:
            chunk = raw[off:off + 512]
            for attempt in range(8):
                try:
                    r = uds.transfer_data(seq, chunk)
                    if neg(r) and len(r) >= 3:
                        if r[2] == 0x24:
                            break
                        if r[2] == 0x72:
                            retries += 1
                            log("块 {} NRC 0x72 -> 重发".format(seq), "warn")
                            time.sleep(0.03); continue
                        log("块 {} NRC {}".format(seq, nrc_text(r[2])), "err")
                        phase("error", "传输失败 NRC=" + nrc_text(r[2])); return
                    break
                except TimeoutError:
                    if attempt == 7:
                        log("块 {} 重试耗尽".format(seq), "err")
                        phase("error", "传输中断（块 {}）".format(seq)); return
            off += len(chunk); seq += 1
            el = time.time() - t0
            rate = off / max(el, 1e-6)
            with LOCK:
                STATE.update({"sent": off, "total": total,
                              "percent": round(off * 100.0 / total, 1),
                              "speed_bps": round(rate, 1),
                              "elapsed": round(el, 1),
                              "eta_s": round((total - off) / max(rate, 1e-6), 1)})
        with LOCK:
            STATE["step_ms"]["4"] = int((time.time() - t0) * 1000)
            if 4 not in STATE["step_ok"]:
                STATE["step_ok"].append(4)
        log("✓ 传输完成 {:.1f}s（重发 {} 次）".format(time.time() - t0, retries), "ok")

        if run(5, lambda: uds.transfer_exit()) is None: return

        step_begin(6)
        phase("running", "复位，Bootloader 开始搬运")
        try:
            uds.ecu_reset(0x01)
        except TimeoutError:
            pass
        step_done(6)
        log("✓ 11 01 已发送 —— 板子自行搬运并跳转新固件", "ok")
        phase("done", "刷写完成 ✓ 观察 OLED / 串口确认新固件启动")
    except Exception as exc:                            # noqa: BLE001
        log("异常：{}".format(exc), "err"); phase("error", str(exc))
    finally:
        if bus is not None:
            try: bus.shutdown()
            except Exception: pass                      # noqa: BLE001


# ── 前端 ────────────────────────────────────────────────────────────────────
PAGE = r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>SBB · UDS 刷写控制台</title>
<style>
:root{--bg:#0b0f14;--card:#141a21;--line:#232b36;--fg:#e6edf3;--mut:#8b949e;
      --acc:#2f81f7;--ok:#3fb950;--err:#f85149;--warn:#d29922;--dim:#5c6b7a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
     font:14px/1.6 -apple-system,"Segoe UI",Roboto,"Microsoft YaHei",sans-serif}
.wrap{max-width:1120px;margin:0 auto;padding:22px}
h1{font-size:19px;margin:0 0 2px;font-weight:600}
.sub{color:var(--mut);font-size:12.5px;margin-bottom:16px}
.grid{display:grid;grid-template-columns:1fr 330px;gap:14px}
@media(max-width:900px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
.card h2{font-size:11.5px;color:var(--mut);font-weight:600;letter-spacing:.07em;
         text-transform:uppercase;margin:0 0 12px}

/* 进度条 */
.bar{height:36px;background:#0b0f14;border:1px solid var(--line);border-radius:9px;
     overflow:hidden;position:relative}
.fill{height:100%;width:0;background:linear-gradient(90deg,#1f6feb,#58a6ff);
      transition:width .3s ease}
.pct{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
     font-size:15px;font-weight:600;font-variant-numeric:tabular-nums;
     text-shadow:0 1px 3px rgba(0,0,0,.65)}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-top:12px}
.stat{background:#0b0f14;border:1px solid var(--line);border-radius:8px;padding:8px 10px}
.stat .k{color:var(--mut);font-size:11px}
.stat .v{font-size:13.5px;font-weight:600;font-variant-numeric:tabular-nums}

/* 流程图 */
.flow{display:flex;align-items:stretch;gap:0;margin-top:6px}
.node{flex:1;position:relative;text-align:center;padding:10px 4px 8px}
.node .bub{width:30px;height:30px;margin:0 auto 7px;border-radius:50%;
    border:1.5px solid var(--line);background:#0b0f14;display:flex;align-items:center;
    justify-content:center;font-size:12px;font-weight:700;color:var(--dim);
    transition:all .25s}
.node .nm{font-size:11.5px;color:var(--dim);line-height:1.35;min-height:32px}
.node .cm{font-family:ui-monospace,Consolas,monospace;font-size:10px;color:#465361}
.node .tm{font-size:10px;color:var(--dim);height:13px}
.node.done .bub{background:var(--ok);border-color:var(--ok);color:#04140a}
.node.done .nm{color:var(--fg)}
.node.cur .bub{border-color:var(--acc);color:var(--acc);
    box-shadow:0 0 0 4px rgba(47,129,247,.14);animation:pulse 1.4s infinite}
.node.cur .nm{color:var(--acc)}
.node.bad .bub{background:var(--err);border-color:var(--err);color:#fff}
@keyframes pulse{0%,100%{box-shadow:0 0 0 4px rgba(47,129,247,.14)}
  50%{box-shadow:0 0 0 8px rgba(47,129,247,.05)}}
.line{height:2px;background:var(--line);flex:0 0 12px;margin-top:22px;border-radius:2px}
.line.on{background:var(--ok)}

/* 镜像选择 */
.drop{border:1.5px dashed var(--line);border-radius:10px;padding:18px;text-align:center;
      color:var(--mut);font-size:13px;cursor:pointer;transition:.2s}
.drop.hover{border-color:var(--acc);background:rgba(47,129,247,.06);color:var(--acc)}
.drop b{color:var(--fg)}
.list{margin-top:10px;max-height:172px;overflow:auto}
.item{display:flex;align-items:center;gap:9px;padding:7px 9px;border-radius:7px;
      cursor:pointer;border:1px solid transparent;font-size:12.5px}
.item:hover{background:#0b0f14}
.item.sel{border-color:var(--acc);background:rgba(47,129,247,.08)}
.item .nm{font-weight:600;color:var(--fg);overflow:hidden;text-overflow:ellipsis;
          white-space:nowrap}
.item .mt{margin-left:auto;color:var(--dim);font-size:11px;font-variant-numeric:tabular-nums}
.item .del{width:18px;height:18px;line-height:16px;text-align:center;border-radius:5px;
    color:var(--dim);font-size:11px;flex:0 0 auto}
.item .del:hover{background:rgba(248,81,73,.16);color:var(--err)}
.item .bd{width:7px;height:7px;border-radius:50%;background:var(--ok);flex:0 0 auto}
.item .bd.raw{background:var(--warn)}
.item .bd.bad{background:var(--err)}

button{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:11px 20px;
       font-size:14px;font-weight:600;cursor:pointer;width:100%}
button:disabled{background:#26303b;color:var(--dim);cursor:default}
button.ghost{background:transparent;color:var(--mut);border:1px solid var(--line);
             margin-top:8px;font-weight:400;padding:8px}
#log{height:270px;overflow:auto;font-family:ui-monospace,Consolas,monospace;
     font-size:12px;background:#0b0f14;border:1px solid var(--line);
     border-radius:8px;padding:10px}
.ok{color:var(--ok)} .err{color:var(--err)} .warn{color:var(--warn)} .info{color:var(--fg)}
.tag{display:inline-block;padding:3px 9px;border-radius:999px;font-size:11.5px;
     background:#1c232c;color:var(--mut);font-weight:600}
.tag.run{background:rgba(47,129,247,.15);color:var(--acc)}
.tag.done{background:rgba(63,185,80,.15);color:var(--ok)}
.tag.err{background:rgba(248,81,73,.15);color:var(--err)}
.warnbox{background:rgba(210,153,34,.1);border:1px solid rgba(210,153,34,.35);
         color:var(--warn);border-radius:8px;padding:8px 11px;font-size:12.5px;margin-top:9px}
</style></head><body><div class="wrap">
<h1>SBB · UDS on CAN 刷写控制台</h1>
<div class="sub">VN1610 CH0 · 1 Mbps · 0x7E0 / 0x7E8 · ISO-TP 512 B/块</div>

<div class="grid">
  <div>
    <div class="card" style="margin-bottom:14px">
      <h2>刷写流程</h2>
      <div class="flow" id="flow"></div>
    </div>

    <div class="card" style="margin-bottom:14px">
      <div style="display:flex;align-items:center;gap:10px;margin-bottom:12px">
        <span class="tag" id="phase">idle</span>
        <span id="msg" style="color:var(--mut);font-size:13px"></span>
      </div>
      <div class="bar"><div class="fill" id="fill"></div><div class="pct" id="pct">0.0%</div></div>
      <div class="stats">
        <div class="stat"><div class="k">已传 / 总量</div><div class="v" id="sent">0 / 0</div></div>
        <div class="stat"><div class="k">速度</div><div class="v" id="spd">0 B/s</div></div>
        <div class="stat"><div class="k">剩余</div><div class="v" id="eta">—</div></div>
        <div class="stat"><div class="k">已用时</div><div class="v" id="ela">0.0s</div></div>
      </div>
    </div>

    <div class="card" style="margin-bottom:14px">
      <h2>内存占用 · 当前构建固件</h2>
      <div style="font-size:11.5px;color:var(--dim);margin:-6px 0 10px">
        来自 <code>bin/sbb_app.elf</code>（构建产物）—— 与镜像列表里上传/删除的 .bin 无关
      </div>
      <div id="mem"><span style="color:var(--dim);font-size:12px">加载中…</span></div>
    </div>

    <div class="card"><h2>日志</h2><div id="log"></div></div>
  </div>

  <div>
    <div class="card" style="margin-bottom:14px">
      <h2>选择镜像</h2>
      <div class="drop" id="drop">
        <b>点击选择</b> 或拖入 <b>.bin / .elf / .map</b><br>
        <span style="font-size:11.5px">.elf 可直接刷（自动转 bin）；.bin 配 .map 可算各段占用</span>
      </div>
      <input type="file" id="file" accept=".bin,.elf,.map" style="display:none">
      <div style="display:flex;align-items:center;gap:8px;margin-top:10px">
        <span style="color:var(--mut);font-size:12px">版本号</span>
        <input id="ver" type="number" min="0" max="65535" placeholder="自动"
               style="width:88px;background:#0b0f14;border:1px solid var(--line);
                      border-radius:6px;color:var(--fg);padding:6px 9px;font-size:12.5px">
        <span style="color:var(--dim);font-size:11px">自动递增；可手改</span>
      </div>
      <div class="list" id="list"></div>
      <div id="img" class="warnbox" style="display:none"></div>
    </div>

    <div class="card">
      <h2>操作</h2>
      <button id="go" onclick="start()">▶ 开始刷写</button>
      <button class="ghost" onclick="loadImages(true)">刷新镜像列表</button>
    </div>
  </div>
</div></div>

<script>
const STEPS = __STEPS__;
let busy = false, sel = null;

function esc(s){ return String(s).replace(/[&<>"]/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])) }
function fmt(n){ return (n||0).toLocaleString('en-US') }
function kb(n){ return n>=1048576 ? (n/1048576).toFixed(2)+' MB' : (n/1024).toFixed(1)+' KB' }

/* ── 流程图 ── */
function renderFlow(s){
  const done = s.step_ok||[], cur = s.step, bad = (s.phase==='error');
  let html = '';
  STEPS.forEach((st,i)=>{
    const isDone = done.includes(i);
    const isCur  = (cur===i) && (s.phase==='running');
    const isBad  = bad && (cur===i);
    const cls = isDone?'done':(isBad?'bad':(isCur?'cur':''));
    const ms  = (s.step_ms||{})[String(i)];
    const tm  = ms!=null ? (ms>=1000 ? (ms/1000).toFixed(1)+'s' : ms+'ms') : '';
    html += `<div class="node ${cls}">
       <div class="bub">${isDone?'✓':(isBad?'✕':i+1)}</div>
       <div class="nm">${st[0]}</div>
       <div class="cm">${st[1]}</div>
       <div class="tm">${tm}</div>
     </div>`;
    if(i < STEPS.length-1) html += `<div class="line ${isDone?'on':''}"></div>`;
  });
  document.getElementById('flow').innerHTML = html;
}

/* ── 镜像列表 ── */
async function loadImages(force){
  const r = await (await fetch('/api/images')).json();
  const box = document.getElementById('list');
  const vin = document.getElementById('ver');
  if (vin && r.next_version) { vin.value = r.next_version; }
  if(!r.items.length){ box.innerHTML = '<div style="color:var(--dim);font-size:12px;padding:6px">out/ 下没有 .bin</div>'; return }
  box.innerHTML = r.items.map(m=>{
    const cls = m.kind==='sbbimg' ? (m.ok?'':'bad') : (m.ok?'raw':'bad');
    const tag = m.semver ? ('v'+m.semver) : (m.kind==='sbbimg' ? ('v'+m.version) : '裸bin');
    return `<div class="item ${sel===m.path?'sel':''}" data-p="${esc(m.path)}">
      <span class="bd ${cls}"></span>
      <span class="nm">${esc(m.name)}</span>
      <span style="color:var(--dim);font-size:11px">${tag} · ${kb(m.bytes)}</span>
      <span class="mt">${new Date(m.mtime*1000).toLocaleTimeString('zh-CN',{hour12:false})}</span>
      <span class="del" data-d="${esc(m.path)}" title="删除">✕</span>
    </div>`;
  }).join('');
  box.querySelectorAll('.del').forEach(el=>{
    el.onclick = async (ev)=>{
      ev.stopPropagation();
      const pp = el.dataset.d;
      const nm = pp.split(/[\/]/).pop();
      if(!confirm('删除 ' + nm + ' ?')) return;
      const rs = await (await fetch('/api/delete',{method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({path:pp})})).json();
      if(rs.ok){ if(sel===pp){ sel=null } loadImages() }
      else alert(rs.why||'删除失败');
    };
  });
  box.querySelectorAll('.item').forEach(el=>{
    el.onclick = async ()=>{
      const p = el.dataset.p;
      const rs = await (await fetch('/api/select',{method:'POST',
        headers:{'Content-Type':'application/json'}, body:JSON.stringify({path:p})})).json();
      if(rs.ok){ sel = p; loadImages(); }
    };
  });
}

/* ── 上传 ── */
const drop = document.getElementById('drop'), fileIn = document.getElementById('file');
drop.onclick = ()=> fileIn.click();
drop.ondragover = e=>{ e.preventDefault(); drop.classList.add('hover') };
drop.ondragleave = ()=> drop.classList.remove('hover');
drop.ondrop = e=>{ e.preventDefault(); drop.classList.remove('hover');
  if(e.dataTransfer.files.length) upload(e.dataTransfer.files[0]); };
fileIn.onchange = ()=> { if(fileIn.files.length) upload(fileIn.files[0]); };

async function upload(f){
  if(!/\.(bin|elf|map)$/i.test(f.name)){ alert('只接受 .bin / .elf / .map 文件'); return }
  drop.innerHTML = `上传中… <b>${esc(f.name)}</b>`;
  const r = await (await fetch('/api/upload', {method:'POST',
      headers:{'X-Filename': f.name,
               'X-Version': ((document.getElementById('ver')||{}).value||'')}, body: f})).json();
  if(r.ok){
    if(r.path){ sel = r.path; }
    if(r.analysis){
      window.__ana = r.analysis;
      drop.innerHTML = `<span style="color:var(--ok)">✓ ${esc(r.why || '已解析')}</span>`;
      setTimeout(()=>{ drop.innerHTML = `<b>点击选择</b> 或拖入 <b>.bin / .elf / .map</b><br><span style="font-size:11.5px">.elf 可直接刷（自动转 bin）；.bin 配 .map 可算各段占用</span>`; }, 4000);
      loadImages(); renderAna(r.analysis); return;
    }
    if(r.packed){
      drop.innerHTML = `<span style="color:var(--ok)">✓ 已自动加 SBBIMG 头（v${r.image.version}, ${kb(r.image.bytes)}）</span>`;
      setTimeout(()=>{ drop.innerHTML = `<b>点击选择</b> 或把 <b>.bin</b> 拖到这里<br><span style="font-size:11.5px">自动校验 / 自动打包 SBBIMG</span>`; }, 3500);
    } else {
      drop.innerHTML = `<b>点击选择</b> 或把 <b>.bin</b> 拖到这里<br><span style="font-size:11.5px">自动校验 SBBIMG 头</span>`;
    }
    loadImages();
  } else {
    drop.innerHTML = `<span style="color:var(--err)">✗ ${esc(r.why||'上传失败')}</span>`;
  }
}

function renderAna(a){
  if(!a || !a.ranges || !a.ranges.length) return;
  const box = document.getElementById('mem');
  const regs = (a.regions||[]).map(g=>{
    const p = g.length ? (100*g.origin/g.length, 100*(g.used||0)/g.length) : 0;
    return `<div style="font-size:12px;color:var(--mut);margin-top:6px">${g.name}
       <b style="color:var(--fg)">${((g.used||0)/1024).toFixed(1)}</b> / ${(g.length/1024).toFixed(0)} KB
       <span style="float:right;color:var(--fg)">${p.toFixed(1)}%</span></div>
      <div style="height:8px;background:#0b0f14;border-radius:4px;overflow:hidden;margin-top:4px">
       <div style="width:${Math.min(100,p)}%;height:100%;background:${p>90?'var(--err)':p>75?'var(--warn)':'var(--acc)'}"></div></div>`;
  }).join('');
  const rows = a.ranges.filter(r=>r.start < 0x20000000).map(r=>`<tr>
      <td style="color:var(--fg);padding:2px 0">${r.name}</td>
      <td style="text-align:right;color:var(--dim);font-family:ui-monospace,monospace">0x${r.start.toString(16).padStart(8,'0')}</td>
      <td style="text-align:right;font-variant-numeric:tabular-nums">${r.size} B</td>
      <td style="text-align:right;color:var(--dim)">${(r.pct_of_region||0).toFixed(1)}%</td></tr>`).join('');
  box.innerHTML = regs +
    `<div style="margin-top:12px;font-size:11.5px;color:var(--mut)">段分布（按 MAP/ELF 地址）</div>
     <table style="width:100%;border-collapse:collapse;font-size:11.5px;margin-top:4px">
      <tr style="color:var(--dim)"><td>段</td><td style="text-align:right">起始</td>
      <td style="text-align:right">大小</td><td style="text-align:right">占比</td></tr>
      ${rows}</table>`;
}

async function loadBuild(){
  let d;
  try { d = await (await fetch('/api/build')).json(); } catch(e){ return }
  if(!d || !d.app){ document.getElementById('mem').textContent = '缺少 size_report.json'; return }
  const a = d.app, b = d.boot || {};
  const bar = (v, max, col) => {
    const p = Math.min(100, 100*v/max);
    const c = p > 90 ? 'var(--err)' : p > 75 ? 'var(--warn)' : col;
    return `<div style="height:8px;background:#0b0f14;border-radius:4px;overflow:hidden;margin-top:4px">
       <div style="width:${p}%;height:100%;background:${c}"></div></div>`;
  };
  const tasks = (d.modules||[]).filter(m=>m.group==='task');
  const mods  = (d.modules||[]).filter(m=>m.group!=='task' && (m.text+m.rodata)>0).slice(0,8);
  let h = '';
  h += `<div style="font-size:12px;color:var(--mut)">FLASH
      <b style="color:var(--fg)">${(a.flash/1024).toFixed(1)}</b> / 50 KB
      <span style="float:right;color:var(--fg)">${a.flash_pct}%</span></div>` + bar(a.flash, a.flash_budget, 'var(--acc)');
  h += `<div style="font-size:12px;color:var(--mut);margin-top:9px">RAM
      <b style="color:var(--fg)">${(a.ram/1024).toFixed(1)}</b> / 20 KB
      <span style="float:right;color:var(--fg)">${a.ram_pct}%</span></div>` + bar(a.ram, a.ram_budget, 'var(--ok)');
  h += `<div style="font-size:12px;color:var(--mut);margin-top:9px">BOOT
      <b style="color:var(--fg)">${(b.flash/1024).toFixed(1)}</b> / 14 KB
      <span style="float:right;color:var(--fg)">${b.flash_pct}%</span></div>` + bar(b.flash, b.flash_budget, 'var(--acc)');
  if(tasks.length){
    h += `<div style="margin-top:12px;font-size:11.5px;color:var(--mut)">任务栈（静态分配）</div>
      <table style="width:100%;border-collapse:collapse;font-size:12px;margin-top:4px">`;
    tasks.forEach(m=>{ h += `<tr><td style="color:var(--fg);padding:2px 0">${m.module.replace('task/','')}</td>
      <td style="text-align:right;font-variant-numeric:tabular-nums">${m.bss} B</td></tr>` });
    h += `</table>`;
  }
  if(mods.length){
    h += `<div style="margin-top:12px;font-size:11.5px;color:var(--mut)">占用最大的模块（FLASH）</div>
      <table style="width:100%;border-collapse:collapse;font-size:12px;margin-top:4px">`;
    mods.forEach(m=>{ h += `<tr><td style="color:var(--fg);padding:2px 0">${m.module}</td>
      <td style="text-align:right;font-variant-numeric:tabular-nums">${m.text+m.rodata} B</td></tr>` });
    h += `</table>`;
  }
  if(d.image_bytes){
    h += `<div style="margin-top:10px;font-size:11.5px;color:var(--dim)">镜像文件 ${d.image_bytes} B</div>`;
  }
  if(d.generated){
    const ts = new Date(d.generated*1000).toLocaleString('zh-CN',{hour12:false});
    h += `<div style="margin-top:4px;font-size:11px;color:var(--dim)">报表生成于 ${ts}</div>`;
  }
  document.getElementById('mem').innerHTML = h;
}

async function start(){
  if(busy) return; busy = true;
  document.getElementById('go').disabled = true;
  const r = await (await fetch('/api/flash',{method:'POST'})).json();
  if(!r.ok) { busy=false; document.getElementById('go').disabled=false; alert(r.why||'启动失败') }
}

/* ── 状态轮询 ── */
async function tick(){
  try{
    const s = await (await fetch('/api/state')).json();
    const ph = document.getElementById('phase');
    ph.textContent = s.phase;
    ph.className = 'tag ' + (s.phase==='running'?'run':s.phase==='done'?'done':s.phase==='error'?'err':'');
    document.getElementById('msg').textContent = s.message||'';
    document.getElementById('fill').style.width = (s.percent||0)+'%';
    document.getElementById('pct').textContent = (s.percent||0).toFixed(1)+'%';
    document.getElementById('sent').textContent = fmt(s.sent)+' / '+fmt(s.total)+' B';
    document.getElementById('spd').textContent = fmt(Math.round(s.speed_bps||0))+' B/s';
    document.getElementById('eta').textContent = (s.eta_s==null?'—':Math.round(s.eta_s)+' s');
    document.getElementById('ela').textContent = (s.elapsed||0).toFixed(1)+'s';
    renderFlow(s);
    if(!window.__mem){ window.__mem = 1; loadBuild(); }
    if(s.analysis && s.analysis !== window.__ana){ window.__ana = s.analysis; renderAna(s.analysis); }

    const im = s.image, box = document.getElementById('img');
    if(im){
      box.style.display = 'block';
      const bad = !im.ok;
      box.style.borderColor = bad ? 'rgba(248,81,73,.35)' : 'rgba(63,185,80,.28)';
      box.style.background  = bad ? 'rgba(248,81,73,.08)' : 'rgba(63,185,80,.07)';
      box.style.color       = bad ? 'var(--err)' : 'var(--ok)';
      box.innerHTML = bad
        ? `✗ ${esc(im.name)} 头校验失败，不能刷`
        : `✓ 已选 <b>${esc(im.name)}</b><br>
           <span style="color:var(--mut)">${kb(im.bytes)} · payload ${fmt(im.size)} B${im.crc?' · crc '+im.crc:''}</span><br>
           <span style="color:var(--dim);font-size:11.5px">${im.fp ? 'v'+im.fp.semver+'  ·  ' : ''}OTA 头 version=${im.version}${im.version?`（= MAJOR×10000+MINOR×100+PATCH）`:''}</span>`;
      if(!sel){ sel = im.path; loadImages(); }
    } else { box.style.display='none' }

    const lb = document.getElementById('log');
    const atBottom = lb.scrollTop + lb.clientHeight >= lb.scrollHeight - 30;
    lb.innerHTML = (s.log||[]).map(e=>`<div class="${e.lvl}">[${e.t}] ${e.m}</div>`).join('');
    if(atBottom) lb.scrollTop = lb.scrollHeight;

    busy = (s.phase==='running');
    document.getElementById('go').disabled = busy || !s.image || !s.image.ok;
  }catch(e){}
}
loadImages(); setInterval(tick,300); tick();
</script></body></html>"""

PAGE = PAGE.replace("__STEPS__", json.dumps([[a, b] for a, b in STEPS], ensure_ascii=False))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _text(self, code, body, ctype):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"):
            self._text(200, PAGE, "text/html; charset=utf-8")
        elif p == "/api/state":
            with LOCK:
                snap = dict(STATE)
                snap.pop("_t0", None)
            self._json(200, snap)
        elif self.path == "/api/build":
            try:
                rp = os.path.join(OUT_DIR, "size_report.json")
                with open(rp, encoding="utf-8") as f:
                    rep = json.load(f)
                rep["generated"] = int(os.path.getmtime(rp))
                self._json(200, rep)
            except Exception:                        # noqa: BLE001
                self._json(404, {"ok": False, "why": "没有 size_report.json，先跑 tools/size_report.py"})
        elif p == "/api/images":
            self._json(200, {"items": list_images(),
                             "selected": STATE.get("selected"),
                             "next_version": next_version()})
        else:
            self._json(404, {})

    def do_POST(self):
        p = self.path.split("?")[0]

        if p == "/api/flash":
            with LOCK:
                if STATE["phase"] == "running":
                    self._json(409, {"ok": False, "why": "正在刷写"}); return
                im = STATE.get("image") or {}
                if not STATE.get("selected"):
                    self._json(400, {"ok": False, "why": "没有选择镜像"}); return
                if not im.get("ok"):
                    self._json(400, {"ok": False,
                                     "why": im.get("why") or "镜像头攒验失败，不能刷"}); return
            threading.Thread(target=worker, daemon=True).start()
            self._json(200, {"ok": True})

        elif p == "/api/upload":
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0 or n > 8 * 1024 * 1024:
                self._json(400, {"ok": False, "why": "文件为空或超过 8 MB"}); return
            data = self.rfile.read(n)
            os.makedirs(UP_DIR, exist_ok=True)
            raw_name = self.headers.get("X-Filename") or "upload.bin"
            ext = os.path.splitext(raw_name)[1].lower()
            fn = safe_name(raw_name)
            if ext in (".elf", ".map"):
                dst = os.path.join(UP_DIR, fn)
                if os.path.exists(dst):
                    stem, e2 = os.path.splitext(fn)
                    dst = os.path.join(UP_DIR, "{}_{}{}".format(stem, int(time.time()), e2))
                open(dst, "wb").write(data)
                try:
                    ana = MA.analyze(dst)
                except Exception as exc:                 # noqa: BLE001
                    self._json(400, {"ok": False, "why": "解析失败: " + str(exc)}); return
                with LOCK:
                    STATE["analysis"] = ana
                    STATE["analysis_path"] = dst
                if ext == ".elf":
                    # ELF -> bin，直接可刷
                    out_bin = os.path.splitext(dst)[0] + ".bin"
                    if MA.elf_to_bin(dst, out_bin):
                        blob = open(out_bin, "rb").read()
                        if blob.startswith(b"SBBIMG"):
                            out_bin2 = out_bin
                        else:
                            _fp = read_fingerprint(blob)
                            _v = (guess_version(os.path.basename(dst)) if not _fp
                                  else _fp["major"] * 10000 + _fp["minor"] * 100 + _fp["patch"])
                            blob = package(blob, _v)
                            open(out_bin, "wb").write(blob)
                            out_bin2 = out_bin
                        m = probe_image(out_bin2)
                        with LOCK:
                            STATE["selected"] = out_bin2
                            STATE["image"] = m
                        self._json(200, {"ok": True, "path": out_bin2, "image": m,
                                         "packed": True, "analysis": ana,
                                         "why": "已从 ELF 生成 bin"}); return
                self._json(200, {"ok": True, "analysis": ana,
                                 "why": "已解析 " + ext[1:].upper()}); return

            dst = os.path.join(UP_DIR, fn)
            if os.path.exists(dst):
                stem, ext = os.path.splitext(fn)
                dst = os.path.join(UP_DIR, "{}_{}{}".format(stem, int(time.time()), ext))
            packed = False
            if not data.startswith(b"SBBIMG"):
                # 裸 app bin -> 自动加 SBBIMG 头（否则 bootloader 一定拒收）
                if len(data) > APP_MAX:
                    self._json(400, {"ok": False,
                                     "why": "裸 bin 超过 50 KB 分区上限"}); return
                if not looks_like_app(data):
                    self._json(400, {"ok": False,
                                     "why": "不是 STM32 固件：SP/复位向量不在预期范围"}); return
                fp = read_fingerprint(data)          # 固件自己声明的版本（最可信）
                ver = guess_version(os.path.basename(self.headers.get("X-Filename") or ""))
                if fp:
                    ver = fp["major"] * 10000 + fp["minor"] * 100 + fp["patch"]
                hv = (self.headers.get("X-Version") or "").strip()
                if hv.isdigit():
                    ver = int(hv)          # 用户在页面上手填的版本号优先
                if ver == 0:
                    ver = next_version()   # 没有线索 -> 自动递增
                data = package(data, ver)
                packed = True
            open(dst, "wb").write(data)
            m = probe_image(dst)
            if not m or not m.get("ok"):
                self._json(400, {"ok": False, "why": "不是有效的 SBBIMG 镜像（或超过 50 KB）"}); return
            with LOCK:
                STATE["selected"] = dst
                STATE["image"] = m
            self._json(200, {"ok": True, "path": dst, "image": m, "packed": packed})

        elif p == "/api/delete":
            try:
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                path = os.path.abspath(body.get("path") or "")
                root = os.path.abspath(OUT_DIR) + os.sep
                if not path.startswith(root) or not os.path.isfile(path):
                    self._json(403, {"ok": False, "why": "路径越界或文件不存在"}); return
                os.remove(path)
                with LOCK:
                    if STATE.get("selected") == path:
                        STATE["selected"] = None
                        STATE["image"] = None
                self._json(200, {"ok": True})
            except Exception as exc:                    # noqa: BLE001
                self._json(400, {"ok": False, "why": str(exc)})

        elif p == "/api/select":
            try:
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                path = os.path.abspath(body.get("path") or "")
                root = os.path.abspath(OUT_DIR) + os.sep
                if not path.startswith(root):
                    self._json(403, {"ok": False, "why": "路径越界"}); return
                m = probe_image(path)
                if not m:
                    self._json(400, {"ok": False, "why": "无法读取"}); return
                with LOCK:
                    STATE["selected"] = path
                    STATE["image"] = m
                self._json(200, {"ok": True, "image": m})
            except Exception as exc:                    # noqa: BLE001
                self._json(400, {"ok": False, "why": str(exc)})
        else:
            self._json(404, {})


def main():
    os.makedirs(UP_DIR, exist_ok=True)
    refresh_image()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("=" * 64)
    print("  SBB · UDS 刷写控制台")
    print("  浏览器：  http://127.0.0.1:{}/".format(PORT))
    print("  镜像目录：{}".format(OUT_DIR))
    print("  上传目录：{}".format(UP_DIR))
    print("  Ctrl+C 退出")
    print("=" * 64)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已退出")


if __name__ == "__main__":
    main()
