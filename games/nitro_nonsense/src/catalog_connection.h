#ifndef NITRO_CATALOG_CONNECTION_H
#define NITRO_CATALOG_CONNECTION_H
#include "catalog.h"
#include "stream_connection.h"
// Keep catalog recovery until the shared runtime accepts its first valid POLL.
uint8_t connection_catalog_step(Connection *connection,Wire *wire,Catalog *catalog,Stream *stream,uint32_t now,uint8_t high);
#endif
