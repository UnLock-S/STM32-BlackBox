# STM32-BlackBox

基于 STM32F103C8T6 的单板黑匣子工程，包含 FreeRTOS 固件、CubeMX 配置、Bootloader、Python 上位机工具及 CANoe 测试配置。应用涉及 CAN、模拟量采集、OLED、SD 卡记录和固件升级；功能完成度以实际源码和目标板验证结果为准。

## 工程布局

| 路径 | 内容 |
| --- | --- |
| `fw/App/` | 应用、板级驱动、调度与链接脚本 |
| `fw/boot/` | Bootloader 与升级处理 |
| `fw/cubemx/` | CubeMX `.ioc`、生成代码、STM32 HAL/CMSIS 和 FreeRTOS 源码 |
| `fw/cmake/`、`fw/CMakeLists.txt` | ARM GNU / CMake / Ninja 构建配置 |
| `fw/tools/` | Python 上位机、UDS 测试、镜像打包、串口和调试脚本 |
| `fw/diag/` | 诊断 JSON 配置及项目 ODX 描述 |
| `fw/jlink/` | J-Flash 工程 |
| `fw/setup/TOOLCHAIN-SETUP.md` | 工具链安装记录 |
| `fw/docs/WIRING.md` | 板级接线说明 |
| `TestEnv/CANoe/` | CANoe 测试配置 |

本仓库只维护可开发的源码、配置和工具环境。硬件资料包、需求文档、工作记录、编译结果、运行日志、上传镜像、CANoe 会话状态和本机编辑器配置留在本地，不纳入版本管理。第三方源码保留各自目录中的许可证；仓库不包含编译器、Vector 驱动或商业软件安装包。

## 固件构建

安装 ARM GNU Toolchain、CMake、Ninja；Windows 使用 Git Bash。在仓库根目录运行：

```bash
source fw/tools/env.sh
sbb-env-check
bash fw/build.sh
```

CubeMX 配置入口为 `fw/cubemx/sbb-f103.ioc`。重新生成代码时保留 `USER CODE` 段和独立的 `fw/App/`、`fw/boot/`。构建目录为 `fw/build/`，固件输出位于 `fw/bin/`；这些产物不会提交。

## 上位机环境

安装 Python 3（桌面 GUI 另需 tkinter），然后在仓库根目录运行：

```powershell
python -m venv fw/.venv
fw/.venv/Scripts/python.exe -m pip install -r fw/tools/requirements.txt
fw/.venv/Scripts/python.exe fw/tools/uds_web.py
```

浏览器入口：<http://127.0.0.1:8080>。桌面 GUI 可运行 `fw/tools/uds_ui.py`，命令行测试可运行 `fw/tools/uds_tester.py --help`。串口采集、日志转换和镜像生成脚本的参数见各文件头部及 `--help`。

Vector CAN 接口需要另行安装对应驱动并配置通道，现有测试仪默认使用 VN1610、CH1、1 Mbps。CANoe 通过 `TestEnv/CANoe/CAN.cfg` 打开，需要本机的 CANoe 安装与授权；首次打开后检查硬件通道映射。

部分既有刷写/启动辅助脚本仍包含原开发机的 Python、J-Link 或固件绝对路径，迁移机器后须按实际安装位置调整；上面的 Python 命令可直接指定虚拟环境解释器。此次入库不修改固件逻辑和刷写行为。

诊断生成器 `fw/tools/odx_build.py` 还依赖本地 `fw/diag/_ref/` 下的 Vector 导出模板，该模板沿用原有排除规则，不在仓库内；使用生成器前需自行准备。项目现有 `SBB_ECU.odx-d` 随诊断配置一起维护。

## Git 维护

默认分支为 `main`。提交前用 `git status --short` 和 `git diff --cached --stat` 检查范围，再提交、推送：

```bash
git add fw TestEnv README.md .gitignore .gitattributes
git diff --cached --stat
git commit -m "Describe the change"
git push
```

新增顶层工程目录时，需同步调整根目录 `.gitignore` 的允许列表。编译产物或测量数据如需交付，应单独打包。
