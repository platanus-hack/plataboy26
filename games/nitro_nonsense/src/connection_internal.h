#ifndef NITRO_CONNECTION_INTERNAL_H
#define NITRO_CONNECTION_INTERNAL_H
#include "connection.h"
enum {TURNAROUND=32,REPLY_SLOT=250,APPLY_GUARD=400,TICK_PERIODS=33000/WIRE_BIT_US};
enum {RENDER_ENABLED=1,RENDER_SEEN=2,RENDER_PAUSED=4,RENDER_CONTEXT=8,
      RENDER_WORK=CONNECTION_DRAW_PERIODS,RENDER_HOLD=RENDER_WORK+300,RENDER_AGE_UNIT=8,RENDER_AGE_MARGIN=48};
uint8_t connection_resume(Connection *c,Wire *wire,uint32_t now);
void connection_context(Connection *c,uint32_t now);
void connection_fault(Connection *c,Link *link,uint32_t now);
uint8_t connection_render_window(Connection *c,const Link *link,const Wire *wire,uint32_t now,uint16_t key);
uint8_t connection_render_receive(Connection *c,Link *link,Wire *wire,uint32_t now,uint16_t key);
void connection_render_prepare(Connection *c,const Link *link,const Wire *wire,uint32_t now,uint16_t key);
void connection_render_hold(Connection *c,const Wire *wire,uint32_t now,uint16_t key);
#endif
