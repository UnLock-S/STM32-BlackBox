# cubemx/ —— CubeMX 工程（配置基准）

## 文件清单

| 文件 | 说明 |
|---|---|
| `sbb-f103.ioc` | **本项目的 CubeMX 配置基准**：引脚分配 + 时钟树 + 外设参数。已验证可加载/回写 |
| `pinout-verified.csv` | 由 CubeMX 从上面这份 `.ioc` 导出的引脚表（48 脚），用于与 SDD 冻结引脚表逐脚比对 |
| `reference/skeleton-6.18.1-saveext.ioc` | CubeMX 自动生成的空白骨架（无我们的配置），留作格式对照 |

---

## 验证状态（实测，非推断）

在 CubeMX **6.18.1-RC2** 上实测：

| 检查项 | 结果 |
|---|---|
| `config load` | **`OK`** |
| `csv pinout` | `OK`，48 脚 / 49 行 |
| `config saveas` | `OK`，且与载入文件**语义零差异**（即已是 CubeMX 规范形式） |
| CAN 波特率（CubeMX 重算） | **`1000000` bps** |
| SPI1 波特率（CubeMX 重算） | **`9.0 MBits/s`** |
| USART1 / I2C1 | `115200` / `400000` |
| 17 个关键引脚的 Signal/Label | 与冻结引脚表**逐脚一致** |

---

## 命令行验证配方（★ 四个坑都在这里）

```bash
cd "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeMX"
jre/bin/java -jar STM32CubeMX.exe -q <脚本文件>
```

脚本内容：

```
config load "<绝对路径>/sbb-f103.ioc"
csv pinout  "<绝对路径>/pinout.csv"
config saveas "<绝对路径>/roundtrip.ioc"
exit
```

### 坑 1：路径**必须加引号**

不加引号时 CubeMX 会**静默不写文件**，却仍返回正常退出码 —— 看不出任何异常。

### 坑 2：是 `config load`，不是 `load`

`load` 只接受 **MCU 名**（如 `load STM32F103C8Tx`）。给它 `.ioc` 路径不会加载任何项目，但随后的 `csv pinout` **仍会写一个只有表头的空 CSV**，极易被误判为成功。

### 坑 3：文件版本必须与所装 CubeMX 版本对齐，否则会挂住

若 `.ioc` 里的 `MxCube.Version` 低于所装版本，`config load` 会弹 **「Migrate / Continue / Cancel」模态框**。静默模式下无人能点 → **进程挂住直到超时，无产物、无报错**。

做自动化验证时：**复制一份**，把 `MxCube.Version` / `MxDb.Version` 改成本机版本，并设 `ProjectManager.AskForMigrate=false`。

> 仓库里这份刻意标注 **6.18.0 / DB.6.0.180** —— 6.18.0 及以上版本都能打开（新版读旧版只弹一次迁移询问，选 `Continue` 即可保持兼容）。**只有做自动化验证时才临时改成 6.18.1**，见 `tools/` 下的做法。

### 坑 4：同一脚本里不要放 `load`

见坑 2 —— 它会卡住，导致后面所有命令都不执行。

---

## 已知导入告警（**非本工程缺陷**，已用对照实验证明）

加载本 `.ioc` 时 CubeMX 6.18.1-RC2 会报 5 条 `OptionalMessage_ERROR`：

```
Pin20 (VP_RIF_VS_RIF1) cannot be retrieved for this MCU
IP (RCC) : Parameter (ADCFreqValue) has invalid value (36000000)
IP (RCC) : Invalid parameter (FamilyName)
IP (RCC) : Parameter (MCOFreq_Value) has invalid value (72000000)
IP (RCC) : Parameter (USBFreq_Value) has invalid value (72000000)
```

**对照实验**：加载一份由 **CubeMX 6.18.0 亲手生成**的现有工程（`eth-t1-converter-fw.ioc`），报出**完全相同的 5 条**。

**结论**：这是该 **RC 版本自身的导入校验缺陷**，与我们的配置无关；且这些告警**不阻止加载**（两份文件都 `OK`、都导出 49 行完整引脚表）。在 6.18.0 稳定版上应不出现。升级到正式版后可复测。

---

## 五个"反直觉"的配置点（都已在 `.ioc` 里体现）

| # | 要点 | 为什么 |
|---|---|---|
| 1 | **PA4 = `GPIO_Output`（标签 `SD_CS`），不是 `SPI1_NSS`** | SD 卡用软件片选；配成硬件 NSS 会与软件 CS 冲突 |
| 2 | **SPI1 必须显式设 `SPI_BAUDRATEPRESCALER_8`** | CubeMX 默认为 `/2` = **36 MBits/s，超 SD 卡上限**；`/8` = 9 MBits/s |
| 3 | **CAN 显式设 `Prescaler=4 / BS1=6TQ / BS2=2TQ`** | 1 Mbps 且采样点 **77.8%**（符合 CiA 301 的 75~87.5%）。STM32 官方例程同速率用 BS1=5/BS2=3，采样点仅 **66.7%** |
| 4 | **PC13 的键名是 `PC13-TAMPER-RTC`** | 写成 `PC13.*` 会被 CubeMX **整个丢弃**，LED 标签随之丢失 |
| 5 | **CAN 引脚的 `Mode` 是 `CAN_Activate`**（不是 `CAN`） | 写错则引脚不被识别 |

另外两条格式规则：

- **含冒号的值要转义**：如 `NVIC.SysTick_IRQn=true\:15\:0\:...`
- **`I2C1.Speed` 是无效参数名**，只需 `I2C1.ClockSpeed=400000`；写了会在导入时报 `Invalid parameter (Speed)`

---

## 代码生成器：能用，但**不是本工程的构建路径**

### 三条实测结论

1. **CLI 支持生成**（实测语法）：
   ```
   config load "<你的.ioc>"
   project name <name>
   project path <dir>
   project generate
   generate code "<dir>/<name>"
   exit
   ```
   bare `generate` 会打印子命令：`generate code <path>`、`generate all_code_in_main`、
   `generate one_file_per_ip`、`generate deviceTree`。

2. **无外设的工程能生成成功**（实测）：产出的裸机工程包含
   `Src/{main.c, stm32f1xx_hal_msp.c, stm32f1xx_it.c, system_stm32f1xx.c}`、
   `Inc/{main.h, stm32f1xx_hal_conf.h, stm32f1xx_it.h}`、`startup_stm32f103xb.s`、
   `STM32F103xx_FLASH.ld`、`Makefile`、`Drivers/`。其 `main.c` 是
   `HAL_Init()` + `SystemClock_Config()` + `while (1)` —— **纯裸机，无 RTOS**。

3. **本 `.ioc` 目前无法作为生成输入**：`project generate` 报
   ```
   [INFO] ProjectBuilder:2453 - IP not ready for code generation: SPI1
   ```
   修好 SPI1 的参数后，又变成 `IP not ready for code generation: I2C1` —— 逐个 IP 扫。
   然后弹「是否继续」对话框，**静默模式无人可点 → 挂死**。

### 为什么「未就绪」

这是一份**引脚/时钟/关键参数基线**，只声明了我们设计真正关心的参数
（如 I2C1 只有 `ClockSpeed`，没有 DutyCycle/OwnAddress/Filter 等）。
**CubeMX 生成代码要求每个 IP 的参数全套齐全**，所以它判定「未就绪」。

→ **本 `.ioc` 的定位就是基线 + 可评审文档 + 交叉核对来源，不是代码生成输入。**
   要让它能生成，得让 CubeMX 把每个 IP 的默认参数补齐（在 GUI 里过一遍再保存），
   而那样文件会被塞进大量与设计无关的默认值 —— 与「可评审的基线」目标冲突。

### 生成器仍有的三种真用法

| 用法 | 说明 |
|---|---|
| **交叉核对寄存器值**（推荐） | 生成到临时目录，读 `MX_*_Init()` / `SystemClock_Config()`，与 `src/bsp/` 里的寄存器值逐项比 → 第三条独立验证路径 |
| **HAL 原型起点** | 有人要快速做 HAL 原型时，拿这份 `.ioc` 生成 Keil/CubeIDE 工程即可跑 |
| **想要「生成带 RTOS 的工程」** | 需**另开一份** `.ioc`（开 HAL + FREERTOS 中间件），主线这份保持纯净 |

### 为什么主线 `.ioc` 里**不**引入 FREERTOS（刻意的）

| | 本工程 | CubeMX 生成物 |
|---|---|---|
| 驱动层 | 寄存器级（直写 CMSIS） | HAL 库 |
| RTOS 内核 | **vendored V11.1.0** | **V10.3.1**（实测模板 `freertos_app_c_v10_3_1.ftl`）|
| RTOS 封装 | 自研 `sched` 层 | CMSIS-RTOS v1 / v2 / native 三选一 |
| 分配方式 | **全静态**，`configSUPPORT_DYNAMIC_ALLOCATION=0`（ADR-10） | 面向堆分配 |
| 代码归属 | `src/` 全部手写 | **全量覆盖** `Core/Src`、`Core/Inc`、`Middlewares/` |

CubeMX 的生成是**覆盖式**的：一生成就会重写 `main.c`、带回 HAL + 堆分配，
冲掉 `sched` 层与寄存器级 `bsp_*`，**使 ADR-10 当场失效**，内核还会从 V11.1.0 降到 V10.3.1。

---

## ⚠ 操作陷阱：**运行中的 CubeMX GUI 会静默覆盖本文件**

实测事故：本文件被提交后又被覆盖成 `/2`（36 MBits/s，超 SD 卡上限）、
SPI1 参数块退回默认 —— 因为 **CubeMX GUI 里打开着这个项目，它保存时用内存中的状态覆盖了磁盘**。
（磁盘 mtime 晚于提交时间；`git status` 显示该文件被改；无 CubeMX 进程运行 = 已退出。）

**规则：**

- **改了本文件后，不要在另一个 CubeMX GUI 会话里打开同一个文件**，除非你接受以 GUI 为唯一写入方
- 若 GUI 已打开且你**要在外部编辑**：先在 GUI 里关掉项目（不保存）
- 发现文件被改了回去：`git restore cubemx/sbb-f103.ioc`（**权威版本在 git 里**）
- 同理：**写入被 Hermes 的「文件已被改动」守卫拦下时，先查是谁改的，不要绕开守卫** —— 守卫是对的

---

## 与仓库其余部分的边界

- **`.ioc` 是配置基线的唯一权威**：改了它必须同步 `src/bsp/` 里的寄存器配置
- **它不接管构建系统**：编译仍走顶层 `CMakeLists.txt` + `cmake/arm-none-eabi.cmake` + vendored FreeRTOS
- **不生成代码进仓库**：CubeMX 的生成产物只作参考（可导到临时目录比对），**绝不覆盖 `src/`**。
  原因：本工程是**寄存器级 + 全静态分配**（ADR-10），而 CubeMX 生成的是 **HAL + 动态分配**工程
- **需要 CubeMX ≥ 6.18.0**；CubeMX2 不支持 F1 系列

---

## 固件包

打开/编辑 `.ioc` **不需要**固件包；只有**生成代码**才需要：

```
swmgr install stm32cube_f1_1.8.7 ask
```

（`STM32Cube FW_F1 V1.8.7`，已写在 `ProjectManager.FirmwarePackage`）

> ⚠ **注意：`swmgr` 命令返回 `KO`，但包实际上装成功了。**
> 实测 `swmgr refresh` / `swmgr install stm32cube_f1_1.8.7 ask` 都打印 `KO`，
> 而 `~/STM32Cube/Repository/STM32Cube_FW_F1_V1.8.7`（**320 MB**）确实落地并可被 CubeMX 识别。
>
> **结论：这个 RC 版的 `KO` 不能当作失败判据**（与 `config saveext` 那次「看起来没写、其实写了」同一类毛病）。
> **判据要换成检查产物本身**：`ls ~/STM32Cube/Repository/` 里有没有 `STM32Cube_FW_F1_V1.8.7`。
> 若确实没有，再走 GUI 的 `INSTALL / REMOVE` 勾选 F1 V1.8.7。
