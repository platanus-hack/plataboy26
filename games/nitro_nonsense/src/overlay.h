#ifndef NN_OVERLAY_H
#define NN_OVERLAY_H

#include <stdint.h>

#if defined(NN_OVERLAYS) && defined(__riscv)
#define NN_OV1 __attribute__((section(".ov1.text"), noinline))
#define NN_OV2 __attribute__((section(".ov2.text"), noinline))
#define NN_OV3 __attribute__((section(".ov3.text"), noinline))
#define NN_OV4 __attribute__((section(".ov4.text"), noinline))
#define NN_OV1_LOCAL __attribute__((section(".ov1.text")))
#define NN_OV2_LOCAL __attribute__((section(".ov2.text")))
#define NN_OV3_LOCAL __attribute__((section(".ov3.text")))
#define NN_OV4_LOCAL __attribute__((section(".ov4.text")))
#define NN_FRAMEBUFFER __attribute__((section(".bss.nn_framebuffer"), aligned(4)))
/* A typed indirect edge prevents short tail-jump relocations across banks. */
#define NN_BANK_CALL(function, ...) \
    ({ __typeof__(&(function)) volatile nn_entry = &(function); nn_entry(__VA_ARGS__); })
#else
#define NN_OV1
#define NN_OV2
#define NN_OV3
#define NN_OV4
#define NN_OV1_LOCAL
#define NN_OV2_LOCAL
#define NN_OV3_LOCAL
#define NN_OV4_LOCAL
#define NN_FRAMEBUFFER
#define NN_BANK_CALL(function, ...) (function)(__VA_ARGS__)
#endif

enum {
    NN_OVERLAY_NONE,
    NN_OVERLAY_CATALOG,
    NN_OVERLAY_GATE,
    NN_OVERLAY_RACE,
    NN_OVERLAY_CHOICES,
    NN_OVERLAY_SETUP = NN_OVERLAY_GATE,
    NN_OVERLAY_COUNT = 4
};

enum {
    NN_OVERLAY_OK,
    NN_OVERLAY_FORMAT,
    NN_OVERLAY_RANGE,
    NN_OVERLAY_READ,
    NN_OVERLAY_INTEGRITY,
    NN_OVERLAY_FLASH
};

#define NN_OVERLAY_META_SIZE 112
#define NN_OVERLAY_PAGE_SIZE 64
#define NN_OVERLAY_RAM_CODE_SIZE (1024 - NN_OVERLAY_PAGE_SIZE)

/* Call only from a resident stack after the complete cohort is quiescent.
 * The loader borrows the complete framebuffer: bytes 0..63 are scratch and the
 * rest hold verified RAM code. No drawing, OLED transfer or buffer reader may run
 * until return. Clear/redraw the full buffer before displaying it.
 * Success verifies the entire window, including its unused tail. */
uint8_t nn_overlay_load(uint8_t group);
uint8_t nn_overlay_current(void);
const uint8_t *nn_overlay_build_id(void);

#endif
