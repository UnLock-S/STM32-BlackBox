# ─────────────────────────────────────────────────────────────────────────────
#  SBB 固件工程 —— CubeMX 生成骨架 + 手写 App 层
# ─────────────────────────────────────────────────────────────────────────────
#  构建：bash fw/build.sh
#   或：cmake -S fw -B fw/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=fw/cmake/arm-none-eabi.cmake
#       cmake --build fw/build
#
#  ★ 分工边界
#     cubemx/**  —— CubeMX 拥有（生成物，只改 USER CODE 段）→ 用 glob 收录，
#                    **再生成后本文件无需改动**
#     App/**     —— 手写应用代码（CubeMX 不认这个目录，再生成不会碰）
#
#  ★ 输出：fw/bin/ 下 sbb_app.elf / .hex / .bin / .map
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.20)

project(sbb_app LANGUAGES C ASM)

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "必须指定工具链文件：-DCMAKE_TOOLCHAIN_FILE=<fw>/cmake/arm-none-eabi.cmake")
endif()

set(MCU_FLAGS -mcpu=cortex-m3 -mthumb -mfloat-abi=soft)

# ── 路径 ────────────────────────────────────────────────────────────────────
set(CUBEMX  ${CMAKE_CURRENT_SOURCE_DIR}/cubemx)     # ★ CubeMX 工程根（生成物）
set(APPDIR  ${CMAKE_CURRENT_SOURCE_DIR}/App)        # ★ 手写应用代码
set(OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
file(MAKE_DIRECTORY ${OUT_DIR})

set(CUBEMX_LD      ${CUBEMX}/STM32F103xx_FLASH.ld)      # Bootloader 用（0x08000000 + 8K）
set(APP_LD         ${APPDIR}/sbb_app.ld)                 # ★ App 用（0x08002000 + 56K）
set(CUBEMX_STARTUP ${CUBEMX}/startup_stm32f103xb.s)

# ── 1) CubeMX 生成物：glob 收录（新增文件自动被发现）──────────────────────
file(GLOB CUBEMX_CORE CONFIGURE_DEPENDS ${CUBEMX}/Core/Src/*.c)
file(GLOB CUBEMX_HAL  CONFIGURE_DEPENDS  ${CUBEMX}/Drivers/STM32F1xx_HAL_Driver/Src/*.c)
file(GLOB CUBEMX_RTOS CONFIGURE_DEPENDS ${CUBEMX}/Middlewares/Third_Party/FreeRTOS/Source/*.c)
set(CUBEMX_RTOS_PORT
    ${CUBEMX}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c)

# ★★ ADR-10 硬约束：排除 sysmem.c
#    sysmem.c 提供 _sbrk()，有它 newlib 的 malloc 就能链接成功（工程里并没有
#    任何 FreeRTOS heap_*.c）。排除之后，**任何动态分配都会在链接期报
#    undefined reference to `_sbrk`** —— ADR-10 从「配置约定」升级为「编译期强制」。
list(REMOVE_ITEM CUBEMX_CORE ${CUBEMX}/Core/Src/sysmem.c)

# ★ 必须收 CMSIS-RTOS 封装（与 CubeMX 生成的 Makefile 一致）：
#   CubeMX 生成的 Core/Src/main.c 用的是 **CMSIS-RTOS V1 API**
#   （osKernelInitialize / osKernelStart / osThreadStaticDef / osThreadCreate / osDelay），
#   不收 cmsis_os.c 会 `fatal error: cmsis_os.h` 且链接缺符号。
#   App/ 层本身仍只用 FreeRTOS 原生 API（见 App/sched/），CMSIS 封装只服务生成代码。
set(CUBEMX_RTOS_CMSIS
    ${CUBEMX}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS/cmsis_os.c)

# ── 2) 手写应用代码 ────────────────────────────────────────────────────────
# ★ 串口调试命令开关：OFF 时整块 app_console.c 被裁掉（省 ~3.3 KB Flash + 1.5 KB RAM）
option(SBB_WITH_CONSOLE "Build the serial debug console" ON)

file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS ${APPDIR}/*.c)

if(SBB_WITH_CONSOLE)
  add_compile_definitions(SBB_WITH_CONSOLE=1)
else()
  add_compile_definitions(SBB_WITH_CONSOLE=0)
endif()

add_executable(sbb_app
    ${APP_SRC}
    ${CUBEMX_CORE}
    ${CUBEMX_HAL}
    ${CUBEMX_RTOS}
    ${CUBEMX_RTOS_PORT}
    ${CUBEMX_RTOS_CMSIS}
    ${CUBEMX_STARTUP}
    ${CMAKE_CURRENT_SOURCE_DIR}/boot/boot_flash.c
)

set_target_properties(sbb_app PROPERTIES
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}
)

set_source_files_properties(${CUBEMX_STARTUP} PROPERTIES LANGUAGE ASM)

# ── 3) 包含路径（与 CubeMX 生成的 Makefile 保持一致）───────────────────────
# ★ App/ 下的子目录自动收集 —— 本轮就因漏加 App/drv 触发
#   "fatal error: ssd1306.h: No such file or directory"。
#   改成 GLOB 后，新增 App/xxx/ 目录不再需要改构建脚本。
file(GLOB APP_SUBDIRS CONFIGURE_DEPENDS ${APPDIR}/*)
set(APP_INC_DIRS "")
foreach(_d IN LISTS APP_SUBDIRS)
    if(IS_DIRECTORY "${_d}")
        list(APPEND APP_INC_DIRS "${_d}")
    endif()
endforeach()

target_include_directories(sbb_app PRIVATE
    ${APP_INC_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/boot
    ${CUBEMX}/Core/Inc
    ${CUBEMX}/Drivers/STM32F1xx_HAL_Driver/Inc
    ${CUBEMX}/Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
    ${CUBEMX}/Drivers/CMSIS/Device/ST/STM32F1xx/Include
    ${CUBEMX}/Drivers/CMSIS/Include
    ${CUBEMX}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${CUBEMX}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS
    ${CUBEMX}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3
)

# ── 4) 编译选项（对齐 CubeMX 生成的 Makefile，保证两份构建等价）────────────
target_compile_options(sbb_app PRIVATE
    ${MCU_FLAGS}
    -Og
    -g -gdwarf-2
    -Wall
    -fdata-sections
    -ffunction-sections
    -DUSE_HAL_DRIVER
    -DSTM32F103xB
)

# ── 5) 链接选项 ────────────────────────────────────────────────────────────
target_link_options(sbb_app PRIVATE
    ${MCU_FLAGS}
    -T${APP_LD}
    -specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${OUT_DIR}/sbb_app.map,--cref
)

# ── 6) 产物：hex / bin + 尺寸报告 ──────────────────────────────────────────
find_program(OBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(SIZE    arm-none-eabi-size    REQUIRED)
find_program(NM      arm-none-eabi-nm      REQUIRED)

add_custom_command(TARGET sbb_app POST_BUILD
    COMMAND ${OBJCOPY} -O ihex   $<TARGET_FILE:sbb_app> ${OUT_DIR}/sbb_app.hex
    COMMAND ${OBJCOPY} -O binary -S $<TARGET_FILE:sbb_app> ${OUT_DIR}/sbb_app.bin
    COMMAND ${SIZE} $<TARGET_FILE:sbb_app>
    # ★ ADR-10 门禁：ELF 里出现 malloc 族就 fail（见 cmake/check-no-malloc.cmake）
    COMMAND ${CMAKE_COMMAND} -DNM=${NM} -DELF=$<TARGET_FILE:sbb_app>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-no-malloc.cmake
    COMMENT "输出到 bin/ + ADR-10 门禁"
)

add_custom_target(size
    COMMAND ${SIZE} ${OUT_DIR}/sbb_app.elf
    DEPENDS sbb_app
)


# ══════════════════════════════════════════════════════════════════════════════
#  Bootloader（裸机，无 RTOS）
# ══════════════════════════════════════════════════════════════════════════════
#  ★ 与 App 共用同一份 BSP（bsp_spi / bsp_sdcard / bsp_uart），
#    靠 -DBSP_WITH_RTOS=0 关掉互斥量，避免为一把锁把整个内核拉进 8 KB 的 bootloader。
file(GLOB BOOT_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/boot/*.c)

# bootloader 只需 HAL + Msp + system，不要 CubeMX 的 main.c/freertos.c/it.c/syscalls.c
set(BOOT_CORE
    ${CUBEMX}/Core/Src/stm32f1xx_hal_msp.c
    ${CUBEMX}/Core/Src/system_stm32f1xx.c
)

add_executable(sbb_boot
    ${BOOT_SRC}
    ${BOOT_CORE}
    ${CUBEMX_HAL}
    ${CUBEMX_STARTUP}
    ${APPDIR}/bsp/bsp_spi.c
    ${APPDIR}/bsp/bsp_uart.c
    ${APPDIR}/bsp/bsp_sdcard.c
)

set_target_properties(sbb_boot PROPERTIES
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}
)

set_source_files_properties(${CUBEMX_STARTUP} PROPERTIES LANGUAGE ASM)

target_include_directories(sbb_boot PRIVATE
    ${APPDIR}/bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/boot
    ${CUBEMX}/Core/Inc
    ${CUBEMX}/Drivers/STM32F1xx_HAL_Driver/Inc
    ${CUBEMX}/Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
    ${CUBEMX}/Drivers/CMSIS/Device/ST/STM32F1xx/Include
    ${CUBEMX}/Drivers/CMSIS/Include
)

target_compile_options(sbb_boot PRIVATE
    ${MCU_FLAGS}
    -Os
    -g -gdwarf-2
    -Wall
    -fdata-sections
    -ffunction-sections
    -DUSE_HAL_DRIVER
    -DSTM32F103xB
    -DBSP_WITH_RTOS=0
    -DVECT_TAB_OFFSET=0x00000000U
)

target_link_options(sbb_boot PRIVATE
    ${MCU_FLAGS}
    -T${CMAKE_CURRENT_SOURCE_DIR}/boot/sbb_boot.ld
    -specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${OUT_DIR}/sbb_boot.map,--cref
)

add_custom_command(TARGET sbb_boot POST_BUILD
    COMMAND ${OBJCOPY} -O ihex   $<TARGET_FILE:sbb_boot> ${OUT_DIR}/sbb_boot.hex
    COMMAND ${OBJCOPY} -O binary -S $<TARGET_FILE:sbb_boot> ${OUT_DIR}/sbb_boot.bin
    COMMAND ${SIZE} $<TARGET_FILE:sbb_boot>
    COMMENT "bootloader 输出到 bin/"
)

# 一条命令同时构建两者
add_custom_target(firmware ALL
    DEPENDS sbb_boot sbb_app
)
