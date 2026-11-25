#ifndef NITRO_CONNECTION_H
#define NITRO_CONNECTION_H
#include "wire.h"
#include "link.h"
#include "discovery.h"
#define CONNECTION_APPLIED LINK_APPLIED
#define CONNECTION_STARTED LINK_START_APPLIED
#define CONNECTION_RENDER LINK_RENDER
#ifdef NN_MINIMAL
// The CPU stress fixture verifies the complete draw plus OLED transfer budget.
#define CONNECTION_DRAW_PERIODS 2100
#else
#define CONNECTION_DRAW_PERIODS 2300
#endif
// Volatile scheduled controller. Catalog/Link/Wire remain external owners.
typedef struct Connection {
    uint32_t deadline,send_after,step_at,render_until,render_context;
    uint16_t seed,render_poll;
    uint8_t packet[LINK_PACKET_MAX],size,waiting,active,render_flags;
} Connection;
// Initialize before discovery; cable recovery must retain this controller/Link.
// Preserve a held first Link packet when handing off from discovery.
void connection_init(Connection *connection,uint16_t seed,uint32_t now);
// Opt in before discovery. A RENDER event permits one bounded OLED update;
// keep calling the controller afterwards to resume at its scheduled boundary.
void connection_enable_render(Connection *connection);
// Sample controls through link_sample; drive wire.low after each call.
// OLED work is permitted only after CONNECTION_RENDER, and must complete within
// CONNECTION_DRAW_PERIODS * wire.period. All other bus service is exclusive.
uint8_t connection_step(Connection *connection,Wire *wire,Discovery *catalog,Link *link,uint32_t now,uint8_t high);
#endif
