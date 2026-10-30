# STM32-BlackBox

基于 STM32F103C8T6 的单板黑匣子工程，包含 FreeRTOS 固件、CubeMX 配置、Bootloader、Python 上位机工具及 CANoe 测试配置。应用涉及 CAN、模拟量采集、OLED、SD 卡记录和固件升级；功能完成度以实际源码和目标板验证结果为准。

A single-board black box project based on the STM32F103C8T6, including FreeRTOS firmware, CubeMX configuration, a bootloader, Python host tools, and a CANoe test configuration. The application covers CAN, analog data acquisition, OLED display, SD card logging, and firmware updates. Feature completion is subject to the actual source code and verification on the target board.

## 许可 / Licensing

项目自有原创代码和文档采用 [MIT License](LICENSE)。第三方组件继续遵循各自许可：FreeRTOS 内核为 MIT，CMSIS 为 Apache-2.0，STM32 HAL、CMSIS-RTOS 适配层及部分 ST 链接脚本为 BSD-3-Clause。上位机外部依赖 `python-can` 为 LGPL-3.0-only，`pyserial` 为 BSD-3-Clause。

Original project-owned code and documentation are available under the [MIT License](LICENSE). Third-party components retain their respective licenses: MIT for the FreeRTOS kernel, Apache-2.0 for CMSIS, and BSD-3-Clause for STM32 HAL, the CMSIS-RTOS adapter, and certain ST linker scripts. External host dependencies use LGPL-3.0-only for `python-can` and BSD-3-Clause for `pyserial`.

项目 MIT 许可不覆盖第三方或来源待核实内容。组件路径、许可全文、分发注意事项及尚待核实的 CubeMX/Vector/字模来源，见[第三方许可与来源清单](THIRD_PARTY_NOTICES.md)和[许可目录](licenses/README.md)。

The project MIT License excludes third-party content and content with unresolved provenance. See the [third-party inventory](THIRD_PARTY_NOTICES.md) and [license directory](licenses/README.md) for component paths, license texts, distribution considerations, and unresolved CubeMX, Vector, and glyph provenance.

## 工程布局 / Project Layout

| 路径 / Path | 中文说明 | English Description |
| --- | --- | --- |
| `fw/App/` | 应用、板级驱动、调度与链接脚本 | Application, board drivers, scheduling, and linker script |
| `fw/boot/` | Bootloader 与升级处理 | Bootloader and update handling |
| `fw/cubemx/` | CubeMX `.ioc`、生成代码、STM32 HAL/CMSIS 和 FreeRTOS 源码 | CubeMX `.ioc`, generated code, and STM32 HAL/CMSIS and FreeRTOS sources |
| `fw/cmake/`, `fw/CMakeLists.txt` | ARM GNU / CMake / Ninja 构建配置 | ARM GNU / CMake / Ninja build configuration |
| `fw/tools/` | Python 上位机、UDS 测试、镜像打包、串口和调试脚本 | Python host tools, UDS tests, image packaging, serial tools, and debug scripts |
| `fw/diag/` | 诊断 JSON 配置及项目 ODX 描述 | Diagnostic JSON configuration and project ODX description |
| `fw/jlink/` | J-Flash 工程 | J-Flash project |
| `fw/setup/TOOLCHAIN-SETUP.md` | 工具链安装记录 | Toolchain setup notes |
| `fw/docs/WIRING.md` | 板级接线说明 | Board wiring guide |
| `TestEnv/CANoe/` | CANoe 测试配置 | CANoe test configuration |

本仓库只维护可开发的源码、配置和工具环境。硬件资料包、需求文档、工作记录、编译结果、运行日志、上传镜像、CANoe 会话状态和本机编辑器配置留在本地，不纳入版本管理。第三方源码保留各自目录中的许可证；仓库不包含编译器、Vector 驱动或商业软件安装包。

This repository maintains development sources, configuration, and tooling. Hardware reference packages, requirements documents, working notes, build outputs, runtime logs, uploaded images, CANoe session state, and local editor settings remain local and are excluded from version control. Third-party sources retain the licenses in their respective directories. Compilers, Vector drivers, and commercial software installers are not included.

## 外设引脚定义 / Peripheral Pin Assignments

以下按外设模块分组，信号方向均以 MCU 为参照：`OUT` = MCU 输出，`IN` = MCU 输入，`I/O` = 双向。芯片脚号为 **STM32F103C8T6 LQFP48 封装脚号，不是开发板排针序号**。

The tables below are grouped by peripheral. Directions are relative to the MCU: `OUT` = MCU output, `IN` = MCU input, and `I/O` = bidirectional. Package pin numbers refer to the **STM32F103C8T6 LQFP48 package, not the development board header positions**.

核对依据：[CubeMX 配置](fw/cubemx/sbb-f103.ioc)、[封装引脚表](fw/cubemx/pinout-verified.csv)、[GPIO 宏定义](fw/cubemx/Core/Inc/main.h)、[外设初始化](fw/cubemx/Core/Src/stm32f1xx_hal_msp.c)及 `fw/App/bsp/` 驱动。模块供电与接线沿用[接线文档](fw/docs/WIRING.md)；本表不代表新增实板验证。

Cross-checked against the linked CubeMX configuration, package pin table, GPIO definitions, peripheral initialization, and the drivers in `fw/App/bsp/`. Module power and wiring follow the [wiring guide](fw/docs/WIRING.md). This table does not imply additional board testing.

### CAN 收发器 / CAN Transceiver

| MCU 引脚 / Pin | 封装脚号 / Package Pin | 信号 / Signal | 方向 / Direction | 模块连接 / Module Connection |
| --- | --- | --- | --- | --- |
| PB9 | 46 | `CAN_TX` | OUT | 收发器 `TXD` / Transceiver `TXD` |
| PB8 | 45 | `CAN_RX` | IN | 收发器 `RXD` / Transceiver `RXD` |
| 3V3 | — | VIO | 电源 / Power | 当前野火模块 `VIO` / `VIO` on the current Wildfire module |
| 5V | — | VCC | 电源 / Power | 模块 `5V` 主电源 / Module `5V` main supply |
| GND | — | GND | 地 / Ground | 模块 `GND` / Module ground |

CAN1 使用 **PB8/PB9 重映射**，当前配置为 1 Mbps。`CANH`、`CANL` 从收发器连接至 CAN 总线，不直接接 MCU；总线终端按实际拓扑配置。上表电源接法针对现有模块，其他模块按其实际接口定义连接。

CAN1 uses **PB8/PB9 remapping** and is currently configured for 1 Mbps. Connect the transceiver's `CANH` and `CANL` to the CAN bus, rather than directly to the MCU, and configure termination for the actual topology. The power connections above apply to the current module; other modules require their own interface definitions.

### SD 卡 / SD Card — SPI1

| MCU 引脚 / Pin | 封装脚号 / Package Pin | 信号 / Signal | 方向 / Direction | 卡座连接 / Socket Connection |
| --- | --- | --- | --- | --- |
| PA4 | 14 | `SD_CS` | OUT | `CS`，软件片选、低有效 / Software chip select, active low |
| PA5 | 15 | `SPI1_SCK` | OUT | `SCK` / `CLK` |
| PA6 | 16 | `SPI1_MISO` | IN | `MISO` / `DO` |
| PA7 | 17 | `SPI1_MOSI` | OUT | `MOSI` / `DI` |
| 3V3 | — | VCC | 电源 / Power | 当前卡座 `VCC` / Current socket `VCC` |
| GND | — | GND | 地 / Ground | 卡座 `GND` / Socket ground |

使用 SPI Mode 0；PA4 是普通 GPIO，不使用硬件 NSS。现有卡座使用 3.3 V，带稳压或电平转换的其他模块需按模块定义核对。

SPI Mode 0 is used. PA4 is a regular GPIO, with no hardware NSS. The current socket uses 3.3 V; check the specifications of other modules with regulators or level translators.

### OLED 显示屏 / OLED Display — I²C1

| MCU 引脚 / Pin | 封装脚号 / Package Pin | 信号 / Signal | 方向 / Direction | OLED 连接 / OLED Connection |
| --- | --- | --- | --- | --- |
| PB6 | 42 | `I2C1_SCL` | OUT，开漏 / Open-drain | `SCL` |
| PB7 | 43 | `I2C1_SDA` | I/O，开漏 / Open-drain | `SDA` |
| 3V3 | — | VCC | 电源 / Power | `VCC` |
| GND | — | GND | 地 / Ground | `GND` |

当前为 SSD1306 128×64 显示屏，驱动默认地址 `0x3C`。SCL/SDA 使用外部上拉至 3.3 V。当前生成代码将 I²C 初始化为 **25 kHz**，旧接线文档中的 100 kHz 描述不代表当前初始化值。

The current display is an SSD1306 128×64, with a default driver address of `0x3C`. SCL/SDA use external pull-ups to 3.3 V. The current generated code initializes I²C at **25 kHz**; the 100 kHz description in the older wiring guide does not reflect the current initialization value.

### 外部 ADC 与旋钮 / External ADC and Potentiometer — C0832C / ADC0832

| ADC 信号 / Signal | ADC 芯片脚号 / IC Pin | MCU 引脚 / Pin | MCU 封装脚号 / Package Pin | 方向与接法 / Direction and Connection |
| --- | --- | --- | --- | --- |
| `CS` | 1 | PB12 | 25 | OUT，`C0832_CS`，低有效 / Active low |
| `CH0` | 2 | — | — | 接电位器滑臂 / Connect to potentiometer wiper |
| `CH1` | 3 | — | — | 当前未用，接 GND / Currently unused, connect to GND |
| `GND` | 4 | GND | — | 共地 / Common ground |
| `DI` | 5 | PB14 | 27 | OUT，`C0832_DI`，MCU → ADC |
| `DO` | 6 | PB15 | 28 | IN，`C0832_DO`，ADC → MCU |
| `CLK` | 7 | PB13 | 26 | OUT，`C0832_CLK` |
| `VCC` | 8 | 5V | — | 当前 ADC 供电 / Current ADC supply |

这四根数字信号使用 **GPIO 软件时序，不是硬件 SPI2**。ADC 芯片的 `DI` 是输入、`DO` 是输出，勿接反。现有驱动接线定义为电位器两端接 5V/GND、滑臂接 CH0，CH1 接地；这里的 5V 接法仅适用于当前外部 ADC，不能套用到 MCU 自带 ADC 输入。

These four digital signals use **GPIO bit-banging, not hardware SPI2**. `DI` is the ADC input and `DO` is its output. The existing driver specifies a potentiometer connected across 5V/GND, its wiper connected to CH0, and CH1 grounded. This 5V wiring applies to the current external ADC and must not be applied to the MCU's internal ADC inputs.

### USB-UART 调试串口 / USB-UART Debug Console — USART1

| MCU 引脚 / Pin | 封装脚号 / Package Pin | 信号 / Signal | 方向 / Direction | 转接器连接 / Adapter Connection |
| --- | --- | --- | --- | --- |
| PA9 | 30 | `USART1_TX` | OUT | 转接器 `RXD` / Adapter `RXD` |
| PA10 | 31 | `USART1_RX` | IN | 转接器 `TXD` / Adapter `TXD` |
| GND | — | GND | 地 / Ground | 转接器 `GND` / Adapter ground |

串口参数为 **115200、8N1**。TX/RX 交叉连接，转接器使用 3.3 V 逻辑电平；开发板独立供电时无需再连接转接器电源输出。

The console uses **115200 baud, 8N1**. Cross-connect TX/RX and use 3.3 V logic levels. When the board is powered separately, no adapter power output connection is required.

### SWD 调试与复位 / SWD Debug and Reset

| 板端引脚 / Board Pin | 封装脚号 / Package Pin | 信号 / Signal | 探针连接 / Probe Connection |
| --- | --- | --- | --- |
| PA13 | 34 | `SWDIO` | `SWDIO`，双向数据 / Bidirectional data |
| PA14 | 37 | `SWCLK` | `SWCLK`，探针 → MCU / Probe → MCU |
| NRST | 7 | 低有效复位 / Active-low reset | `nRESET`，用于硬件复位 / For hardware reset |
| 3V3 | — | 目标电平参考 / Target voltage reference | 探针 `VTref` 输入 / Probe `VTref` input |
| GND | — | 地 / Ground | 探针 `GND` / Probe ground |

`VTref` 是探针的电平参考输入，不能当作通用的供电输出；具体探针的供电脚需另行核对。工程保留 SWD，关闭 JTAG。

`VTref` is the probe's voltage reference input, not a general power output. Check any probe power output separately. The project keeps SWD enabled and disables JTAG.

### 板载 LED / Onboard LED

| MCU 引脚 / Pin | 封装脚号 / Package Pin | 标签 / Label | 功能 / Function |
| --- | --- | --- | --- |
| PC13 | 2 | `LED_Pin` | 低电平点亮，高电平熄灭 / Low = on, high = off |

所有外设与调试器必须共地。开发板上的 3V3/5V/GND 是电源网络名称，不是某个 GPIO；外设电源接法以当前模块版本为准。

All peripherals and the debugger must share a common ground. The board's 3V3/5V/GND labels identify power nets, not GPIOs. Confirm peripheral power connections against the actual module revision.

## 固件构建 / Firmware Build

安装 ARM GNU Toolchain、CMake、Ninja；Windows 使用 Git Bash。在仓库根目录运行：

Install ARM GNU Toolchain, CMake, and Ninja. On Windows, use Git Bash. Run the following commands from the repository root:

```bash
source fw/tools/env.sh
sbb-env-check
bash fw/build.sh
```

CubeMX 配置入口为 `fw/cubemx/sbb-f103.ioc`。重新生成代码时保留 `USER CODE` 段和独立的 `fw/App/`、`fw/boot/`。构建目录为 `fw/build/`，固件输出位于 `fw/bin/`；这些产物不会提交。

Open `fw/cubemx/sbb-f103.ioc` to configure the project in CubeMX. When regenerating code, preserve the `USER CODE` sections and the separate `fw/App/` and `fw/boot/` directories. The build directory is `fw/build/`, and firmware outputs are placed in `fw/bin/`. These artifacts are excluded from version control.

## 上位机环境 / Host Tools Setup

安装 Python 3（桌面 GUI 另需 tkinter），然后在仓库根目录运行：

Install Python 3, with tkinter available for the desktop GUI. On Windows, run the following commands from the repository root:

```powershell
python -m venv fw/.venv
fw/.venv/Scripts/python.exe -m pip install -r fw/tools/requirements.txt
fw/.venv/Scripts/python.exe fw/tools/uds_web.py
```

浏览器入口：<http://127.0.0.1:8080>。桌面 GUI 可运行 `fw/tools/uds_ui.py`，命令行测试可运行 `fw/tools/uds_tester.py --help`。串口采集、日志转换和镜像生成脚本的参数见各文件头部及 `--help`。

Open <http://127.0.0.1:8080> in your browser. For the desktop GUI, run `fw/tools/uds_ui.py`. For command-line test options, run `fw/tools/uds_tester.py --help`. Refer to each script's header and `--help` for serial capture, log conversion, and image generation options.

Vector CAN 接口需要另行安装对应驱动并配置通道，现有测试仪默认使用 VN1610、CH1、1 Mbps。CANoe 通过 `TestEnv/CANoe/CAN.cfg` 打开，需要本机的 CANoe 安装与授权；首次打开后检查硬件通道映射。

Vector CAN interfaces require the appropriate drivers and channel configuration. The existing tester defaults to VN1610, channel 1, at 1 Mbps. Open `TestEnv/CANoe/CAN.cfg` in a locally installed and licensed copy of CANoe, and check the hardware channel mapping on first use.

部分既有刷写/启动辅助脚本仍包含原开发机的 Python、J-Link 或固件绝对路径，迁移机器后须按实际安装位置调整；上面的 Python 命令可直接指定虚拟环境解释器。

Some existing flashing and startup helper scripts still contain absolute paths to Python, J-Link, or firmware on the original development machine. Adjust these paths when moving to another machine. The Python commands above explicitly select the virtual environment interpreter.

诊断生成器 `fw/tools/odx_build.py` 还依赖本地 `fw/diag/_ref/` 下的 Vector 导出模板，该模板沿用原有排除规则，不在仓库内；使用生成器前需自行准备。项目现有 `SBB_ECU.odx-d` 随诊断配置一起维护。

The diagnostic generator `fw/tools/odx_build.py` also requires a locally available Vector-exported template under `fw/diag/_ref/`. This template is excluded by the existing ignore rules and must be supplied before using the generator. The project's existing `SBB_ECU.odx-d` is maintained alongside the diagnostic configuration.

## Git 维护 / Git Workflow

默认分支为 `main`。提交前用 `git status --short` 和 `git diff --cached --stat` 检查范围，再提交、推送：

The default branch is `main`. Review the changes with `git status --short` and `git diff --cached --stat` before committing and pushing:

```bash
git add fw TestEnv README.md .gitignore .gitattributes
git diff --cached --stat
git commit -m "Describe the change"
git push
```

新增顶层工程目录时，需同步调整根目录 `.gitignore` 的允许列表。编译产物或测量数据如需交付，应单独打包。

When adding a new top-level project directory, update the allowlist in the root `.gitignore`. Package build artifacts or measurement data separately when they need to be delivered.
