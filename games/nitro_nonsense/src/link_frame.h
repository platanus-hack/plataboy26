#ifndef NITRO_LINK_FRAME_H
#define NITRO_LINK_FRAME_H
#include "link.h"
// Internal envelope owner shared by versioned transaction codecs.
static inline void put16(uint8_t *out,uint16_t value){out[0]=value;out[1]=value>>8;}
static inline uint16_t get16(const uint8_t *in){return (uint16_t)in[0]|((uint16_t)in[1]<<8);}
void link_frame_header(const Link *link,uint8_t kind,uint16_t sequence,uint8_t *out);
uint8_t link_frame_valid(const Link *link,const uint8_t *packet,uint8_t size);
// Call only after link_frame_valid. Unknown control kinds are rejected.
uint8_t link_control_receive(Link *link,const uint8_t *packet);
#endif
