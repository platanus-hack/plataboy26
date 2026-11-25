#ifndef NITRO_STREAM_CONNECTION_H
#define NITRO_STREAM_CONNECTION_H
#include "connection.h"
#include "stream.h"
uint8_t connection_stream_step(Connection *connection,Wire *wire,Discovery *catalog,Stream *stream,uint32_t now,uint8_t high);
// Service a bound catalog after its owner hands off the first session POLL.
// Clients must continue catalog recovery until connection.active becomes true,
// even after binding; an unrelated or invalid candidate frame does not hand off.
uint8_t connection_stream_service(Connection *connection,Wire *wire,Stream *stream,uint32_t now,uint8_t high);
#endif
