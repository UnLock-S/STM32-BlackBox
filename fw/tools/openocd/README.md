# OpenOCD 配置目录 —— CH347 + STM32F103C8T6

## 这个目录里有什么

| 文件 | 作用 |
|---|---|
| `interface/ch347.cfg` | **自写的 CH347 接口配置**（上游没有这个文件） |
| `sbb-probe.cfg` | 一键：CH347 + STM32F1x，flash 容量**自动探测** ← CH347 日常用这个 |
| `sbb-128k.cfg` | 一键：同上但**强制 128 KB** ← CH347 做 TC-01 用这个 |
| `sbb-jlink.cfg` | 一键：**J-Link** + STM32F1x，容量自动探测 ← J-Link 日常用这个 |
| `sbb-jlink-128k.cfg` | 一键：J-Link + 强制 128 KB ← J-Link 做 TC-01 用这个 |
| `../../tools/openocd.sh` | 启动脚本（自动组好两个 `-s` 搜索路径） |

> 上游 **自带** `interface/jlink.cfg`（`adapter driver jlink`），J-Link 不需要自写配置；
> 只有 ch347 缺文件所以自写了一份。

## 怎么用

```bash
# 用启动脚本（自动处理路径）
bash ../openocd.sh                  # CH347，容量自动探测（默认）
bash ../openocd.sh --128k           # CH347，强制 128 KB（TC-01）
bash ../openocd.sh --jlink          # J-Link，容量自动探测
bash ../openocd.sh --jlink --128k   # J-Link，强制 128 KB

# 或手动
openocd -s . -s "$XPACK_SCRIPTS" -f sbb-jlink.cfg
```

## 调试器怎么选（针对 F103C8T6）

| 调试器 | 可用性 | 说明 |
|---|---|---|
| **J-Link** | ✅ 首选 | 上游配置现成；**不用 Zadig/WinUSB**（走 SEGGER 驱动）；SWD 可跑高频 |
| **CH347T** | ✅ 次选 | 自写配置已就绪；但 **SWD 上限 1 MHz** + 固件版本门槛（见下） |
| **Lauterbach TRACE32** | ⚠️ 用不上 | F103C8T6 是 **LQFP48，无 GPIOE → 无 PE2–PE6 → ETM trace 引脚不存在**，而 STM32F1 又**没有 ETB**。trace 是它的核心价值，在这块芯片上发挥不了；而且它有自己独立的软件栈（PowerView），不走 OpenOCD |

**J-Link V9 特别提醒**：SEGGER 已把 V9 列为 **legacy**（官方原文 "not eligible for support by SEGGER"）。
若是国产克隆件，**千万别用 SEGGER 软件给它升级固件**（可能变砖）；OpenOCD 自己实现协议，一般不检测克隆，实测为准。

连上后 OpenOCD 会开两个端口：
- **4444** telnet（命令台，`telnet localhost 4444`）
- **3333** GDB（`arm-none-eabi-gdb` / VS Code Cortex-Debug 连这里）

## 三个必须记住的前提

1. **CH347T 拨到 mode 3（UART + JTAG）** —— 走 SWD 也一样，拨码必须选含 JTAG 的那档。
2. **CH347T 固件要升级到最新**（≥ 4.41）：
   - v2.41 **根本没有 SWD 实现**
   - v4.41 起 SWD 上限 1 MHz，且读 AP 寄存器后 SWDIO 会被误拉高 392 ns，必要时在 SWDIO 串电阻
3. **Windows 驱动分两路**：
   - JTAG/SWD 那一组接口 → 绑定 **WinUSB (libusb)**（用 Zadig 或 WCH 工具）
   - 串口那一组 → 保留 WCH **VCP 驱动**（给 UART 日志用）
   两组接口的 VID/PID 都是 `1a86:55dd`（CH347T/mode3），但接口号不同（JTAG 是 interface 2）。

## 关于 flash 容量（TC-01）

官方 `target/stm32f1x.cfg` 第 27–33 行**本身就支持 `FLASH_SIZE` 变量覆盖**，所以：

```tcl
set FLASH_SIZE 0x20000      ;# 128 KB，必须在 source target 之前
source [find target/stm32f1x.cfg]
```

**不需要修改 OpenOCD 安装目录里的任何文件** —— 网上流传的"手改 flash bank 那行"是因为不知道这个变量。

## 常见报错对照

| 报错 | 含义 | 处置 |
|---|---|---|
| `Error: no device found` | 板子没插 / 拨码不对 / JTAG 接口没绑 WinUSB | 检查拨码到 mode 3；用 Zadig 绑 WinUSB |
| `Error: libusb_open failed` | 驱动还是 WCH 的，不是 WinUSB | 同上 |
| `Error: CH347T old version ... bitwise mode` | 固件太旧 | 升级固件 |
| `SWD is not implemented` | 固件正好是 2.41 | 升级固件 |
| 读到数据偶发 parity error | v4.41 的 SWDIO 392 ns 毛刺 | SWDIO 串电阻（见 ch347.cfg 注释） |
| `expected id 0x1ba01477, got ...` | 目标没供电或 SWDIO/SWCLK 接反 | 检查 PA13/PA14 与共地 |
