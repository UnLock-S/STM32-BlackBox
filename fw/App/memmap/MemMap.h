/*
 * MemMap.h -- AUTOSAR-style memory mapping (SBB application)
 *
 * 用法与 AUTOSAR SWS_MemMap 一致：
 *
 *     #define SBB_START_SEC_VERSION
 *     #include "MemMap.h"
 *     /* 这一段的定义会落到 .sbb_version 段 *\/
 *     #define SBB_STOP_SEC_VERSION
 *     #include "MemMap.h"
 *
 * 注意：本文件【故意不设 include guard】—— AUTOSAR 规定 MemMap.h
 *       要能被反复包含，靠 START/STOP 宏自己判断本次该展开哪段。
 *       加了 guard 会导致第二次包含失效、STOP 段不生效。
 */
#ifdef SBB_START_SEC_VERSION
# undef SBB_START_SEC_VERSION
# define SBB_START_SEC_VERSION
# define SBB_VAR_VERSION  __attribute__((section(".sbb_version"), used))
#endif

#ifdef SBB_STOP_SEC_VERSION
# undef SBB_STOP_SEC_VERSION
# define SBB_STOP_SEC_VERSION
# undef SBB_VAR_VERSION
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * 其它标准段（AUTOSAR 命名的对应关系）
 *
 *   START/STOP 对                落到的 ELF 段    说明
 *   SBB_START_SEC_CODE           .text           普通代码
 *   SBB_START_SEC_CONST          .rodata         只读常量
 *   SBB_START_SEC_VAR_INIT       .data           有初值的变量
 *   SBB_START_SEC_VAR_NOINIT     .bss            零初始化变量
 *   SBB_START_SEC_VAR_POWER_ON   .bss            上电保持（本板无备份域，同 bss）
 *   SBB_START_SEC_VERSION        .sbb_version    固定 0x08003A00 的版本指纹
 *
 * 用法（和上面 VERSION 段完全一样）：
 *     #define SBB_START_SEC_VAR_NOINIT
 *     #include "MemMap.h"
 *     static uint8_t s_buf[512] SBB_VAR_NOINIT;
 *     #define SBB_STOP_SEC_VAR_NOINIT
 *     #include "MemMap.h"
 *
 * 说明：本工程用 GCC，属性写在【定义】上，所以宏要放在变量名后面；
 *       AUTOSAR 的 Tasking/GHS 写法是放在类型前面的 pragma。
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef SBB_START_SEC_CODE
# undef SBB_START_SEC_CODE
# define SBB_START_SEC_CODE
# define SBB_CODE  __attribute__((section(".text")))
#endif
#ifdef SBB_STOP_SEC_CODE
# undef SBB_STOP_SEC_CODE
# define SBB_STOP_SEC_CODE
# undef SBB_CODE
#endif

#ifdef SBB_START_SEC_CONST
# undef SBB_START_SEC_CONST
# define SBB_START_SEC_CONST
# define SBB_CONST __attribute__((section(".rodata")))
#endif
#ifdef SBB_STOP_SEC_CONST
# undef SBB_STOP_SEC_CONST
# define SBB_STOP_SEC_CONST
# undef SBB_CONST
#endif

#ifdef SBB_START_SEC_VAR_INIT
# undef SBB_START_SEC_VAR_INIT
# define SBB_START_SEC_VAR_INIT
# define SBB_VAR_INIT __attribute__((section(".data")))
#endif
#ifdef SBB_STOP_SEC_VAR_INIT
# undef SBB_STOP_SEC_VAR_INIT
# define SBB_STOP_SEC_VAR_INIT
# undef SBB_VAR_INIT
#endif

#ifdef SBB_START_SEC_VAR_NOINIT
# undef SBB_START_SEC_VAR_NOINIT
# define SBB_START_SEC_VAR_NOINIT
# define SBB_VAR_NOINIT __attribute__((section(".bss")))
#endif
#ifdef SBB_STOP_SEC_VAR_NOINIT
# undef SBB_STOP_SEC_VAR_NOINIT
# define SBB_STOP_SEC_VAR_NOINIT
# undef SBB_VAR_NOINIT
#endif
