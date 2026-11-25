#ifndef NITRO_WIRE_H
#define NITRO_WIRE_H
#include <stdint.h>
#ifdef NN_MINIMAL
#define WIRE_BIT_US 32
#else
#define WIRE_BIT_US 24
#endif
#define WIRE_FRAME_MAX 16
// Twelve break periods, two released periods, length plus payload UART bytes.
#define WIRE_FRAME_PERIODS(size) (14U+10U*((size)+1U))
#define WIRE_RECEIVED 1
#define WIRE_SENT 2
#define WIRE_ERROR 4
#define WIRE_ELECTED 8
#define WIRE_IDENTIFIED 16
#define WIRE_SELECTED 32
#define WIRE_SCANNED 64
#define WIRE_EMPTY 128
#define WIRE_ID_BYTES 12
#define WIRE_ELECTION_QUIET_PERIODS 5000
// Volatile physical framing state. low=1 pulls down; low=0 releases the bus.
// Buffers/events remain owned by the caller until wire_listen acknowledges them.
typedef struct {
    uint32_t at,edge;
    uint16_t period;
    uint8_t state,low,high,seen,bit,byte,used,length,half,event;
    uint8_t data[WIRE_FRAME_MAX];
} Wire;
uint8_t wire_init(Wire *wire,uint16_t period,uint32_t now);
void wire_listen(Wire *wire,uint32_t now);
uint8_t wire_send(Wire *wire,const uint8_t *packet,uint8_t size,uint32_t now);
// Arm full-identity arbitration; caller must poll continuously through the break.
// Both result events retain the winner's complete identity in data[0..11].
uint8_t wire_elect(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint32_t now);
uint8_t wire_reselect(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint32_t now);
// Enumerate unknown identities without electing another coordinator.
// Registered nodes observe; only the coordinator initiates the scan marker.
uint8_t wire_scan(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint8_t participate,uint8_t initiate,uint32_t now);
uint8_t wire_step(Wire *wire,uint32_t now,uint8_t high);
// True only between frames/elections after an observed continuously released gap.
uint8_t wire_quiet(const Wire *wire,uint32_t now,uint16_t periods);
uint8_t wire_busy(const Wire *wire);
#endif
