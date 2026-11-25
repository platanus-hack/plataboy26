#include "overlay.h"

static uint8_t verified_group;

#if defined(__GNUC__) && !defined(__clang__)
#define LOADER_ENTRY __attribute__((noinline,noclone))
#else
#define LOADER_ENTRY __attribute__((noinline))
#endif

uint8_t nn_overlay_current(void) { return verified_group; }

#ifndef NN_OVERLAYS
const uint8_t *nn_overlay_build_id(void) {
    static const uint8_t identity[32];
    return identity;
}
LOADER_ENTRY uint8_t nn_overlay_load(uint8_t group) {
    if(!group || group > NN_OVERLAY_COUNT) return NN_OVERLAY_FORMAT;
    verified_group = group;
    return NN_OVERLAY_OK;
}
#else
/* plataboy26 library build: each group is an LZ stream (firmware/tools/lz.py) that the
 * library kernel decompresses straight into the window (kunpack.S, kernel entry
 * KBASE + 8: the same decompressor that installs every program). The resident metadata
 * (format 3) holds, per group, the stream offset and size and the CRC32 of the full
 * window (unused tail = FF), checked after the unpack. The loader borrows the last 96
 * bytes of the framebuffer as the kernel's page buffer. */

#include "ch32v003.h"

extern uint8_t framebuffer[1024];
extern const volatile uint8_t __nn_overlay_meta[NN_OVERLAY_META_SIZE];
extern const uint8_t __nn_overlay_base[], __nn_overlay_size[];
#define window_base() ((uint32_t)(uintptr_t)__nn_overlay_base)
#define window_size() ((uint32_t)(uintptr_t)__nn_overlay_size)
#define KERNEL_UNPACK (*(const volatile uint32_t *)0x08003C48u)

void kunpack(uint32_t source, uint32_t from, uint32_t to, uint8_t *scratch);

const uint8_t *nn_overlay_build_id(void) {
    return (const uint8_t *)(__nn_overlay_meta + 20);
}

static uint32_t get32(const volatile uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

LOADER_ENTRY uint8_t nn_overlay_load(uint8_t group) {
    if(!group || group > NN_OVERLAY_COUNT) return NN_OVERLAY_FORMAT;
    if(group == verified_group) return NN_OVERLAY_OK;
    const volatile uint8_t *m = __nn_overlay_meta, *d = m + 52 + (group - 1u) * 12;
    /* a kernel without the unpack entry (word KBASE + 8 not a jump) */
    if(get32(m) != 0x564f4e4e || m[4] != 3 || (KERNEL_UNPACK & 0xfff) != 0x06f)
        return NN_OVERLAY_FORMAT;
    /* No retained tag can authorize a partly replaced body, including after reset. */
    verified_group = NN_OVERLAY_NONE;
    FLASH->KEYR = 0x45670123;
    FLASH->KEYR = 0xcdef89ab;
    FLASH->MODEKEYR = 0x45670123;
    FLASH->MODEKEYR = 0xcdef89ab;
    kunpack(get32(m + 8) + get32(d), window_base(), window_base() + window_size(),
              framebuffer + 1024 - 96);
    FLASH->CTLR = FLASH_CTLR_LOCK | FLASH_CTLR_FLOCK;
    uint32_t crc = 0xffffffffu;
    for(const uint8_t *p = (const uint8_t *)window_base(); p < (const uint8_t *)window_base() + window_size(); p++) {
        crc ^= *p;
        for(uint8_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    if(~crc != get32(d + 8)) return NN_OVERLAY_INTEGRITY;
    /* FENCE.I synchronizes instruction fetch after verified self-programming. */
    __asm__ volatile(".word 0x0000100f" ::: "memory");
    verified_group = group;
    return NN_OVERLAY_OK;
}

#endif
