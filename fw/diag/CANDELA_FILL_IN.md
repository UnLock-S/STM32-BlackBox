# CANdelaStudio 手建清单（照抄即可）

数据源：`diag_table.json`（与固件一一对应）。建完另存为 `SBB_ECU.cdd`。

---

## 1. ECU / 通信

| 项 | 值 |
|---|---|
| ECU 名称 | SBB_F103C8T6 |
| 诊断请求 ID | **0x7E0** |
| 诊断响应 ID | **0x7E8** |
| 总线 | CAN 11-bit，1 Mbps |
| ISO-TP | BS=1，STmin=2 ms，最大报文 520 B，每块 512 B |
| P2 | 50 ms |
| P2* | 5000 ms（0x78 挂起时反复续期）|

---

## 2. 会话（0x10）

| 子功能 | 名称 | 解锁后才能做的 |
|---|---|---|
| 0x01 | Default | — |
| 0x02 | Programming | 27/31/34/36/37 |
| 0x03 | Extended | 22 |

> 离开 0x02 会清 `unlocked` 标志（固件行为）

---

## 3. 安全访问（0x27）

| 项 | 值 |
|---|---|
| 级别 | 0x01（请求 seed） / 0x02（送 key）|
| seed 长度 | 4 B |
| key 长度 | 4 B |
| 算法 | `key = CRC32(seed_be ‖ "SBB1")`，CRC32 为标准反射多项式 0xEDB88320 |
| 失败上限 | 3 次 |
| 锁定时间 | 10 s |
| NRC | key 错 → **0x35**；锁定期间 → **0x37** |
| seed 特性 | 一次性（用过即失效），每次请求不同 |

> CANdelaStudio 里选 "Security Access"，级别 1/2，把上面填进去；
> 算法栏可写自定义 DLL 或用 ASCII 备注（仿真时用 `uds_tester.py` 的公式）。

---

## 4. DID（0x22 读）

| DID | 名称 | 长度 | 类型 | 内容 |
|---|---|---|---|---|
| **0xF186** | ActiveDiagnosticSession | 1 B | u8 | 1=Default 2=Programming 3=Extended |
| **0xF187** | BootloaderVersion | 8 B | ASCII | 例 `0.9.0` + 补零 |
| **0xF190** | PartitionInfo | 16 B | byte[] | boot_base(u32) boot_size(u32) app_base(u32) app_size(u32) |
| **0xF1A0** | StagingStatus | 8 B | byte[] | magic(u32) size(u32) |

> 0xF190 / 0xF1A0 建议在 CANdelaStudio 里建成 8 个独立的 u32 子参数（更好看波形），
> 或直接建成 byte[16] / byte[8]（更省事）。

---

## 5. 例程（0x31 01 启动）

| RID | 名称 | 输入 | 输出 |
|---|---|---|---|
| **0xFF00** | EraseStaging | 无 | 无（成功回 71 01 FF 00）|
| **0xFF01** | VerifyStaging | 无 | 无（CRC 不符回 72）|

---

## 6. 刷写服务

| SID | 名称 | 关键约束 |
|---|---|---|
| **0x34** | RequestDownload | 需 0x02 会话 + 已解锁；`dataFormatIdentifier=0x00`、`addressAndLengthFormatIdentifier=0x44`；长度 ≤ 50 KB |
| **0x36** | TransferData | 每块 **512 B**（一个 SD 扇区），块序号 1..N |
| **0x37** | RequestTransferExit | 触发 CRC 校验 → 写升级标志 |
| **0x11** | ECUReset | 只支持 0x01 hardReset；先回 51 01，30 ms 后复位 |
| **0x3E** | TesterPresent | 抑制正响应 0x80 |

---

## 7. 需要建进 CDD 的 NRC

| NRC | 含义 | 出现场景 |
|---|---|---|
| 0x12 | subFunctionNotSupported | 子功能不支持 |
| 0x13 | incorrectMessageLengthOrInvalidFormat | 报文太短 |
| 0x22 | conditionsNotCorrect | **已有下载在进行**（重入保护）|
| 0x24 | requestSequenceError | **块序号错**（该块已落盘）|
| 0x31 | requestOutOfRange | DID/例程不存在、dataFormat 不对 |
| 0x33 | securityAccessDenied | 未解锁 |
| 0x35 | invalidKey | key 错 |
| 0x37 | requiredTimeDelayNotExpired | **安全访问锁定中** |
| 0x72 | generalProgrammingFailure | SD 写失败 / 长度超限 / 镜像校验失败 |
| 0x78 | requestCorrectlyReceivedResponsePending | 擦除/校验耗时 > P2 |

---

## 8. 建完自检

在 CANdelaStudio 的 "Diagnostic Test" 或导出给 CANoe 后，按这个顺序验证：

```
1. 0x10 0x02        -> 50 02
2. 0x27 0x01        -> 67 01 xx xx xx xx
3. 0x27 0x02 key    -> 67 02
4. 0x22 0xF186      -> 62 F1 86 "1.1.0\0\0\0"
5. 0x31 0x01 0xFF00 -> 71 01 FF 00
6. 0x34 0x00 0x44 <len> -> 74 20 02 00
7. 0x36 <seq> <512B>    -> 76 <seq>        (重复 N 次)
8. 0x37             -> 77
9. 0x11 0x01        -> 51 01  (板子随后重启，屏幕回到状态页)
```
第 4 步的版本号来自镜像 0x200 处的指纹（`SBBF` + 1.1.0），与屏幕显示一致。
