#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  flash-all.sh —— 一次性烧 Bootloader + App 两个镜像
# ─────────────────────────────────────────────────────────────────────────────
#  分区:
#    0x08000000  Bootloader  (14 KB)   bin/sbb_boot.bin
#    0x08003800  App         (50 KB)   bin/sbb_app.bin
#    0x0800FC00  标志页      ( 1 KB)   ← 整片擦除会一并清掉（= 无升级请求）
#
#  ★ 用 RESETPIN（烧写式调用有效：前面有 loadbin，J-Link 才会真的拉 RST）
#  ★ 整片擦除后标志页为 0xFF，bootloader 判定"无升级请求"→ 直接跳 App ✓
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
JLINK="C:/Program Files/SEGGER/JLink_V812g/JLink.exe"
DEVICE="${SBB_DEVICE:-STM32F103C8}"
SPEED="${SBB_SPEED:-4000}"

BOOT_BIN="C:/Users/zh244/Documents/UserProject/leasure/fw/bin/sbb_boot.bin"
APP_BIN="C:/Users/zh244/Documents/UserProject/leasure/fw/bin/sbb_app.bin"

for f in "$BOOT_BIN" "$APP_BIN"; do
    [ -f "$f" ] || { echo "[sbb] ✗ 找不到 $f（先跑 bash build.sh）"; exit 1; }
done

BSZ=$(stat -c %s "$BOOT_BIN")
ASZ=$(stat -c %s "$APP_BIN")
echo "[sbb] boot = $BSZ B (上限 14336)"
echo "[sbb] app  = $ASZ B (上限 51200)"
[ "$BSZ" -le 14336 ] || { echo "[sbb] ✗ bootloader 超过 12 KB"; exit 1; }
[ "$ASZ" -le 51200 ] || { echo "[sbb] ✗ App 超过 51 KB"; exit 1; }

JLF="C:/Users/zh244/AppData/Local/hermes/cache/scratch/sbbflash.jlink"
{
    echo "si SWD"
    echo "speed $SPEED"
    echo "device $DEVICE"
    echo "exec SetResetType = 2"
    echo "connect"
    echo "erase"
    echo "loadbin $BOOT_BIN,0x08000000"
    echo "loadbin $APP_BIN,0x08003800"
    echo "verifybin $BOOT_BIN,0x08000000"
    echo "verifybin $APP_BIN,0x08003800"
    echo "r"
    echo "g"
    echo "qc"
} > "$JLF"

echo "[sbb] 烧写中（整片擦除 + boot + app + 校验）..."
OUT=$("$JLINK" -CommanderScript "$JLF" 2>&1)
RC=$?
rm -f "$JLF"

# ★ 失败时必须把 J-Link 的原话打出来，否则只剩"退出码 1"这种无用信息 ✗
if [ $RC -ne 0 ] || ! printf '%s' "$OUT" | grep -qi "Verify successful"; then
    echo "[sbb] ✗ 失败（J-Link 退出码 $RC），完整输出："
    printf '%s\n' "$OUT" | tr -d '\r' | sed 's/^/    /'
    exit 1
fi

printf '%s\n' "$OUT" | tr -d '\r' | grep -aiE "Reset device|Verify successful" | sed 's/^/  /'
echo "[sbb] ✓ 两个镜像均已写入并校验通过"
exit 0
