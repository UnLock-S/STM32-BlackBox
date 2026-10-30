# ─────────────────────────────────────────────────────────────────────────────
#  check-no-malloc.cmake —— ADR-10 编译期门禁（构建后自动执行）
# ─────────────────────────────────────────────────────────────────────────────
#  用法（由 CMakeLists.txt 的 POST_BUILD 调用）：
#    cmake -DNM=<arm-none-eabi-nm> -DELF=<elf 路径> -P check-no-malloc.cmake
#
#  为什么需要它：
#    ADR-10 要求「禁用动态内存」。仅靠 configSUPPORT_DYNAMIC_ALLOCATION=0 与
#    不编译 heap_*.c 还不够 —— newlib 的 stdio（findfp.o）会自己引用 _malloc_r，
#    而 --specs=nosys.specs 提供 _sbrk 桩，于是 malloc 仍能链接进镜像。
#    → 本脚本在每次构建后检查 ELF 的已定义符号，出现 malloc 族就 **直接 fail**。
#      ADR-10 由此从「配置约定」变成「构建期强制」。
# ─────────────────────────────────────────────────────────────────────────────

if(NOT NM OR NOT ELF)
    message(FATAL_ERROR "check-no-malloc.cmake: 需要 -DNM=<nm路径> 与 -DELF=<elf路径>")
endif()

execute_process(
    COMMAND "${NM}" --defined-only "${ELF}"
    OUTPUT_VARIABLE nm_out
    ERROR_VARIABLE  nm_err
    RESULT_VARIABLE nm_rc
)

if(NOT nm_rc EQUAL 0)
    message(WARNING "ADR-10 门禁：nm 执行失败（rc=${nm_rc}），跳过检查。${nm_err}")
    return()
endif()

string(REPLACE "\r\n" "\n" nm_out "${nm_out}")
string(REPLACE "\n" ";" nm_lines "${nm_out}")

set(bad "")
foreach(line IN LISTS nm_lines)
    if(line MATCHES "(^| )(malloc|_malloc_r|pvPortMalloc|vPortFree|free|_free_r|calloc|_calloc_r|realloc|_realloc_r|_sbrk|_sbrk_r)$")
        list(APPEND bad "  ${line}")
    endif()
endforeach()

if(bad)
    list(JOIN bad "\n" bad_txt)
    message(FATAL_ERROR
        "❌ ADR-10 违规：ELF 中出现动态分配符号（本工程禁止动态内存）\n"
        "${bad_txt}\n"
        "排查方向：是否引入了 newlib stdio（printf/vsnprintf/malloc）、\n"
        "FreeRTOS heap_*.c，或直接调用了 malloc/calloc/realloc/free。")
else()
    message(STATUS "✅ ADR-10 门禁通过：ELF 无动态分配符号")
endif()
