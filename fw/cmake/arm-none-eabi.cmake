# ─────────────────────────────────────────────────────────────────────────────
#  arm-none-eabi.cmake —— CMake 交叉编译工具链文件 (STM32F103C8T6 / Cortex-M3)
# ─────────────────────────────────────────────────────────────────────────────
#  ★ 为什么必须单独成文件：
#    CMAKE_SYSTEM_NAME / CMAKE_TRY_COMPILE_TARGET_TYPE 只有在 **第一次 project()/
#    enable_language() 之前** 生效。写在 CMakeLists.txt 的 project() 之后是无效的，
#    CMake 会去链接一个宿主机可执行文件来做编译器探测 → 直接失败。
#    放在 toolchain 文件里并用 -DCMAKE_TOOLCHAIN_FILE 传入，才能保证在 project() 前生效。
#
#  用法：
#    cmake -S fw -B fw/build -G Ninja \
#          -DCMAKE_TOOLCHAIN_FILE=fw/cmake/arm-none-eabi.cmake
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  arm)

# 交叉编译时生成的 exe 无法在宿主机运行 —— 用静态库模式做编译器探测
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── 编译器：靠 tools/env.sh 把 ARM GNU Toolchain 放进 PATH ──
set(CMAKE_C_COMPILER    arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER  arm-none-eabi-gcc)
set(CMAKE_OBJCOPY       arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP       arm-none-eabi-objdump)
set(CMAKE_SIZE          arm-none-eabi-size)
set(CMAKE_AR            arm-none-eabi-ar)
set(CMAKE_RANLIB        arm-none-eabi-ranlib)

# ── 不要在宿主机上找库/头文件 ──
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 默认构建类型
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
