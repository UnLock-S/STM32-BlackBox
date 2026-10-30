#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  uds_ui.py —— SBB UDS on CAN 刷写工具（Windows GUI，tkinter，无第三方 UI 依赖）
# ─────────────────────────────────────────────────────────────────────────────
#  运行:  python3 tools/uds_ui.py
#  依赖:  pip install python-can   （已装 4.6.1 ✓）
#
#  设计要点:
#    · 复用 tools/uds_tester.py 里已验证的 ISO-TP / UDS 实现，UI 只负责编排 ✓
#    · 刷写在**后台线程**里跑，UI 不冻结；日志/进度通过队列回传 ✓
#    · 流水化: 发送一块 -> 等响应 -> 立刻发下一块；SD 落盘在 ECU 侧后台做 ✓
#      （UDS 0x36 是严格请求-响应，协议上不能真并行 ✗，能做的是把"等待"压到最小）
# ─────────────────────────────────────────────────────────────────────────────
import os
import queue
import struct
import sys
import threading
import time
import zlib

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import can
    from uds_tester import IsoTp, Uds, REQ_ID, RSP_ID, neg, nrc_text, key_from_seed
except Exception as exc:                                    # noqa: BLE001
    print("依赖缺失: ", exc)
    raise


# ══════════════════════════════════════════════════════════════════════════════
#  后台刷写任务
# ══════════════════════════════════════════════════════════════════════════════
class Flasher(threading.Thread):
    def __init__(self, cfg, path, out_q):
        super().__init__(daemon=True)
        self.cfg = cfg
        self.path = path
        self.q = out_q
        self.stop_flag = False

    def log(self, msg, level="info"):
        self.q.put(("log", level, msg))

    def prog(self, done, total):
        self.q.put(("prog", done, total))

    def run(self):
        bus = None
        try:
            raw = open(self.path, "rb").read()
            if len(raw) < 32 or not raw.startswith(b"SBBIMG"):
                self.log("不是 SBBIMG 镜像（先用 make_ota_image.py 打包）", "err")
                return
            ver, size, crc, base, hcrc = struct.unpack("<IIIII", raw[8:28])
            if (zlib.crc32(raw[:24]) & 0xFFFFFFFF) != hcrc:
                self.log("镜像头 CRC 错", "err")
                return
            self.log("镜像 v{}  {} B  crc=0x{:08X}  base=0x{:08X}".format(ver, size, crc, base))

            self.log("连接 VN1610 CH{} @ {} ...".format(self.cfg["channel"], self.cfg["bitrate"]))
            bus = can.Bus(interface="vector", channel=self.cfg["channel"],
                          bitrate=self.cfg["bitrate"], serial=self.cfg["serial"],
                          app_name="sbb_uds_ui", receive_own_messages=False)
            tp = IsoTp(bus)
            uds = Uds(tp)
            self.log("已连接 (0x{:03X} -> 0x{:03X})".format(REQ_ID, RSP_ID), "ok")

            def step(name, resp):
                if resp is None:
                    self.log(name + " -> 无响应", "err")
                    return False
                txt = resp.hex(" ")
                if neg(resp):
                    self.log("{} -> NRC {} ({})".format(name, "0x%02X" % resp[2], nrc_text(resp[2])), "err")
                    return False
                self.log("{} -> {}".format(name, txt), "ok")
                return True

            if not step("10 02 编程会话", uds.session(0x02)):      return
            r = uds.security_seed()
            if neg(r):
                self.log("27 01 被拒: " + nrc_text(r[2]), "err"); return
            seed = r[2:]
            key = key_from_seed(seed)
            self.log("seed={}  key={}".format(seed.hex(" "), key.hex(" ")))
            if not step("27 02 安全访问", uds.security_key(key)):  return
            if not step("31 01 FF00 擦暂存区", uds.routine(0x01, 0xFF00)): return
            if not step("34 请求下载", uds.request_download(0, len(raw))): return

            t0 = time.time()
            off = 0
            seq = 1
            while off < len(raw):
                if self.stop_flag:
                    self.log("用户中止", "warn"); return
                chunk = raw[off:off + 512]
                r = uds.transfer_data(seq, chunk)
                if neg(r):
                    self.log("0x36 seq={} NRC {}".format(seq, nrc_text(r[2])), "err")
                    return
                off += len(chunk)
                seq += 1
                self.prog(off, len(raw))
            dt = max(time.time() - t0, 1e-6)
            self.log("传输完成 {:.2f}s  {:.1f} KB/s".format(dt, len(raw) / 1024.0 / dt), "ok")

            if not step("37 传输结束", uds.transfer_exit()):
                return
            self.log("11 01 复位（ECU 将由 Bootloader 搬运并跳转）", "ok")
            uds.ecu_reset(0x01)
            self.log("完成 ✓ 约 8 秒后可用「读版本」确认", "ok")
        except Exception as exc:                            # noqa: BLE001
            self.log("异常: {}".format(exc), "err")
        finally:
            if bus is not None:
                try:
                    bus.shutdown()
                except Exception:                            # noqa: BLE001
                    pass
            self.q.put(("done", None))


# ══════════════════════════════════════════════════════════════════════════════
#  GUI
# ══════════════════════════════════════════════════════════════════════════════
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SBB · UDS on CAN 刷写工具")
        self.geometry("880x600")
        self.q = queue.Queue()
        self.worker = None
        self.bin_path = tk.StringVar(value=os.path.join(os.path.dirname(HERE), "out", "SBB_APP.BIN"))

        self._build_ui()
        self.after(80, self._poll)

    def _build_ui(self):
        top = ttk.LabelFrame(self, text="总线")
        top.pack(fill="x", padx=8, pady=6)

        ttk.Label(top, text="VN1610 序列号").grid(row=0, column=0, padx=4, pady=4, sticky="w")
        self.ent_serial = ttk.Entry(top, width=12)
        self.ent_serial.insert(0, "744342")
        self.ent_serial.grid(row=0, column=1, padx=4)

        ttk.Label(top, text="通道").grid(row=0, column=2, padx=4)
        self.cmb_ch = ttk.Combobox(top, values=["0", "1"], width=4, state="readonly")
        self.cmb_ch.current(0)
        self.cmb_ch.grid(row=0, column=3, padx=4)

        ttk.Label(top, text="速率").grid(row=0, column=4, padx=4)
        self.cmb_br = ttk.Combobox(top, values=["1000000", "500000", "250000"], width=9, state="readonly")
        self.cmb_br.current(0)
        self.cmb_br.grid(row=0, column=5, padx=4)

        ttk.Button(top, text="总线自检", command=self.on_selftest).grid(row=0, column=6, padx=6)
        ttk.Button(top, text="读版本", command=self.on_read_did).grid(row=0, column=7, padx=6)

        mid = ttk.LabelFrame(self, text="镜像")
        mid.pack(fill="x", padx=8, pady=6)
        ttk.Entry(mid, textvariable=self.bin_path).pack(side="left", fill="x", expand=True, padx=6, pady=6)
        ttk.Button(mid, text="选择 BIN...", command=self.on_pick).pack(side="left", padx=4)

        act = ttk.Frame(self)
        act.pack(fill="x", padx=8, pady=4)
        ttk.Button(act, text="擦除暂存区", command=self.on_erase).pack(side="left", padx=4)
        self.btn_flash = ttk.Button(act, text="▶ 开始刷写", command=self.on_flash)
        self.btn_flash.pack(side="left", padx=4)
        ttk.Button(act, text="11 01 复位", command=self.on_reset).pack(side="left", padx=4)
        ttk.Button(act, text="清空日志", command=lambda: self.txt.delete("1.0", "end")).pack(side="right", padx=4)

        self.pb = ttk.Progressbar(self, maximum=100)
        self.pb.pack(fill="x", padx=10, pady=6)

        self.txt = tk.Text(self, height=22, bg="#101418", fg="#d6e2ee", insertbackground="#d6e2ee")
        self.txt.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        for tag, color in (("info", "#d6e2ee"), ("ok", "#5ddc7a"), ("err", "#ff6b6b"),
                           ("warn", "#ffc857")):
            self.txt.tag_configure(tag, foreground=color)

        self.status = tk.Label(self, text="就绪", anchor="w")
        self.status.pack(fill="x", padx=10, pady=(0, 6))

    # ── 工具 ──
    def log(self, msg, level="info"):
        self.txt.insert("end", time.strftime("[%H:%M:%S] ") + msg + "\n", level)
        self.txt.see("end")

    def cfg(self):
        return {"serial": int(self.ent_serial.get() or 744342),
                "channel": int(self.cmb_ch.get()),
                "bitrate": int(self.cmb_br.get())}

    def _poll(self):
        try:
            while True:
                item = self.q.get_nowait()
                kind = item[0]
                if kind == "log":
                    self.log(item[2], item[1])
                elif kind == "prog":
                    done, total = item[1], item[2]
                    self.pb["value"] = done * 100.0 / max(total, 1)
                    self.status.config(text="{}/{} B  ({:.1f}%)".format(done, total, done * 100.0 / max(total, 1)))
                elif kind == "done":
                    self.btn_flash.config(state="normal")
                    self.status.config(text="完成")
        except queue.Empty:
            pass
        self.after(80, self._poll)

    def _bus_call(self, fn, *args):
        """在临时总线上执行一次短请求（不阻塞 UI 太久，用于自检/读 DID）。"""
        try:
            bus = can.Bus(interface="vector", channel=self.cfg()["channel"],
                          bitrate=self.cfg()["bitrate"], serial=self.cfg()["serial"],
                          app_name="sbb_uds_ui", receive_own_messages=False)
        except Exception as exc:                            # noqa: BLE001
            self.log("打开总线失败: {}".format(exc), "err")
            return
        try:
            fn(bus, *args)
        except Exception as exc:                            # noqa: BLE001
            self.log("异常: {}".format(exc), "err")
        finally:
            bus.shutdown()

    # ── 动作 ──
    def on_selftest(self):
        def job(bus):
            t0, n, ids = time.time(), 0, {}
            while time.time() - t0 < 3:
                m = bus.recv(0.05)
                if m:
                    n += 1
                    ids[m.arbitration_id] = ids.get(m.arbitration_id, 0) + 1
            self.log("3s 收到 {} 帧 {}".format(n, {hex(k): v for k, v in ids.items()}), "ok")
            if n == 0:
                self.log("总线上没有帧：检查接线 / 120Ω 终端 / 速率", "warn")
        self._bus_call(job)

    def on_read_did(self):
        def job(bus):
            uds = Uds(IsoTp(bus))
            tp = uds.tp
            for did, name in ((0xF186, "App版本"), (0xF187, "Boot版本"), (0xF190, "分区信息")):
                try:
                    r = uds.read_did(did)
                    if neg(r):
                        self.log("22 {:04X} {} -> NRC {}".format(did, name, nrc_text(r[2])), "err")
                    else:
                        self.log("22 {:04X} {} -> {}".format(did, name, r.hex(" ")), "ok")
                except TimeoutError:
                    self.log("22 {:04X} {} -> 超时（ECU 侧服务未实现？）".format(did, name), "warn")
        self._bus_call(job)

    def on_erase(self):
        def job(bus):
            uds = Uds(IsoTp(bus))
            uds.session(0x02)
            r = uds.routine(0x01, 0xFF00)
            self.log("31 01 FF00 -> " + (("NRC " + nrc_text(r[2])) if neg(r) else r.hex(" ")),
                     "err" if neg(r) else "ok")
        self._bus_call(job)

    def on_reset(self):
        def job(bus):
            uds = Uds(IsoTp(bus))
            uds.ecu_reset(0x01)
            self.log("已发 11 01（复位后不响应属正常）", "ok")
        self._bus_call(job)

    def on_pick(self):
        p = filedialog.askopenfilename(title="选择 SBB_APP.BIN",
                                       filetypes=[("SBB image", "*.BIN *.bin"), ("All", "*.*")])
        if p:
            self.bin_path.set(p)

    def on_flash(self):
        p = self.bin_path.get()
        if not os.path.isfile(p):
            messagebox.showerror("找不到镜像", p)
            return
        if not messagebox.askyesno("确认刷写",
                                   "将把镜像写入 ECU 暂存区并复位搬运。\n\n{}".format(p)):
            return
        self.pb["value"] = 0
        self.btn_flash.config(state="disabled")
        self.status.config(text="刷写中...")
        self.worker = Flasher(self.cfg(), p, self.q)
        self.worker.start()


if __name__ == "__main__":
    App().mainloop()
