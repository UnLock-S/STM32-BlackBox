#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
#  uds_tester.py —— PC 侧 UDS on CAN 测试仪（VN1610 / ISO-TP / ISO 14229）
# ─────────────────────────────────────────────────────────────────────────────
#  用法:
#    python3 tools/uds_tester.py --selftest          # 只测总线连通（收 0x100）
#    python3 tools/uds_tester.py --did               # 读 DID (22 F186/F187/F190)
#    python3 tools/uds_tester.py --session 2         # 10 02 进编程会话
#    python3 tools/uds_tester.py --flash out/SBB_APP.BIN   # 完整刷写流程
#    python3 tools/uds_tester.py --erase             # 31 01 FF00 擦暂存区
#
#  依赖: pip install python-can
#  硬件: Vector VN1610, CH1, 1 Mbps（与板子一致）
# ─────────────────────────────────────────────────────────────────────────────
import argparse
import struct
import sys
import time
import zlib

import can

REQ_ID = 0x7E0          # 测试仪 -> ECU
RSP_ID = 0x7E8          # ECU -> 测试仪
P2_MS = 5000            # 服务端处理时间（放宽：SD 写卡偶发更慢）
P2STAR_MS = 5000        # 0x78 之后的延长

# ── ISO-TP (ISO 15765-2) ────────────────────────────────────────────────────
class IsoTp:
    def __init__(self, bus, req_id=REQ_ID, rsp_id=RSP_ID, timeout=1.0):
        self.bus = bus
        self.req_id = req_id
        self.rsp_id = rsp_id
        self.timeout = timeout

    def _tx(self, data):
        """发一帧。★ 这里必须重试：python-can/Vector 在 TX 邮箱满时会静默丢帧，
        之前没检查 -> 连续帧发到一半就断了，而循环还在继续。"""
        msg = can.Message(arbitration_id=self.req_id, is_extended_id=False,
                          data=bytes(data))
        for attempt in range(20):
            try:
                self.bus.send(msg, timeout=0.05)
                return
            except can.CanError:
                time.sleep(0.002)
        raise TimeoutError("发送失败（20 次重试）: " + msg.data.hex(" "))

    def _rx_frame(self, deadline):
        while time.time() < deadline:
            m = self.bus.recv(0.05)
            if m is not None and m.arbitration_id == self.rsp_id:
                return bytes(m.data)
        return None

    def _wait_fc(self, deadline):
        """等流控帧：忽略非 0x30 的 0x7E8 帧（可能是上一次请求的残留响应）"""
        while time.time() < deadline:
            f = self._rx_frame(deadline)
            if f is None:
                return None
            if (f[0] & 0xF0) == 0x30:
                return f
        return None

    def send(self, payload):
        """发送一个 UDS 请求，返回完整响应字节（含 SID）。"""
        payload = bytes(payload)
        n = len(payload)

        if n <= 7:                                  # 单帧
            self._tx([0x00 | n] + list(payload))
        else:                                       # 首帧 + 连续帧
            self._tx([0x10 | ((n >> 8) & 0x0F), n & 0xFF] + list(payload[:6]))
            fc = self._wait_fc(time.time() + self.timeout)
            if fc is None:
                raise TimeoutError("未收到流控帧 (FC)")
            bs = fc[1]                      # 0 = 一次发完
            stmin = fc[2]                   # 0..0x7F = 毫秒
            seq = 1
            off = 6
            sent_in_block = 0
            while off < n:
                chunk = payload[off:off + 7]
                self._tx([0x20 | (seq & 0x0F)] + list(chunk))
                off += len(chunk)
                seq += 1
                sent_in_block += 1
                if off < n:
                    # 下限 2 ms：板子 CAN 任务 1 ms 轮询 + FIFO 3 深，
                    # 全速连发会丢帧 -> 序号错 -> 整条消息作废（实测踩过 ✗）
                    time.sleep(max(min(stmin, 0x7F), 5) / 1000.0)
                if bs and sent_in_block >= bs and off < n:
                    fc = self._wait_fc(time.time() + self.timeout)   # 等下一条 FC
                    if fc is None:
                        raise TimeoutError("块间隔的流控帧丢失")
                    bs = fc[1]
                    stmin = fc[2]
                    sent_in_block = 0

        # 收响应
        deadline = time.time() + self.timeout
        f = self._rx_frame(deadline)
        if f is None:
            raise TimeoutError("响应超时")
        pci = f[0] >> 4
        if pci == 0:                                # 单帧
            ln = f[0] & 0x0F
            return f[1:1 + ln]
        if pci == 1:                                # 首帧
            total = ((f[0] & 0x0F) << 8) | f[1]
            buf = bytearray(f[2:])
            # 送流控：CTS, BS=0, STmin=0
            self._tx([0x30, 0x00, 0x00])
            seq = 1
            while len(buf) < total:
                c = self._rx_frame(time.time() + self.timeout)
                if c is None:
                    raise TimeoutError("连续帧超时")
                if (c[0] & 0xF0) != 0x20 or (c[0] & 0x0F) != (seq & 0x0F):
                    raise ValueError("连续帧序号错: 0x%02X" % c[0])
                buf.extend(c[1:])
                seq += 1
            return bytes(buf[:total])
        raise ValueError("未知 PCI 0x%X (data=%s)" % (pci, f.hex()))


def neg(resp):
    return resp[0] == 0x7F


def nrc_text(code):
    return {
        0x10: "generalReject", 0x11: "serviceNotSupported", 0x12: "subFunctionNotSupported",
        0x13: "incorrectMessageLength", 0x22: "conditionsNotCorrect",
        0x24: "requestSequenceError", 0x31: "requestOutOfRange",
        0x33: "securityAccessDenied", 0x35: "invalidKey",
        0x72: "generalProgrammingFailure", 0x78: "responsePending",
    }.get(code, "0x%02X" % code)


class Uds:
    def __init__(self, tp):
        self.tp = tp

    def _req(self, payload, allow_pending=True):
        """发请求；自动处理 0x78（responsePending）。"""
        while True:
            r = self.tp.send(payload)
            if allow_pending and neg(r) and len(r) >= 3 and r[2] == 0x78:
                self.tp.timeout = P2STAR_MS / 1000.0
                continue
            self.tp.timeout = P2_MS / 1000.0
            return r

    # ── 服务 ──
    def session(self, s):
        return self._req([0x10, s])

    def read_did(self, did):
        return self._req([0x22, (did >> 8) & 0xFF, did & 0xFF])

    def routine(self, sub, rid):
        return self._req([0x31, sub, (rid >> 8) & 0xFF, rid & 0xFF])

    def security_seed(self):
        return self._req([0x27, 0x01])

    def security_key(self, key: bytes):
        return self._req([0x27, 0x02] + list(key))

    def request_download(self, addr, size):
        # 34 | dataFormatId(0x00=未压缩/未加密) | addressAndLengthFormatId(0x44 = 4字节地址+4字节长度)
        return self._req([0x34, 0x00, 0x44] +
                         list(struct.pack(">I", addr)) + list(struct.pack(">I", size)))

    def transfer_data(self, seq, data):
        return self._req([0x36, seq & 0xFF] + list(data))

    def transfer_exit(self):
        return self._req([0x37])

    def ecu_reset(self, kind=0x01):
        try:
            return self._req([0x11, kind])
        except TimeoutError:
            return b""                     # 复位后不响应是正常的


def key_from_seed(seed: bytes) -> bytes:
    """G5: key = CRC32(seed_be || SECRET)。

    与固件 diag_handlers.c 的 sec_crc32(seed||"SBB1") 完全一致。
    SECRET 是台架级共享密钥（固件里同值），量产应改为每 ECU 唯一。
    """
    import struct
    import zlib
    secret = b"SBB1"
    return struct.pack(">I", zlib.crc32(seed[:4] + secret) & 0xFFFFFFFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--serial", type=int, default=744342)
    ap.add_argument("--bitrate", type=int, default=1000000)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--did", action="store_true")
    ap.add_argument("--session", type=int, default=0)
    ap.add_argument("--erase", action="store_true")
    ap.add_argument("--flash", metavar="BIN")
    a = ap.parse_args()

    bus = can.Bus(interface="vector", channel=a.channel, bitrate=a.bitrate,
                  serial=a.serial, app_name="sbb_uds", receive_own_messages=False)
    tp = IsoTp(bus)
    uds = Uds(tp)
    print("✓ VN1610 CH{} @ {} bps  (req 0x{:03X} / rsp 0x{:03X})".format(
        a.channel, a.bitrate, REQ_ID, RSP_ID))

    try:
        if a.selftest:
            t0 = time.time()
            n = 0
            ids = {}
            while time.time() - t0 < 3:
                m = bus.recv(0.05)
                if m:
                    n += 1
                    ids[m.arbitration_id] = ids.get(m.arbitration_id, 0) + 1
            print("  3s 收到 {} 帧 {}".format(n, {hex(k): v for k, v in ids.items()}))
            return 0

        if a.session:
            r = uds.session(a.session)
            print("  10 0{} -> {}".format(a.session, r.hex(" ") if r else "(无响应)"))
            if neg(r):
                print("     NRC: " + nrc_text(r[2]))
            return 0 if (r and not neg(r)) else 1

        if a.did:
            for did, name in ((0xF186, "App版本"), (0xF187, "Boot版本"), (0xF190, "分区信息")):
                try:
                    r = uds.read_did(did)
                    print("  22 {:04X} ({}) -> {}".format(did, name, r.hex(" ")))
                except TimeoutError:
                    print("  22 {:04X} ({}) -> 超时（服务未实现？）".format(did, name))
            return 0

        if a.erase:
            r = uds.routine(0x01, 0xFF00)
            print("  31 01 FF00 (擦暂存区) -> {}".format(r.hex(" ") if r else "(无响应)"))
            return 0 if (r and not neg(r)) else 1

        if a.flash:
            raw = open(a.flash, "rb").read()
            if len(raw) < 32:
                print("✗ 镜像太短"); return 1
            magic = raw[:8]
            ver, size, crc, base, hcrc = struct.unpack("<IIIII", raw[8:28])
            calc = zlib.crc32(raw[:24]) & 0xFFFFFFFF
            print("  镜像: {} B  crc=0x{:08X} base=0x{:08X} {}".format(
                size, crc, base, "✓" if (magic.startswith(b"SBBIMG") and calc == hcrc) else "✗"))
            if not (magic.startswith(b"SBBIMG") and calc == hcrc):
                return 1

            print("  [1] 10 02 ->", uds.session(0x02).hex(" "))
            print("  [2] 27 01 ->", uds.security_seed().hex(" "))
            seed = uds.security_seed()[2:]
            key = key_from_seed(seed)
            print("      seed={} key={}".format(seed.hex(" "), key.hex(" ")))
            print("  [3] 27 02 ->", uds.security_key(key).hex(" "))
            print("  [4] 31 01 FF00 ->", uds.routine(0x01, 0xFF00).hex(" "))
            print("  [5] 34 ->", uds.request_download(0, len(raw)).hex(" "))

            seq = 1
            off = 0
            t0 = time.time()
            def written_bytes():
                try:
                    r = uds.read_did(0xF1A0)
                    if not neg(r) and len(r) >= 10:
                        return int.from_bytes(r[7:11], "big")  # 62 F1 A0 | size(3..6) | written(7..10)
                except Exception:
                    pass
                return -1

            retries = 0
            while off < len(raw):
                chunk = raw[off:off + 512]
                r = None
                for attempt in range(6):
                    try:
                        r = uds.transfer_data(seq, chunk)
                        break
                    except TimeoutError:
                        # ★ 响应丢了：直接重发同一块。
                        #   若块实际已落盘，板子会回 NRC 0x24（序号错）
                        #   那说明该块已经写进去了 -> 当作成功继续 ✓
                        if attempt >= 5:
                            print("      ✗ 第 {} 块重试耗尽".format(seq))
                            return 1
                        print("      ⚠ 第 {} 块响应丢失，重发 ({}/3)".format(seq, attempt + 1))
                if r is None:
                    return 1
                if neg(r) and len(r) >= 3 and r[2] == 0x24:
                    print("      (序号错=该块已落盘，继续)")
                    r = bytes([0x76, seq & 0xFF])
                if neg(r):
                    print("      ✗ 0x36 seq={} NRC={}".format(seq, nrc_text(r[2])))
                    return 1
                off += len(chunk)
                seq += 1
                if seq % 20 == 0 or off >= len(raw):
                    print("      {}/{} B  {:.1f}%".format(off, len(raw), off * 100.0 / len(raw)))
            dt = time.time() - t0
            print("      传输完成 {:.1f}s  {:.1f} KB/s".format(dt, len(raw) / 1024.0 / dt))

            print("  [6] 37 ->", uds.transfer_exit().hex(" "))
            print("  [7] 11 01 (复位) ->", uds.ecu_reset(0x01).hex(" ") or "(无响应=正常)")
            print("\n  => 复位后由 Bootloader 搬运；10 秒后可用 --did 读版本确认")
            return 0
    finally:
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
