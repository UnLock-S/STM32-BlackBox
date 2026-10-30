# 开发环境与工具链 —— 安装配置记录

| 项 | 值 |
|---|---|
| 项目 | STM32F103C8 单板黑匣子（`SBB`） |
| 目标 OS | FreeRTOS（内核版本待定，见 §7） |
| 主机 | Windows 11 · git-bash (MSYS) |
| 记录日期 | 2026-10-12 |
| 状态 | **软件侧全部就位并验证通过；硬件侧待 CH347 接入** |

---

## 0. 当前状态一览

| 环节 | 状态 | 说明 |
|---|---|---|
| ARM 交叉编译工具链 | ✅ 已装并实测 | GNU Arm 14.2.Rel1，编译 Cortex-M3 目标文件通过 |
| 构建系统 | ✅ 已就位 | CMake 4.3.1 + Ninja 1.13.2（均在 PATH） |
| 调试器软件 OpenOCD | ✅ 已装并验证 | xPack 0.12.0-7，**ch347 驱动已编入** |
| OpenOCD 接口配置 | ✅ 已自写 | 上游没有 `ch347.cfg`，本项目补上（见 §2） |
| flash 容量覆盖配置 | ✅ 已就位 | 用官方 `FLASH_SIZE` 变量，无需改安装目录 |
| VS Code 调试扩展 | ✅ 已装 | Cortex-Debug 1.12.1 + peripheral-viewer + rtos-views |
| 串口工具 | ✅ 已装 | pyserial 3.5（两个 python 都装） |
| 串口终端 | ⬜ 用现成的 | Docklight（用户既有），或 VS Code Serial Monitor |
| FreeRTOS 内核 | ✅ 已就位 | FreeRTOS-Kernel **V11.1.0**（commit `dbf7055`） |
| 固件工程骨架 | ✅ 已建 | CMake + Ninja + **自写 GNU 启动文件/链接脚本**（官方包只有 Keil 版） |
| **端到端构建** | ✅ **已实测通过** | 编译链接出 `sbb_app.elf/.hex/.bin` —— 见 §3.4 |
| **CH347 硬件联通** | ⛔ **待做** | 板子未接入；驱动绑定与固件升级待办（见 §4） |

---

## 1. 已安装的软件

| 软件 | 版本 | 安装方式 | 路径 |
|---|---|---|---|
| **GNU Arm Embedded Toolchain** | **14.2.Rel1** | `winget install -e --id Arm.GnuArmEmbeddedToolchain` | `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin` |
| **xPack OpenOCD** | **0.12.0-7** (dev-02228, 2025-10-04) | `winget install -e --id xpack-dev-tools.openocd-xpack` | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\xpack-dev-tools.openocd-xpack_*\xpack-openocd-0.12.0-7\` |
| CMake | 4.3.1 | 既有 | `C:\Program Files\CMake\bin` |
| Ninja | 1.13.2 | 既有 | `%LOCALAPPDATA%\Microsoft\WinGet\Links` |
| VS Code | 既有 | 既有 | 用户级安装 |
| Cortex-Debug | 1.12.1 | `code --install-extension marus25.cortex-debug` | VS Code 扩展目录 |
| pyserial | 3.5 | `pip install pyserial` | Hermes python + `C:\Python314` |
| SEGGER J-Link 套件 | V812g | **既有**（注意：只装了软件，是否有 J-Link 硬件待确认） | `C:\Program Files\SEGGER\JLink_V812g` |

**关于旧工具链**：本机 Documents 下另有一个 ARM GCC 12.2.1，属**别的项目**留下的临时解压，随时可能被清理。

本项目**完全不依赖它** —— `tools/env.sh` 已彻底移除对该路径的引用。工具链位置统一由
「`$SBB_ARM_GCC_BIN` → 已在 PATH → 常见安装位置」自动解析，**不写死任何机器路径**。

---

## 2. 本项目新增的配置

```
fw/
├── tools/
│   ├── env.sh                    环境变量 + sbb-env-check 自检函数
│   ├── openocd.sh                OpenOCD 启动器（自动组搜索路径）
│   └── openocd/
│       ├── interface/ch347.cfg   ★ 自写的 CH347 接口配置
│       ├── sbb-probe.cfg         ★ 一键：CH347 + STM32F1x，容量自动探测
│       ├── sbb-128k.cfg          ★ 一键：CH347 + STM32F1x，强制 128 KB
│       └── README.md             用法 + 报错对照表
├── setup/                        本目录（安装配置记录）
└── build/                        构建输出（空）
```

### 2.1 为什么 `ch347.cfg` 要自己写

上游 OpenOCD 仓库**确实没有** `tcl/interface/ch347.cfg`（master 上实测 404），
但驱动本身已合入。xPack 0.12.0-7 的二进制里**已编入**该驱动，可用下面这条验证：

```bash
openocd -c "adapter list"          # 输出中应含：  ch347  { jtag swd }
```

### 2.2 flash 容量覆盖：**不要改安装目录里的文件**

官方 `target/stm32f1x.cfg` 第 27–33 行本身就支持变量覆盖：

```tcl
if { [info exists FLASH_SIZE] } { set _FLASH_SIZE $FLASH_SIZE } else { set _FLASH_SIZE 0 }
...
flash bank $_FLASHNAME stm32f1x 0x08000000 $_FLASH_SIZE 0 0 $_TARGETNAME
```

所以只要在 `source target/stm32f1x.cfg` **之前** `set FLASH_SIZE 0x20000` 即可。
网上流传的"手改 flash bank 那一行"是因为不知道这个变量 —— 本项目 `sbb-128k.cfg` 用的是正确做法。

---

## 3. 验证记录（实测输出）

### 3.1 交叉编译器实测

```
$ arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -Os -ffreestanding -c probe.c -o probe.o
  ✅ 编译成功
   text    data     bss     dec     hex filename
     44       0       4      48      30 probe.o

$ arm-none-eabi-objdump -d probe.o
00000000 <SysTick_Handler>:
   0:  4a02        ldr r2, [pc, #8]
   2:  6813        ldr r3, [r2, #0]
   4:  3301        adds    r3, #1
   6:  6013        str r3, [r2, #0]
   8:  4770        bx      lr
```
→ Thumb 指令正确，工具链可用。

### 3.2 工具链自检

```
$ source fw/tools/env.sh && sbb-env-check
  ✅ arm-none-eabi-gcc        Arm GNU Toolchain 14.2.Rel1 (Build arm-14.52) 14.2.1 20241119
  ✅ arm-none-eabi-gdb        GNU gdb 15.2.90.20241130-git
  ✅ arm-none-eabi-size       GNU size 2.43.1.20241119
  ✅ openocd                  xPack OpenOCD 0.12.0+dev-02228-ge5888bda3-dirty (2025-10-04)
  ✅ cmake                    cmake version 4.3.1
  ✅ ninja                    1.13.2
  ✅ git                      git version 2.53.0.windows.3
  ✅ python                   Python 3.14.7
  ─────────────
  通过 8 项，缺 0 项
  ✅ OpenOCD ch347 驱动        ch347 { jtag swd }
  ✅ ch347 接口配置            .../fw/tools/openocd/interface/ch347.cfg
```

### 3.3 OpenOCD 配置实测（关键：失败点正是预期的"只差硬件"）

```
$ bash fw/tools/openocd.sh
xPack Open On-Chip Debugger 0.12.0+dev-02228-ge5888bda3-dirty
Info : CH347 SWD mode enabled                      ← transport select swd 生效
Info : Listening on port 6666 for tcl connections   ← 全部配置解析成功
Info : Listening on port 4444 for telnet connections
Error: libusb_open() failed with LIBUSB_ERROR_NOT_FOUND
Error: CH347 not found. Tried VID/PID pairs: 1a86:55dd 1a86:55de 1a86:55e7
Error: CH347 open error
  → 退出码: 1
```

**判读**：配置语法、transport 选择、VID/PID 搜索**全部正确**；
唯一缺的是硬件。`sbb-128k.cfg`（`FLASH_SIZE` 路径）同样干净加载，无 Tcl 报错。

### 3.4 ★ 端到端构建验证（工具链可用的硬证据）

前面 3.1–3.3 只证明"单个工具能跑"。真正的判据是：**能否把 FreeRTOS 工程
编译+汇编+链接成可烧写的 ELF**。

```
$ bash fw/build.sh
[1/9] Building ASM object   app/startup_stm32f103c8.s.obj
[2/9] Building C object     FreeRTOS-Kernel/list.c.obj
[3/9] Building C object     cmsis/system_stm32f10x.c.obj
[4/9] Building C object     app/main.c.obj
[5/9] Building C object     FreeRTOS-Kernel/portable/GCC/ARM_CM3/port.c.obj
[6/9] Building C object     FreeRTOS-Kernel/queue.c.obj
[7/9] Building C object     FreeRTOS-Kernel/tasks.c.obj
[8/9] Linking C executable  sbb_app.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:        4028 B        64 KB      6.15%
             RAM:        3640 B        20 KB     17.77%
   text    data     bss     dec     hex filename
   4016       8    3632    7656    1de8 sbb_app.elf
```

**产物**：`build/sbb_app.elf`（390 KB，含调试信息）· `sbb_app.hex`（11 KB）· `sbb_app.bin`（4028 B）

### 3.5 二进制内容逐项核验

| 检查项 | 结果 |
|---|---|
| `.isr_vector` 位置/大小 | ✅ 落在 `0x08000000`，`0xEC` = **59 × 4 B**（16 内核 + 43 外设中断，数目正确） |
| 向量表第 1 字（初始 SP） | ✅ `0x20005000` = RAM 顶（20 KB） |
| 向量表第 2 字（复位入口） | ✅ `0x08000201` = `Reset_Handler` + Thumb 位 |
| 向量表 offset `0x2C/0x38/0x3C` | ✅ `0x08000C61`(SVC) / `0x08000CB1`(PendSV) / `0x08000DC1`(SysTick) —— **真指向 port.c，不是 Default_Handler** |
| `.data` 的 LMA | ✅ VMA `0x20000000` ← LMA `0x08000FB4`（flash 加载点正确，启动拷贝能对上） |
| 启动流程反汇编 | ✅ `bl SystemInit` → `bl __libc_init_array` → `bl main` → `b .` |
| **★ ADR-10 无动态分配** | ✅ `malloc`/`calloc`/`realloc`/`free`/`pvPortMalloc`/`_sbrk` **符号全无**；MemMang 目标文件 **0 个** |

> 核验方法留痕：`arm-none-eabi-nm -w <symbol>` 查符号、`objdump -h` 查段布局与 LMA、
> `objdump -s -j .isr_vector` 逐字读向量表、`objdump -d` 反汇编启动流程。

---

## 4. 待完成的硬件侧步骤（插上 CH347 后按顺序做）

| # | 步骤 | 判据 / 备注 |
|---|---|---|
| 1 | **CH347 拨码切到 mode 3（UART + JTAG）** | 走 SWD 也一样，必须选含 JTAG 的那档 |
| 2 | 装 **WCH CH347 驱动**（串口那组用 VCP） | 设备管理器里出现一串"USB-SERIAL CH347" |
| 3 | 用 **Zadig** 把 **JTAG 那组接口** 绑 **WinUSB** | CH347T 的 JTAG 是 interface 2，VID:PID = `1a86:55dd` |
| 4 | **升级 CH347T 固件到最新（≥4.41）** | v2.41 **没有 SWD 实现**；旧版只能 bitwise 模式 |
| 5 | 重新跑 `bash fw/tools/openocd.sh` | 应看到 `CH347 SWD mode enabled` + **成功打开设备** + 打印 flash size |

### 4.1 TC-01 容量探测（硬件通了立刻做）

```bash
bash fw/tools/openocd.sh --128k       # 一个终端
telnet localhost 4444                 # 另一个终端
```
```
flash probe 0
mwb 0x08010000 0xA5        # 往 64 KB 之外写一个字节
mdb 0x08010000 1           # 回读
```
- 回读 `0xa5` → 实际 **128 KB** → OTA 走 A/B 双槽（SDD 方案 A）
- 回读 `0xff` → 真的 **64 KB** → OTA 走单槽 + Bootloader 兜底（SDD 方案 B）

### 4.2 三个调试器的取舍（已确认手上有 J-Link V9 / Lauterbach / CH347T）

| 调试器 | 结论 | 依据 |
|---|---|---|
| **J-Link V9** | ✅ **首选** | 上游 OpenOCD **自带** `interface/jlink.cfg`；**不需要 Zadig/WinUSB**（用 SEGGER 驱动）；SWD 远高于 CH347T 的 1 MHz 上限。本机已装 SEGGER JLink_V812g + Ozone V3.50a |
| **CH347T** | ✅ 次选 | 配置已就绪，但 **SWD 硬上限 1 MHz** + 固件版本门槛（见 3.2 / README） |
| **Lauterbach TRACE32** | ⚠️ **这块芯片上用不上** | 见下 |

#### Lauterbach 为什么在 F103C8T6 上发挥不了

1. **F103C8T6 是 LQFP48，没有 GPIOE** → 没有 PE2–PE6。
   而 RM0008（STM32F1 参考手册 §31.17.2 TRACE pin assignment）规定
   **TRACECLK = PE2、TRACED0–3 = PE3–PE6**。
2. **STM32F1 没有 ETB**（片上 trace 缓存）—— RM0008 的 Debug 章节里
   ITM(§31.14)/ETM(§31.15)/TPIU(§31.17) 都有，**没有 ETB 一节**。
   → trace 数据只能从 TPIU 的引脚输出，没有引脚就等于没有 trace。
3. 结论：Lauterbach 最值钱的**指令 trace** 在这块蓝板上根本上不了，
   只剩普通 JTAG/SWD 断点调试 —— 那 J-Link 也能做。

#### Lauterbach 现状（本机实测）

- 已安装：`C:\T32\bin\windows64\t32marm.exe` + `t32start.exe`，`C:\T32\config.t32`
- 已有现成的 STM32F103 资源：`C:\T32\demo\arm\combiprobe\stm32f103\`、
  flash 脚本 `C:\T32\demo\arm\flash\stm32f10x.cmm`
- **但硬件当前不通**：`config.t32` 走以太网找调试器（`PBI=` + `NET` + `NODE=t32`），
  试跑报 `node name (t32) unknown (HOST_NOT_FOUND)` → 调试器没连上/没上电/不在网内。
  `config_usb.t32` 是 USB 版模板。
- TRACE32 **不走 OpenOCD/GDB**，它有自己的软件栈（PowerView）。要跟 Eclipse 系集成得用
  Lauterbach 自己的 TCF 插件或 GDB stub，属于"能凑但不被 ST 支持"的组合。

#### 关于 CubeIDE 对 Lauterbach 的支持

**不原生支持。** STM32CubeIDE 原生只认 **ST-LINK / J-Link / CMSIS-DAP**：

- ST 官方社区原话：*"STM32CubeIDE natively supports only ST-Link, J-Link, and CMSIS-DAP debuggers"*
- CubeIDE for VS Code 的调试类型只有三种：`stlinkgdbtarget` / `openocdtarget` / `jlinkgdbtarget`
- ST 对 ULINKpro 的官方答复可作旁证：*"There is no plan at the moment to implement support for ULINK.
  The current supported probes are ST-LINK ... and SEGGER J-LINK."*

要硬凑只能用 Lauterbach 的 Eclipse 插件（TCF：`http://www.lauterbach.com/eclipse/tcf`）
或手连 TRACE32 的 GDB stub（`SETUP.API.GDB.Enable`）——
但 Lauterbach 官方文档明确说明：**经 GDB stub 时 trace 视图 / MMU 视图 / OS-aware 调试都不支持**，
等于把它的价值砍掉。而且 Lauterbach 自己推荐用 TCF 而非 GDB。

**结论**：为 Lauterbach 装 CubeIDE 是走反了 —— TRACE32 本来就有独立完整的 IDE。

### 4.3 双通道建议

即使有了 J-Link，**CH347 仍然有用**：它那路 UART 正好当**串口日志/命令台**
（正是 SDD 里 CH347 的定位）。所以最优是：

**J-Link 管烧写 + 调试（SWD）· CH347 管 UART 日志**

---

## 5. 踩坑记录（本次实际遇到的，均已解决）

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 1 | `Error: Can't find sbb-probe.cfg` | 本机 **MSYS 路径转换关闭**，`-s /c/Users/...` 传给原生 `openocd.exe` 是无效路径 | `env.sh` 里用 `cygpath -m` 生成 `*_W` 变量，所有 `-s`/`-f` 一律用 Windows 风格路径 |
| 2 | 上游没有 `interface/ch347.cfg` | OpenOCD 只合入了驱动，没提供该板卡配置 | 自写 `interface/ch347.cfg`（含官方文档里的全部前置条件注释） |
| 3 | 网上都说"要手改 flash bank" | 大家不知道官方 cfg 支持 `FLASH_SIZE` 变量 | 用 `set FLASH_SIZE` 覆盖，不动安装目录 |
| 4 | — | **CH347T 固件 ≥2.41 才有可用 SWD；v4.41 起 SWD 上限 1 MHz，且读 AP 后 SWDIO 会误拉高 392 ns** | 写进 `ch347.cfg` 注释与 `openocd/README.md`；必要时在 SWDIO 串电阻 |
| 5 | — | CH347 在 Windows 上**两组接口驱动不同**：JTAG 组要 WinUSB，串口组要 VCP | 写进 §4 步骤 2/3 与 README 报错对照表 |
| 6 | `CMAKE_SYSTEM_NAME` 设了却不生效，CMake 去链接宿主机 exe 失败 | 该变量**必须在第一次 `project()` 之前**生效，写在 `project()` 之后无效 | 移入独立 toolchain 文件 `cmake/arm-none-eabi.cmake`，并在 CMakeLists 里加 `FATAL_ERROR` 强制要求 `-DCMAKE_TOOLCHAIN_FILE` |
| 7 | `undefined reference to '_init'` | `-nostartfiles` 下 `crti.o/crtn.o` 不参与链接，而 newlib 的 `__libc_init_array()` 会调用 `_init` | 补 `app/init_stub.c` 提供空的 `_init`/`_fini`。**不要**改成删掉 `__libc_init_array` 调用 —— 那是埋雷，将来加构造函数会静默失效 |
| 8 | 官方包的 `startup_stm32f10x_md.s` GCC 汇编不了 | 整包启动文件都是 **Keil(MDK/armcc) 语法**（`AREA`/`PROC`/`__main`），全盘 grep `.isr_vector\|.syntax unified` = 0 命中 | 自己写 GNU as 版 `app/startup_stm32f103c8.s`（59 项向量表 + 弱符号默认处理） |
| 9 | `Error: /c/.../build is not a directory` | 与坑 1 同源：`cmake --build` 的位置参数也走原生程序 | `build.sh` 里所有传给 cmake/objcopy/size 的路径统一 `cygpath -m` |

---

## 6. 日常用法速查

```bash
# 载入环境（每个新 shell 一次）
source fw/tools/env.sh
sbb-env-check                    # 自检

# 启动 OpenOCD
bash fw/tools/openocd.sh         # 日常：容量自动探测
bash fw/tools/openocd.sh --128k  # TC-01：强制 128 KB

# 另开终端连 GDB
arm-none-eabi-gdb -ex "target extended-remote :3333" build/xxx.elf

# 串口终端（Docklight 亦可）
python -m serial.tools.miniterm COM<x> 115200
```

---

## 7. 下一步

### 7.1 已完成（本轮）

| # | 事项 | 结果 |
|---|---|---|
| 1 | 获取 FreeRTOS 内核源码 | ✅ FreeRTOS-Kernel **V11.1.0**，只取 `tasks/list/queue` + `portable/GCC/ARM_CM3` + `port.c` |
| 2 | 搭工程骨架 `fw/app/` + `CMakeLists.txt` | ✅ CMake + Ninja，toolchain 文件 `cmake/arm-none-eabi.cmake` |
| 3 | 引入 CMSIS | ✅ 已拷入 `third_party/cmsis/`（`stm32f10x.h`/`system_stm32f10x.c`/`core_cm3.h`） |
| 4 | 写 `FreeRTOSConfig.h` | ✅ 已按 ADR-10 改版：`configSUPPORT_DYNAMIC_ALLOCATION 0`、栈溢出检查方法 2 |
| 5 | 自写 GNU 启动文件 + 链接脚本 | ✅ `startup_stm32f103c8.s`（59 向量）+ `stm32f103c8.ld`（64 K flash / 20 K RAM） |
| 6 | 端到端构建验证 | ✅ 见 §3.4 / §3.5 |

### 7.2 待办

| # | 事项 | 说明 |
|---|---|---|
| 1 | **接 CH347 → 烧写 + 点灯** | 硬件侧见 §4；这是第一个"看得见"的里程碑（PC13 每 500 ms 翻转） |
| 2 | 出 `launch.json` | 模板已备 `fw/.vscode/launch.json.template`，把占位符换掉后改名即生效 |
| 3 | 标准库 FWlib 按需引入 | CMSIS 已有；`FWlib/src` 里的 `stm32f10x_gpio/rcc/usart/spi/i2c/can.c` 按 SDD 引脚表逐个加，**别一次全加** |
| 4 | BSP 分层落地 | 按 SDD §3.3.1 分层图：`bsp_led`(已有雏形) → `bsp_uart` → `bsp_i2c`(OLED) → `bsp_spi`(SD) → `bsp_adc`(C0832C) → `bsp_can`(野火) |
| 5 | 时钟确认 | 现在走 SPL `SetSysClockTo72`（HSE×9），`main` 里已调 `SystemCoreClockUpdate()` 兜底 HSE 起振失败 |
| 6 | SDD 文档同步 | ADR-10 需由「不引入 RTOS」改写为 **「引入 FreeRTOS + 静态分配」**；触发条件 T1 已达成（128 KB 待 TC-01 确认） |

---

## 8. 需要你确认的两件事

1. **有没有 J-Link 探针？** 本机装了 SEGGER J-Link V812g 软件 —— 如果有硬件，建议「J-Link 烧写调试 + CH347 跑 UART」双通道；如果没有，就全走 CH347（受 1 MHz SWD 上限约束，调试体验会慢一些）。
2. **CH347 板子现在方便插上吗？** 插上后我带你走 §4 的 5 步（拨码 → 驱动 → WinUSB 绑定 → 固件升级 → 联通验证），并把 TC-01 容量探测一次性做掉。
