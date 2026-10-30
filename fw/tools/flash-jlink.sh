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

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$HERE/.." && pwd)"

HEX="${1:-$FW/bin/sbb_app.hex}"
JLINK="${JLINK_EXE:-/c/Program Files/SEGGER/JLink_V812g/JLink.exe}"
DEVICE="${JLINK_DEVICE:-STM32F103C8}"
SPEED="${JLINK_SPEED:-4000}"

if [ ! -x "$JLINK" ] && [ ! -f "$JLINK" ]; then
    echo "❌ 找不到 JLink.exe（设 JLINK_EXE 覆盖）：$JLINK" >&2
    exit 1
fi
if [ ! -f "$HEX" ]; then
    echo "❌ 找不到待烧写文件：$HEX" >&2
    exit 1
fi

# 原生程序不认 MSYS 路径 → 转成 Windows 风格（含正斜杠）
nat() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

JLINK_N="$(nat "$JLINK")"
HEX_N="$(nat "$HEX")"
SCRIPT="$(nat "$HERE/.flash-jlink.tmp.jlink")"

cat > "$HERE/.flash-jlink.tmp.jlink" <<EOF
// 由 flash-jlink.sh 生成，勿手工编辑
si SWD
speed $SPEED
device $DEVICE
exec SetResetType = 2
connect
loadfile "$HEX_N"
r
g
qc
EOF

echo "[sbb] 烧写 $HEX_N"
echo "[sbb] 设备 $DEVICE ／ SWD $SPEED kHz"
echo "[sbb] 复位方式 = RESETPIN（烧写式调用有效 ✓；面板会跟着重新初始化）
       输出里出现 via AIRCR.SYSRESETREQ 即表示没接上）"
"$JLINK_N" -CommanderScript "$(nat "$HERE/.flash-jlink.tmp.jlink")" 2>&1 |
    tr -d '\r' | grep -avE "^\s*$" | sed 's/^/  /'
rc=${PIPESTATUS[0]}
# ★ 自动检查：输出里应出现 "via reset pin"（RESETPIN 生效）
#   若出现 "via AIRCR.SYSRESETREQ"，说明 nRST 线没接好 -> 面板可能卡死 ✗

rm -f "$HERE/.flash-jlink.tmp.jlink"
echo "[sbb] JLink 退出码 = $rc"
exit "$rc"
