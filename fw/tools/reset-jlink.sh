#  ★ 复位方式：RESETPIN（SetResetType = 2）
#
#    实机结论（2026-10-19 修正）:
#      · RESETPIN 稳定有效 ✓ —— 前提是 J-Link 的 nRESET 线接好（之前判"无效"是线束松动导致的误判 ✗）
#      · RESETPIN 拉的是 MCU 的 NRST -> MCU 重启 -> 固件重跑 OLED 初始化（四个自检画面）✓
#      · 它【不给 OLED 模块断电】-> 面板自身状态机锁死时，仍需给模块 VCC 断电 ✗
#      · 判据永远用可观测状态：读 xTickCount 是否归零 / 看自检画面是否重跑 ✓
#
#    ⚠ 注意：`loadbin/loadfile` 会触发一次隐式 reset&halt，`r` 才是我们要的复位，
#      两者叠加没有问题；单独 `r` 也有效（线束正常时）。

set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
JLINK="C:/Program Files/SEGGER/JLink_V812g/JLink.exe"
BIN="C:/Users/zh244/Documents/UserProject/leasure/fw/bin/sbb_app.bin"
DEVICE="${SBB_DEVICE:-STM32F103C8}"
SPEED="${SBB_SPEED:-4000}"

[ -f "$BIN" ] || { echo "[sbb] ✗ 找不到 bin/sbb_app.bin"; exit 1; }

{
  echo "si SWD"
  echo "speed $SPEED"
  echo "device $DEVICE"
  echo "exec SetResetType = 2"
  echo "connect"
  echo "loadbin $BIN,0x08000000"
  echo "r"
  echo "g"
  echo "qc"
} > "$HERE/.reset.tmp.jlink"

echo "[sbb] 复位（烧写式，真正拉低 RST 脚）"
"$JLINK" -CommanderScript "$HERE/.reset.tmp.jlink" 2>&1 | tr -d '\r' | grep -aiE "Reset type|Reset device|O\.K\."
rm -f "$HERE/.reset.tmp.jlink"
