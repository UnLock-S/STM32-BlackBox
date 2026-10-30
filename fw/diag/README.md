# 诊断描述文件（ODX / CDD）

## 文件

| 文件 | 内容 | 用途 |
|---|---|---|
| `diag_table.json` | **诊断唯一真源**：ECU/会话/安全/DID/例程/服务/NRC | 人可读、可 diff、生成器的输入 |
| `SBB_ECU.odx` | 由 JSON 生成的 ODX 2.2.0 结构 | 导入 ODXStudio / CANdelaStudio |

生成：`python3 tools/gen_diag.py`

## 内容（与固件一一对应）

**DID**
| DID | 名称 | 长度 | 来源 |
|---|---|---|---|
| F186 | ActiveDiagnosticSession | 1 B | app_uds.c |
| F187 | BootloaderVersion | 8 B ASCII | diag_handlers.c |
| F190 | PartitionInfo | 16 B | diag_handlers.c |
| F1A0 | StagingStatus | 8 B | diag_handlers.c |

**例程**
| RID | 名称 |
|---|---|
| FF00 | EraseStaging（擦 SD 卡尾暂存区）|
| FF01 | VerifyStaging（校验暂存区镜像）|

**服务**：10 / 11 / 22 / 27 / 31 / 34 / 36 / 37 / 3E
**安全访问**：level 01/02，`key = CRC32(seed || "SBB1")`，3 次失败锁 10 s（NRC 0x37）

## 关于 CDD（重要）

`.cdd` 是 **Vector CANdelaStudio 的私有格式**，没有公开规范，**无法用脚本生成** ✗。

三条可行路径：

1. **ODX → CANdelaStudio**（推荐）
   CANdelaStudio 支持导入 ODX；导入后按提示补齐 ADMIN-DATA 等必填项，另存为 `.cdd`。
   注意：本仓库生成的 ODX 是**结构完整但未做 XSD 严格校验**的版本，
   导入时可能需要手工补少数字段（不影响诊断语义）。

2. **直接照 JSON 在 CANdelaStudio 里建**
   `diag_table.json` 里每个 DID/例程/服务的地址、长度、子功能、NRC 都是全的，
   照着填即可 —— 这条路最稳，不依赖 ODX 导入器。

3. **纯 ODX 工作流**（不碰 CANdelaStudio）
   CANoe / CANoe.DiVa 都直接吃 ODX，ODXStudio 打开后补 ADMIN-DATA 即可用于仿真。

## 局限（诚实说明）

- 生成的 ODX **没有** REQUEST/POS-RESPONSE 的完整物理编码描述（只有 DID 的位长），
  因为本项目的报文布局简单（ISO-TP + 固定字段），复杂负响应/多参数服务需要补 REQUEST 对象。
- 安全访问算法是**台架级**（SECRET 在固件里），ODX 里只描述 seed/key 长度与 NRC，
  不包含密钥本身 ✓（也不该包含）。
