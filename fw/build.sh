#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build.sh —— SBB 固件一键构建（跨平台）
#
#  用法：  bash fw/build.sh             # 增量构建
#          bash fw/build.sh --clean     # 清理后重新配置并构建
#          bash fw/build.sh --size      # 只打印尺寸
#
#  前置：source fw/tools/env.sh 所提供的工具链（本脚本会自动 source）
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/tools/env.sh" >/dev/null

# ⚠ cmake / arm-none-eabi-size 是原生程序：Windows 上不认 MSYS 风格路径 /c/...
#   → 统一用 _sbb_native 转换（非 Windows 平台是恒等变换）
HERE_W="$(_sbb_native "$HERE")"
TCF_W="$(_sbb_native "$HERE/cmake/arm-none-eabi.cmake")"
BUILD_W="$(_sbb_native "$HERE/build")"

case "${1:-}" in
  --clean) rm -rf "$HERE/build"; echo "[sbb] 已清理 build/" ;;
  --size)  exec arm-none-eabi-size "$(_sbb_native "$HERE/bin/sbb_app.elf")" ;;
esac

# 前置检查
for t in cmake ninja arm-none-eabi-gcc; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "❌ 缺少 $t。先执行： source fw/tools/env.sh && sbb-env-check"
    exit 1
  }
done

# 首次或未配置 → 跑一次 cmake 配置
if [ ! -f "$HERE/build/build.ninja" ]; then
  echo "[sbb] CMake 配置中…"
  cmake -S "$HERE_W" -B "$BUILD_W" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TCF_W" \
        -DCMAKE_BUILD_TYPE=Debug || exit 1
fi

echo "[sbb] 构建中…"
cmake --build "$BUILD_W" || exit 1

echo
echo "════════ 产物 ════════"
ls -lh "$HERE/bin"/sbb_app.elf "$HERE/bin"/sbb_app.hex "$HERE/bin"/sbb_app.bin 2>/dev/null |
  awk '{ printf "  %-12s %8s\n", $NF, $5 }'

echo
arm-none-eabi-size "$(_sbb_native "$HERE/bin/sbb_app.elf")" 2>/dev/null
