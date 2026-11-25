#ifndef NITRO_STREAM_SESSION_H
#define NITRO_STREAM_SESSION_H
#include "stream.h"
uint8_t session_stream_receive(Stream *stream,const uint8_t *packet,uint8_t size);
uint8_t session_stream_sent(Stream *stream,const uint8_t *packet,uint8_t size);
#endif
