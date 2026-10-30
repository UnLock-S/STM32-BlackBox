#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  SBB 项目工具链环境（跨平台：Windows/git-bash · Linux · macOS）
# ─────────────────────────────────────────────────────────────────────────────
#  ★ 设计原则：本文件**不含任何写死的机器路径**，也不依赖任何外部下载。
#     工具链位置按下面的优先级自动解析，每一项都可以用环境变量覆盖：
#
#       ARM GCC     : $SBB_ARM_GCC_BIN      → 已在 PATH → 常见安装位置
#       OpenOCD     : $SBB_OPENOCD_BIN      → 已在 PATH → 常见安装位置
#       OCD scripts : $SBB_OPENOCD_SCRIPTS  → 从 OpenOCD 目录推断
#
#  用法：  source fw/tools/env.sh
#          sbb-env-check          # 一键自检
# ─────────────────────────────────────────────────────────────────────────────

export SBB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── 原生 Windows 程序需要 Windows 风格路径；Linux/macOS 上是恒等变换 ──
#   本机 MSYS 路径转换关闭：`-s /c/Users/...` 传给 openocd.exe / cmake.exe 会失效。
#   cygpath -m 给出 `C:/Users/...`（正斜杠，原生程序与 bash 都吃）。
if command -v cygpath >/dev/null 2>&1; then
  _sbb_native() { cygpath -m "$1"; }
else
  _sbb_native() { printf '%s' "$1"; }
fi

_sbb_has() { command -v "$1" >/dev/null 2>&1; }

# 在若干候选目录里找第一个含指定可执行文件的目录；找不到返回非 0
_sbb_first_dir_with() {
  local exe="$1"; shift
  local d
  for d in "$@"; do
    [ -n "$d" ] && [ -d "$d" ] || continue
    if [ -f "$d/$exe" ] || [ -f "$d/$exe.exe" ]; then printf '%s' "$d"; return 0; fi
  done
  return 1
}

# ═════════════════ ARM 交叉编译工具链 ═════════════════
if [ -n "${SBB_ARM_GCC_BIN:-}" ]; then
  ARM_GCC_BIN="$SBB_ARM_GCC_BIN"
elif _sbb_has arm-none-eabi-gcc; then
  ARM_GCC_BIN="$(dirname "$(command -v arm-none-eabi-gcc)")"
elif _sbb_has arm-none-eabi-gcc.exe; then
  ARM_GCC_BIN="$(dirname "$(command -v arm-none-eabi-gcc.exe)")"
else
  ARM_GCC_BIN="$(_sbb_first_dir_with arm-none-eabi-gcc \
      /c/Program\ Files\ \(x86\)/Arm\ GNU\ Toolchain\ arm-none-eabi/*/bin \
      /c/Program\ Files/Arm\ GNU\ Toolchain\ arm-none-eabi/*/bin \
      "$LOCALAPPDATA"/Microsoft/WinGet/Packages/xpack-dev-tools.arm-none-eabi-gcc-*/xpack-arm-none-eabi-gcc-*/bin \
      /opt/xpack-arm-none-eabi-gcc-*/bin \
      /opt/gcc-arm-none-eabi/bin \
      /usr/bin 2>/dev/null)"
fi

# ═════════════════ OpenOCD ═════════════════
if [ -n "${SBB_OPENOCD_BIN:-}" ]; then
  XPACK_OPENOCD_BIN="$SBB_OPENOCD_BIN"
elif _sbb_has openocd; then
  XPACK_OPENOCD_BIN="$(dirname "$(command -v openocd)")"
elif _sbb_has openocd.exe; then
  XPACK_OPENOCD_BIN="$(dirname "$(command -v openocd.exe)")"
else
  XPACK_OPENOCD_BIN="$(_sbb_first_dir_with openocd \
      "$LOCALAPPDATA"/Microsoft/WinGet/Packages/xpack-dev-tools.openocd-xpack_*/xpack-openocd-*/bin \
      "$HOME"/AppData/Local/Microsoft/WinGet/Packages/xpack-dev-tools.openocd-xpack_*/xpack-openocd-*/bin \
      /opt/xpack-openocd-*/bin \
      /opt/openocd/bin 2>/dev/null)"
fi

# 可执行文件名（Windows 上是 openocd.exe）
if [ -n "$XPACK_OPENOCD_BIN" ] && [ -f "$XPACK_OPENOCD_BIN/openocd.exe" ]; then
  SBB_OCD_EXE="$XPACK_OPENOCD_BIN/openocd.exe"
elif [ -n "$XPACK_OPENOCD_BIN" ]; then
  SBB_OCD_EXE="$XPACK_OPENOCD_BIN/openocd"
fi

# ═════════════════ OpenOCD scripts 目录 ═════════════════
#   优先环境变量；否则按发行版布局推断（xPack: ../openocd/scripts；发行版: ../share/openocd/scripts）
if [ -n "${SBB_OPENOCD_SCRIPTS:-}" ]; then
  XPACK_SCRIPTS="$SBB_OPENOCD_SCRIPTS"
elif [ -n "$XPACK_OPENOCD_BIN" ]; then
  for cand in "$XPACK_OPENOCD_BIN/../openocd/scripts" \
              "$XPACK_OPENOCD_BIN/../share/openocd/scripts"; do
    if [ -d "$cand" ]; then XPACK_SCRIPTS="$(cd "$cand" && pwd)"; break; fi
  done
fi

# ═════════════════ 本项目自写的 OpenOCD 配置目录 ═════════════════
export SBB_OCD_CFG="$SBB_ROOT/tools/openocd"

export ARM_GCC_BIN XPACK_OPENOCD_BIN XPACK_SCRIPTS SBB_OCD_CFG SBB_OCD_EXE

# ── 工具链优先入 PATH（找不到则不加，由 sbb-env-check 报错）──
[ -n "$ARM_GCC_BIN" ]       && export PATH="$ARM_GCC_BIN:$PATH"
[ -n "$XPACK_OPENOCD_BIN" ] && export PATH="$XPACK_OPENOCD_BIN:$PATH"

# ── 便捷函数（比 alias 更好用：可追加参数、可组合）──
sbb-ocd() {
  "$SBB_OCD_EXE" -s "$(_sbb_native "$SBB_OCD_CFG")" -s "$(_sbb_native "$XPACK_SCRIPTS")" "$@"
}

# ═════════════════ 自检 ═════════════════
sbb-env-check() {
  local ok=0 bad=0
  _chk() {
    local name="$1" cmd="$2"
    if command -v "$cmd" >/dev/null 2>&1; then
      local v; v="$("$cmd" --version 2>&1 | head -1 | tr -d '\r')"
      printf "  ✅ %-22s %s\n" "$name" "${v:0:70}"; ok=$((ok+1))
    else
      printf "  ❌ %-22s 缺失\n" "$name"; bad=$((bad+1))
    fi
  }
  echo "── SBB 工具链自检 ──────────────────────────────────"
  _chk "arm-none-eabi-gcc" arm-none-eabi-gcc
  _chk "arm-none-eabi-gdb" arm-none-eabi-gdb
  _chk "openocd"           openocd
  _chk "cmake"             cmake
  _chk "ninja"             ninja
  echo "  ────────────────────────────────"
  printf "  通过 %d 项，缺 %d 项\n" "$ok" "$bad"

  # OpenOCD 是否带 ch347 驱动（本项目依赖）
  if command -v openocd >/dev/null 2>&1; then
    if openocd -c "adapter list" 2>&1 | grep -q "ch347"; then
      printf "  ✅ %-22s ch347 { jtag swd }\n" "OpenOCD ch347 驱动"
    else
      printf "  ❌ %-22s 驱动缺失（需更新的 OpenOCD）\n" "OpenOCD ch347 驱动"; bad=$((bad+1))
    fi
  fi
  printf "  %-24s %s\n" "OpenOCD scripts" "${XPACK_SCRIPTS:-❌ 未定位}"
  printf "  %-24s %s\n" "本项目 OCD 配置" \
    "$([ -f "$SBB_OCD_CFG/interface/ch347.cfg" ] && echo "$SBB_OCD_CFG" || echo '❌ 未找到')"
  echo "────────────────────────────────────────────────────"
  if [ "$bad" -gt 0 ]; then
    echo "  提示：工具链不在默认位置时，用环境变量指定后重新 source："
    echo "    export SBB_ARM_GCC_BIN=/path/to/arm-gnu-toolchain/bin"
    echo "    export SBB_OPENOCD_BIN=/path/to/openocd/bin"
    echo "    export SBB_OPENOCD_SCRIPTS=/path/to/openocd/scripts"
  fi
  return $bad
}

echo "[sbb] 环境已载入 · SBB_ROOT=$SBB_ROOT"
echo "[sbb] 执行 'sbb-env-check' 检查工具链"
