# 第三方许可与来源 / Third-Party Licenses and Provenance

核对日期 / Review date: **2026-10-30**

本清单依据 Git 跟踪文件、随附许可证、源码文件头以及固定版本的 Python 发布包。它是来源与许可记录，不代表所有历史代码来源及所有二进制分发方式均已核实。

This inventory is based on Git-tracked files, bundled licenses, source headers, and the pinned Python release archives. It records provenance and licensing; it does not establish every historical source or cover every binary distribution method.

## 项目自有代码 / Project-Owned Code

根目录 [LICENSE](LICENSE) 采用 **MIT**，仅适用于项目权利人拥有并有权授权的原创代码和文档，包括 `fw/App/`、`fw/boot/`、`fw/tools/` 中的原创部分。它不覆盖第三方代码、保留供应商文件头的链接脚本、CubeMX/ST 生成或派生代码、来源待核实的素材及商业工具资产。文件自带的第三方许可和版权声明优先适用于相应内容。

The root [LICENSE](LICENSE) applies the **MIT License** only to original code and documentation owned and licensable by the project copyright holder, including original portions of `fw/App/`, `fw/boot/`, and `fw/tools/`. It does not relicense third-party code, linker scripts carrying vendor headers, CubeMX/ST-generated or derived code, assets with unresolved provenance, or commercial tool assets. Existing third-party licenses and copyright notices continue to govern their respective content.

## 随仓库分发的第三方内容 / Bundled Third-Party Content

| 组件 / Component | 路径与证据 / Paths and Evidence | 许可 / License |
| --- | --- | --- |
| FreeRTOS kernel **V10.3.1** | `fw/cubemx/Middlewares/Third_Party/FreeRTOS/Source/`，版本取自 `include/task.h` / Version from `include/task.h` | **MIT**；原始 `LICENSE` 与文件头 / Original `LICENSE` and file headers |
| CMSIS Core headers | `fw/cubemx/Drivers/CMSIS/Include/`；Arm/IAR 文件头及 `Drivers/CMSIS/LICENSE.txt` / Arm/IAR headers and bundled license | **Apache-2.0** |
| STM32F1 CMSIS device files | `fw/cubemx/Drivers/CMSIS/Device/ST/STM32F1xx/` | **Apache-2.0**；V1.8.7 包级许可表与本地组件通知一致 / V1.8.7 package license table agrees with the local component notice |
| STM32F1 HAL/LL driver **1.1.10** | `fw/cubemx/Drivers/STM32F1xx_HAL_Driver/`；版本取自 `Src/stm32f1xx_hal.c` / Version from `Src/stm32f1xx_hal.c` | **BSD-3-Clause**；V1.8.7 包级许可表与本地组件通知一致 / V1.8.7 package license table agrees with the local component notice |
| CMSIS-RTOS V1 FreeRTOS adapter | `fw/cubemx/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS/cmsis_os.c`、`cmsis_os.h` | 文件头为 **BSD-3-Clause**，版权归 Arm / ST；不能归入内核 MIT / BSD-3-Clause headers, Arm / ST copyrights; distinct from the MIT kernel |
| FreeRTOS configuration | `fw/cubemx/Core/Inc/FreeRTOSConfig.h` | 文件头为 **MIT**，保留 Amazon / ST 版权 / MIT header with Amazon / ST copyrights |
| ST linker script and derivatives | `fw/cubemx/STM32F103xx_FLASH.ld`、`fw/App/sbb_app.ld`、`fw/boot/sbb_boot.ld` | 文件头包含完整 **BSD-3-Clause** 条款与 ST 2025 版权 / Full BSD-3-Clause terms and ST 2025 copyright in headers |

许可全文集中保存在 [licenses/](licenses/README.md)，原组件目录和文件头中的声明保持原样。CMSIS 目录内 ST Device 子目录使用其自己的许可通知。`.ioc` 指定 STM32Cube FW_F1 V1.8.7；已从本机对应安装包补入 [Package_license.md](licenses/STM32CubeF1-V1.8.7-Package_license.md)，其表格确认了 HAL 和 Device 的上述许可。包级文档还列有本仓库未使用的组件，不能据此认定它们也被包含。

Full license texts are collected in [licenses/](licenses/README.md), while notices in the original component directories and file headers remain intact. The ST Device subtree has its own license notice. The `.ioc` specifies STM32Cube FW_F1 V1.8.7. Its [Package_license.md](licenses/STM32CubeF1-V1.8.7-Package_license.md) was recovered from the corresponding local installation and confirms the HAL and Device licenses above. That package-wide document also lists components absent from this repository; it does not establish their inclusion here.

上游参考 / Upstream references: [STM32CubeF1 component license table](https://github.com/STMicroelectronics/STM32CubeF1/blob/master/LICENSE.md), [HAL 1.1.10 license](https://github.com/STMicroelectronics/stm32f1xx-hal-driver/blob/v1.1.10/LICENSE.md), [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0).

## 上位机外部依赖 / External Host Dependencies

这些包由 `fw/tools/requirements.txt` 安装，当前 Git 仓库不包含它们的源码、wheel 或打包后的运行环境。以下版本许可已从对应 PyPI 源码发布包核对。

These packages are installed from `fw/tools/requirements.txt`. Their sources, wheels, and bundled runtimes are not included in this Git repository. The license information below was checked against the corresponding PyPI source distributions.

| 依赖 / Dependency | 用途 / Use | 许可与来源 / License and Source |
| --- | --- | --- |
| `python-can==4.6.1` | UDS/CAN 上位机通过 `import can` 使用 / Imported by UDS/CAN host tools | **LGPL-3.0-only**；[versioned release](https://pypi.org/project/python-can/4.6.1/)、[license copy](licenses/LGPL-3.0-python-can.txt) |
| `pyserial==3.5` | 串口采集与启动日志 / Serial capture and boot logs | **BSD-3-Clause**；[versioned release](https://pypi.org/project/pyserial/3.5/)、[license copy](licenses/BSD-3-Clause-pyserial.txt) |

项目的 MIT 许可不会替换 `python-can` 的 LGPL。当前源码加独立安装依赖的形式可以保留各自许可；若将上位机打包为 EXE、安装包或预置环境分发，需要一并处理库的版权/许可文本、对应源码提供方式及用户替换/修改库的能力。LGPLv3 还引用 GPLv3 条款，因此许可目录同时保存 GPLv3 全文。这不意味着本工程的固件被改为 GPL。

The project's MIT License does not replace python-can's LGPL. Source code with separately installed dependencies retains the respective licenses. Distributing a host executable, installer, or prebuilt environment requires handling library attribution and license texts, corresponding-source availability, and users' ability to replace or modify the library. LGPLv3 incorporates GPLv3 terms, so the license directory includes the GPLv3 text as well. This does not relicense this project's firmware under GPL.

本次未对 Python 的所有传递依赖、可选 CAN 后端、Python/tkinter 运行时或外部工具链生成完整 SBOM；实际打包时须按最终环境补齐。[LGPLv3 官方条款](https://www.gnu.org/licenses/lgpl-3.0.html)是分发要求的依据。

This review is not a complete SBOM of Python transitive dependencies, optional CAN backends, Python/tkinter runtimes, or external toolchains. Inventory the actual shipped environment before packaging. Refer to the [official LGPLv3 terms](https://www.gnu.org/licenses/lgpl-3.0.html) for distribution requirements.

## 待核实来源与商业工具资产 / Unresolved Provenance and Commercial Tool Assets

| 范围 / Scope | 检查结果与后续处理 / Finding and Follow-up |
| --- | --- |
| `fw/cubemx/Core/`（除带 MIT 文件头的 `FreeRTOSConfig.h` 外 / except the MIT-marked `FreeRTOSConfig.h`）、`fw/cubemx/startup_stm32f103xb.s` | 多个 ST 文件头指向组件根目录 `LICENSE`，当前生成工程根目录没有对应文件。保留文件头；需核对所用 CubeMX/STM32CubeF1 包的原始条款后补齐适用许可，不能用项目 MIT 替代。/ Several ST headers refer to a component-root `LICENSE` absent from the generated project root. Retain headers and recover applicable terms from the actual CubeMX/STM32CubeF1 package; do not substitute the project MIT License. |
| `fw/App/drv/ssd1306.c` 中 `s_font` 字模 / glyph data | 未发现来源或第三方授权说明，不能仅凭常见 5×7 字模认定属于某个库，也不能确认其独立来源。若来自外部，补充原始出处和许可。/ No provenance or third-party notice was found. A common 5×7 pattern alone neither identifies a library nor proves independent origin. Add the original source and license if externally derived. |
| `fw/diag/SBB_ECU.odx-d` | 既有生成脚本说明使用 Vector 导出模板；模板未随仓库提供。项目诊断数据与模板内容应分别核对，模板衍生部分的再分发权限尚未证实。/ The existing generator describes using a Vector-exported template that is absent from the repository. Distinguish project diagnostic data from template content; redistribution rights for template-derived portions remain unverified. |
| `TestEnv/CANoe/CAN.cfg` | 商业工具保存的工程配置，包含 Actipro 版权字符串；这些字符串本身不能证明嵌入了可单独分发的软件库或已获开源许可。保留配置原字节，按 Vector/相关供应商条款核对再分发范围。/ A project configuration saved by a commercial tool, containing Actipro copyright strings. These strings alone establish neither a separately distributable embedded library nor an open-source grant. Preserve the file bytes and verify redistribution scope under the applicable vendor terms. |

本项目不随仓库分发 CANoe、CANdelaStudio、J-Link、STM32CubeMX、Vector 驱动、Python 或 ARM GNU 工具链的安装包；它们不因本项目采用 MIT 而改变许可。

The repository does not distribute installers for CANoe, CANdelaStudio, J-Link, STM32CubeMX, Vector drivers, Python, or the ARM GNU toolchain. Their licenses are unaffected by this project's MIT License.

## 维护规则 / Maintenance

- 保留第三方版权、许可全文和免责条款；新增组件时更新本清单及许可目录。/ Preserve third-party copyrights, full license texts, and disclaimers; update this inventory when adding components.
- 修改 Apache-2.0 文件时，在文件中显著说明修改；如原发布包含适用的 `NOTICE`，分发时保留相应内容。当前跟踪文件中未发现上游 `NOTICE`。/ Mark modifications to Apache-2.0 files prominently and retain applicable upstream `NOTICE` content if supplied. No upstream `NOTICE` was found among the tracked files reviewed.
- 分发固件或上位机二进制时，附带适用的第三方声明和许可，并按实际构成复核要求。/ Include applicable notices and licenses with firmware or host binaries and review the actual shipped composition.
