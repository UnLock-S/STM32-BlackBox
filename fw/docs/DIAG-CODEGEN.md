# 诊断代码生成链：CDD/ODX → STM32 UDS 服务

> 目标：**CDD 作为输入，脚本生成 STM32 的诊断功能**（DID/例程/服务表），
> 同时兼容标准 UDS on CAN，并支持后期换 ODX 前端。

---

## 1. 先说结论：**不要直接解析 .cdd** ✗

| 格式 | 性质 | 可解析性 |
|---|---|---|
| `.cdd`（Vector CANdelaStudio）| **厂商私有 XML**，无公开 schema，版本间会变 | 脆弱 ✗ 不建议作为生产输入 |
| `.odx/.pdx`（ASAM MCD-2D）| **国际标准**，schema 公开、有 XSD | 可解析 ✓ 但要处理多文档继承/变体 ✗ 工作量大 |
| **自有 diag 表（YAML/JSON）** | 我们定义 | ✓ 简单、可评审、可 diff |

**推荐链路** ✓：
```
CANdelaStudio (.cdd)
        │  导出（工具自带能力，一次性动作）
        ▼
   ODX / PDX  ─┐
               ├──▶  diag_convert.py  ──▶  diag_table.json  ──▶  gen_diag_c.py  ──▶  app_uds_tbl.c/.h
手写 diag.yaml ┘        （前端可换）          （唯一真源）         （生成 C 表）
```
**为什么中间夹一层 JSON** ✓：
- 生成器只认一种格式 ✓ 前端（CDD/ODX/手写）可以换 ✓
- JSON 能**进 git 评审** ✓（CDD 是二进制式 XML，diff 没法看 ✗）
- 出问题能定位在"前端转换"还是"代码生成" ✓

---

## 2. ISO-TP 参数（连续帧模式，标准 UDS on CAN）

| 参数 | 取值 | 说明 |
|---|---|---|
| 帧格式 | 经典 CAN，11 bit | `0x7E0` 请求 / `0x7E8` 响应 |
| 单帧上限 | 7 B | PCI `0x0n` |
| 首帧 | 12 bit 长度（最大 4095） | 我们的镜像 44 KB **超了** ✗ → 见下 |
| 连续帧 | 7 B/帧，SN 0..15 循环 | PCI `0x2S` |
| 流控 | `BS=0`（一次发完）、`STmin=0` | 板子处理快；若丢帧改成 `BS=8, STmin=2` |
| **块大小** | **512 B/次 0x36** | 一个扇区对应一次写入 ✓ 便于掉电续传 ✓ |

**⚠ 关键约束** ✓：ISO-TP 首帧只有 **12 bit 长度（≤4095 B）** ✗
**44 KB 的镜像不可能一次 ISO-TP 传输** ✗ → **必须按 `0x36` 分块（每块 ≤512 B）** ✓
**=> 这正是标准 UDS 刷写的做法** ✓（一次 0x34 声明总长，N 次 0x36 各带 512 B，每次内部再拆 74 个连续帧 ✓）

**每块 512 B 的帧构成** ✓：
```
36 01 <512B>  →  首帧(6B) + 73 个连续帧(7B) = 74 帧  →  ECU 回 76 01（肯定响应）
```
1 Mbps 下 74 帧 ≈ **1.2 ms** ✓ → 44 KB ≈ 86 块 ≈ **0.1 s 纯总线时间** ✓（实际瓶颈在 SD 写 ✓）

---

## 3. `diag_table.json` 结构（生成的唯一输入）

```jsonc
{
  "meta": { "source": "SBB.cdd", "version": "1.0", "p2_ms": 50, "p2star_ms": 5000 },
  "sessions": [
    { "id": 1, "name": "default", "p2_ms": 50 },
    { "id": 2, "name": "programming", "p2_ms": 50, "requires": [] }
  ],
  "security": { "level": 1, "seed_size": 4, "algo": "uart_fixed_v1" },
  "dids": [
    { "id": "F186", "name": "AppSwVersion", "access": ["default","programming"],
      "type": "ascii", "len": 16, "read": "did_read_version" },
    { "id": "F190", "name": "PartitionInfo", "access": ["default"],
      "type": "raw", "len": 16, "read": "did_read_partition" }
  ],
  "routines": [
    { "id": "FF00", "name": "EraseStaging", "access": ["programming"],
      "start": "rtn_erase_staging", "result": "rtn_erase_staging_result" },
    { "id": "FF01", "name": "CheckImage", "access": ["programming"],
      "start": "rtn_check_image", "result": "rtn_check_image_result" }
  ],
  "services": { "10": true, "11": true, "22": true, "27": true, "31": true,
                "34": true, "36": true, "37": true, "3E": true, "85": false }
}
```

---

## 4. 生成器产出（`gen_diag_c.py`）

```
App/app/gen/app_uds_tbl.h     DID/例程 的 ID 宏 + 回调原型
App/app/gen/app_uds_tbl.c     三张只读表（放 Flash，不占 RAM ✓）
    g_did_table[]        { id, access_mask, len, read_fn, write_fn }
    g_routine_table[]    { id, access_mask, start_fn, result_fn }
    g_session_table[]    { id, p2_ms, p2star_ms }
App/app/gen/app_uds_tbl.md    人类可读的对照表（评审用 ✓）
```

**生成器做的事** ✓：
- 校验 DID 唯一性、access 与 session 的匹配 ✓
- 生成 `access_mask` 位图（session × security）✓
- 回调只生成**原型**，实现留在 `App/app/diag_handlers.c` ✓（手写 ✓ 生成不覆盖 ✓）
- 生成 **DID 长度不符 / 未知 DID 的 NRC 分支** ✓（`0x13` / `0x31` ✓）

**不生成的东西**（保持手写 ✓）：ISO-TP、服务机主循环、读写 Flash 的具体实现 ✓

---

## 5. 目录约定

```
fw/
  diag/
    diag_table.json          ← 唯一真源（进 git ✓）
    diag.yaml                ← 手写前端（可选）
    SBB.cdd                  ← 原始输入（只读，存档 ✓）
  tools/
    diag_convert.py          ← CDD/ODX -> diag_table.json
    gen_diag_c.py            ← diag_table.json -> App/app/gen/*
  App/app/
    diag_handlers.c          ← 手写回调实现（生成器不碰 ✓）
    gen/                     ← 生成物（.gitignore 可忽略，构建时生成 ✓）
```

---

## 6. 实施顺序

| 步骤 | 内容 | 产出 |
|---|---|---|
| D1 | `diag_table.json` + `gen_diag_c.py` | 空表也能生成、能编译 ✓ |
| D2 | `app_isotp.c`（连续帧 + 流控）| CANoe/uds_tester 能收到多帧响应 ✓ |
| D3 | `app_uds.c`（服务机用生成的表）| `22 F186` 能读到版本 ✓ |
| D4 | `diag_handlers.c`（读 DID/例程/暂存区）| `31 FF00` 擦暂存区 ✓ |
| D5 | `diag_convert.py`（ODX 前端）| 从 CDD 导出的 ODX 能自动生成 ✓ |

**D5 放最后** ✓ —— 前四步完成后，**有没有 CDD 都能刷写** ✓，CDD 只是"把 DID 表和 NRC 规则自动搬过来" ✓
