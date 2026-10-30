# bin/ —— 编译输出（交付物）

`bash fw/build.sh` 之后，这个目录里会出现全部可直接使用的产物：

| 文件 | 用途 |
|---|---|
| `sbb_app.elf` | **调试用**：VS Code Cortex-Debug / `arm-none-eabi-gdb` 加载它（带符号与调试信息） |
| `sbb_app.hex` | **烧写用**：Intel HEX，OpenOCD / STM32CubeProgrammer 都可直接吃 |
| `sbb_app.bin` | **烧写用**：裸二进制，从 `0x08000000` 起；也适合 UART bootloader 刷写 |
| `sbb_app.map` | 链接映射表：查某个符号落在哪个段、`--gc-sections` 到底丢了什么 |

中间产物（CMake 缓存、`.o`、ninja 日志）在 `../build/`，随便删。

> ⚠ 本目录下的 `*.elf/*.hex/*.bin/*.map` 已被 `.gitignore` 忽略（它们是可重建的产物），
> 只有本 README 入库。要归档某次构建的产物，请连同 commit id 一起放到别处（如 `archive/`）。
