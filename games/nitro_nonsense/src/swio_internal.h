#ifndef NITRO_SWIO_INTERNAL_H
#define NITRO_SWIO_INTERNAL_H
#include "wire.h"
// Shared physical fast path for the versioned source-owned controllers.
uint8_t swio_controller_ready(Wire *wire);
void swio_drive(uint8_t low);
#endif
