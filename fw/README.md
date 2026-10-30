# fw —— STM32F103C8 单板黑匣子 固件工作区

> 目标 OS：**FreeRTOS**
> 设计基线在 `../req/`（系统设计文档）与 `../Docs/STM32F103C8-单板黑匣子/`（工作文档 + 图纸）。
> 本目录**自包含**：单独打包即可二次开发与编译。

## ★ 自包含设计（本目录可直接打包分发）

| 特性 | 做法 |
|---|---|
| **无外部源码依赖** | FreeRTOS 与 CMSIS 全部**内置在 `third_party/`**，不靠 clone / 下载 |
| **无绝对路径 / 无机器名** | 源码与构建脚本里没有任何含用户名的路径；全部基于 `CMAKE_CURRENT_SOURCE_DIR` 与 `${BASH_SOURCE[0]}` 解析 |
| **工具链位置自动解析** | `tools/env.sh` 按「环境变量 → 已在 PATH → 厂商常见安装位置」三级查找，任一项都可用 `SBB_ARM_GCC_BIN` / `SBB_OPENOCD_BIN` / `SBB_OPENOCD_SCRIPTS` 覆盖 |
| **跨平台** | Windows/git-bash · Linux · macOS 均可构建（Windows 下自动用 `cygpath -m` 产出原生程序能吃的路径；其他平台是恒等变换） |
| **体积** | 整目录 **3.3 MB**（FreeRTOS 从 20 MB 精简到 1.4 MB，见 `third_party/FreeRTOS-Kernel/VENDORED.md`） |

**唯一的外部前提**：机器上装有 ARM GNU Toolchain 与 CMake+Ninja（编译器本身无法内置）。
两者都不在默认位置时，用环境变量指定即可，**不需要改任何源码**。

### 自包含验证（已实测）

把整个 `fw/` 复制到一个全新目录、**故意不带 `build/`**，然后从零构建：

```
$ cp -r fw /tmp/fw-selftest && rm -rf /tmp/fw-selftest/build
$ cd /tmp/fw-selftest && bash build.sh
[sbb] CMake 配置中…
[sbb] 构建中…
[1/10] Building ASM object   app/startup_stm32f103c8.s.obj
...
[9/10] Linking C executable  sbb_app.elf
           FLASH:        4028 B        64 KB      6.15%
```

✅ 编译链接全部通过，**FLASH 4028 B 与原目录逐字节一致** → 确认无外部依赖。

---

## 当前状态

| 部分 | 状态 |
|---|---|
| 工具链与环境 | ✅ 已装已配 —— 见 `setup/TOOLCHAIN-SETUP.md` |
| FreeRTOS + CMSIS | ✅ 内置（FreeRTOS **V11.1.0**，commit `dbf7055`） |
| 固件工程骨架 | ✅ 已能构建：`bash build.sh` → `sbb_app.elf/.hex/.bin` |
| OpenOCD 配置 | ✅ 自写 CH347 接口配置；**实测已能打开探针**（Chip version 5.44） |
| MCU 侧硬件联通 | ⏳ 只剩 SWD 接线（`CK`/`SWC` ↔ `TCK`/`TMS` + 共地） |
| 业务功能（OLED/SD/CAN/ADC） | ⬜ 未开始（BSP 按 SDD 引脚表逐个加） |

---

## 目录

```
fw/
├── README.md                      本文件
├── build.sh                       ★ 一键构建（Windows/Linux/macOS 通用）
├── CMakeLists.txt                 工程定义（全相对路径）
├── .gitignore
├── cmake/
│   └── arm-none-eabi.cmake        ★ 交叉编译 toolchain 文件
├── app/
│   ├── main.c                     FreeRTOS 双任务 + PC13 LED（全静态创建）
│   ├── FreeRTOSConfig.h           ★ ADR-10：动态分配已关闭
│   ├── startup_stm32f103c8.s      ★ 自写 GNU as 启动文件（官方包只有 Keil 版）
│   ├── stm32f103c8.ld             ★ 链接脚本（64K flash / 20K RAM）
│   └── init_stub.c                _init/_fini 空实现
├── third_party/                   ★ 内置依赖（打包即用）
│   ├── FreeRTOS-Kernel/           V11.1.0 精简版 1.4 MB —— 见其 VENDORED.md
│   └── cmsis/                     STM32F10x CMSIS —— 见其 ORIGIN.md
├── setup/
│   └── TOOLCHAIN-SETUP.md         ★ 环境安装配置记录（含踩坑与验证证据）
├── tools/
│   ├── env.sh                     环境解析 + `sbb-env-check` 自检
│   ├── openocd.sh                 OpenOCD 启动器
│   └── openocd/
│       ├── interface/ch347.cfg    自写的 CH347 接口配置（上游没有）
│       ├── sbb-probe.cfg          CH347 · 容量自动探测（日常）
│       ├── sbb-128k.cfg           CH347 · 强制 128 KB（TC-01）
│       ├── sbb-jlink.cfg          J-Link · 容量自动探测
│       ├── sbb-jlink-128k.cfg     J-Link · 强制 128 KB
│       └── README.md              调试器选择 + 报错对照表
├── .vscode/
│   └── launch.json.template       Cortex-Debug 配置（出 ELF 后改名生效）
└── build/                         构建输出（已 gitignore）
```

---

## 上手命令

```bash
source fw/tools/env.sh && sbb-env-check     # 1) 环境自检
bash   fw/build.sh                          # 2) 构建（约 3 秒）
bash   fw/tools/openocd.sh                  # 3) 启动调试器（CH347，默认）
bash   fw/tools/openocd.sh --jlink --128k   #    或 J-Link + 强制 128 KB
python -m serial.tools.miniterm COM<x> 115200   # 4) 看 UART 日志（Docklight 亦可）
```

**构建结果**：FLASH **4028 B / 64 KB (6.15%)** · RAM **3640 B / 20 KB (17.77%)**

---

## 关键约定

- **不用动态内存**（ADR-10）：`configSUPPORT_DYNAMIC_ALLOCATION = 0`，全部 `xTaskCreateStatic`，
  二进制里 `malloc/calloc/realloc/free/pvPortMalloc/_sbrk` **符号全无**（已核验）
- **换 OS 只改一层**：App 层与服务层只调 `sched_*`，与 RTOS 解耦
- **文档惯例**：`archive/` 只移动不删除；需求 ID 一经分配永不复用

## 下一步

| # | 事项 |
|---|---|
| 1 | SWD 接线：CH347 `TCK`→C8T6 `CLK`、`TMS`→`SWC`、`GND`→`GND`；接完跑 `bash fw/tools/openocd.sh` |
| 2 | **TC-01**：`bash fw/tools/openocd.sh --128k` + telnet 4444 里写 `0x08010000` 回读 → 定死 64/128 KB |
| 3 | `launch.json` 生效（把 `launch.json.template` 的占位符换掉后改名） |
| 4 | BSP 分层：`bsp_uart` → `bsp_i2c`(OLED) → `bsp_spi`(SD) → `bsp_adc`(C0832C) → `bsp_can`(野火) |
