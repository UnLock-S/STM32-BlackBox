#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  启动 OpenOCD（STM32F103C8T6）—— 跨平台
#
#  用法：  bash fw/tools/openocd.sh                    # CH347，容量自动探测（默认）
#          bash fw/tools/openocd.sh --128k             # CH347，强制 128 KB（TC-01）
#          bash fw/tools/openocd.sh --jlink            # J-Link，容量自动探测
#          bash fw/tools/openocd.sh --jlink --128k     # J-Link，强制 128 KB
#          bash fw/tools/openocd.sh --srst             # ⚠ 当前构建不支持（见 sbb-probe.cfg）
#          bash fw/tools/openocd.sh --cmd "adapter speed 500"   # 追加任意 OpenOCD 命令
#
#  连上后：  4444 端口 = telnet 命令台   ·   3333 端口 = GDB
#
#  ⚠ 踩坑记录：本机 MSYS 路径转换关闭，`-s /c/Users/...` 这种 POSIX 路径传给原生
#     openocd.exe 会报 "Can't find <cfg>"。所有传给 openocd 的路径统一走 _sbb_native
#     （Windows → C:/...；Linux/macOS → 恒等变换）。
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh" >/dev/null

# ── 解析参数：调试器型号 × flash 容量 × 复位方式 ──
#    向后兼容：只给 --128k 时调试器仍默认 CH347
ADAPTER="ch347"
SIZE="probe"
EXTRA_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --jlink)  ADAPTER="jlink" ;;
    --ch347)  ADAPTER="ch347" ;;
    --128k|128k) SIZE="128k" ;;
    --srst)
      # ⚠ 当前 xPack OpenOCD 0.12.0-7 构建不支持（实测 "Asserting SRST not supported!"）
      #   保留此开关供将来换新版 OpenOCD 时使用
      EXTRA_ARGS+=(-c "reset_config srst_only srst_nogate connect_assert_srst")
      ;;
    --cmd)
      shift
      EXTRA_ARGS+=(-c "$1")
      ;;
  esac
  shift
done

if [ "$ADAPTER" = "jlink" ]; then
  if [ "$SIZE" = "128k" ]; then
    CFG="sbb-jlink-128k.cfg"; DESC="J-Link · 强制 128 KB flash（TC-01）"
  else
    CFG="sbb-jlink.cfg";      DESC="J-Link · flash 容量自动探测（日常）"
  fi
else
  if [ "$SIZE" = "128k" ]; then
    CFG="sbb-128k.cfg";       DESC="CH347 · 强制 128 KB flash（TC-01）"
  else
    CFG="sbb-probe.cfg";      DESC="CH347 · flash 容量自动探测（日常）"
  fi
fi

# ── 前置检查（给出可操作的报错，而不是让 openocd 打哑谜）──
if [ -z "${SBB_OCD_EXE:-}" ] || [ ! -x "$SBB_OCD_EXE" ]; then
  echo "❌ 找不到 openocd。先执行： source fw/tools/env.sh && sbb-env-check"
  echo "   或指定位置： export SBB_OPENOCD_BIN=/path/to/openocd/bin"
  exit 1
fi
if [ -z "${XPACK_SCRIPTS:-}" ] || [ ! -d "$XPACK_SCRIPTS" ]; then
  echo "❌ 定位不到 OpenOCD scripts 目录。请设置 SBB_OPENOCD_SCRIPTS"
  exit 1
fi

CFG_DIR="$(_sbb_native "$SBB_OCD_CFG")"
SCR_DIR="$(_sbb_native "$XPACK_SCRIPTS")"

echo "──────────────────────────────────────────────────────────"
echo " OpenOCD  ·  STM32F103C8T6"
echo " 调试器：$ADAPTER"
echo " 配置：$CFG   —— $DESC"
echo " 脚本搜索路径："
echo "   1) $CFG_DIR   （本项目自写）"
echo "   2) $SCR_DIR   （发行版官方）"
echo " 端口：4444 telnet  /  3333 gdb      Ctrl-C 退出"
echo "──────────────────────────────────────────────────────────"

# ⚠ set -u 下空数组的安全展开写法
exec "$SBB_OCD_EXE" -s "$CFG_DIR" -s "$SCR_DIR" -f "$CFG_DIR/$CFG" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
