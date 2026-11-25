#ifndef NN_LOAD_PAUSE_H
#define NN_LOAD_PAUSE_H
#include "stream_connection.h"
#define LOAD_PAUSED 16
#define LOAD_FAILED 32
void load_pause_begin(Connection *c,Wire *w);
void load_pause_end(Connection *c,Wire *w,uint8_t result,uint32_t now);
uint8_t load_pause_wait(Connection *c,Wire *w,Stream *s,uint32_t now,uint8_t high);
#endif
