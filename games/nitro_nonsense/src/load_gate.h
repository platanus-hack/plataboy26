#ifndef NITRO_LOAD_GATE_H
#define NITRO_LOAD_GATE_H
#include "connection.h"

/* A resident barrier reuses the idle controller's packet and deadlines. */
typedef struct {
    uint8_t group, state, peer, fragment, fragments, error;
} LoadGate;

#define LOAD_GATE_NORMAL 1
#define LOAD_GATE_LOAD 2
#define LOAD_GATE_RESUMED 4
#define LOAD_GATE_FAILED 8
#define LOAD_GATE_ID_PACKET 0xb0
#define LOAD_GATE_GRANT_PACKET 0xb1
#define LOAD_GATE_POLL_PACKET 0xb2
#define LOAD_GATE_RESUME_PACKET 0xb3
#define LOAD_GATE_REPLY_PACKET 0xb4

/* The host enters only after every previous input ACK. Clients may enter early;
   NORMAL preserves events for the prior Catalog/Stream owner until the first
   valid identity fragment. The caller selects that prior owner. */
uint8_t load_gate_begin(LoadGate *gate, Connection *connection, Wire *wire,
                        const Link *link, uint8_t group, uint32_t now);
/* LOAD has already asserted wire.low. Drive the physical pin before calling the
   blocking resident loader, then report its result with load_gate_complete.
   RESUMED on a client preserves the first new ordinary Wire event for routing. */
uint8_t load_gate_step(LoadGate *gate, Connection *connection, Wire *wire,
                       const Link *link, const uint8_t build_id[32],
                       uint32_t now, uint8_t high);
void load_gate_complete(LoadGate *gate, Connection *connection, Wire *wire,
                        uint8_t result, uint32_t now);
#endif
