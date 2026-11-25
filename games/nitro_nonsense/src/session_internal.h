#ifndef NITRO_SESSION_INTERNAL_H
#define NITRO_SESSION_INTERNAL_H
#include "link.h"
// Both versioned codecs enter the same canonical Game application boundary.
uint8_t session_apply(Link *link,uint8_t event);
#ifdef NN_OVERLAYS
void session_load_begin(void);
void session_load_group(uint8_t group);
void session_load_end(void);
#endif
#endif
