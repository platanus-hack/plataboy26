#ifndef NITRO_SWIO_H
#define NITRO_SWIO_H
#include "wire.h"
#include "link.h"
struct Discovery;
struct Connection;
struct Stream;
struct Catalog;
// Caller owns the boot recovery input and exclusive communication windows.
// No ordinary app path currently enables this experimental binding.
uint8_t swio_begin(Wire *wire,uint8_t preserve_debug);
uint8_t swio_send(Wire *wire,const uint8_t *packet,uint8_t size);
uint8_t swio_elect(Wire *wire,const uint8_t identity[WIRE_ID_BYTES]);
uint8_t swio_scan(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint8_t participate,uint8_t initiate);
void swio_identity(uint8_t identity[WIRE_ID_BYTES]);
uint8_t swio_poll(Wire *wire);
uint8_t swio_discover(struct Discovery *discovery,Wire *wire);
uint8_t swio_connect(struct Connection *connection,struct Discovery *discovery,Link *link,Wire *wire);
uint8_t swio_stream_connect(struct Connection *connection,struct Discovery *discovery,struct Stream *stream,Wire *wire);
uint8_t swio_catalog_connect(struct Connection *connection,struct Catalog *catalog,struct Stream *stream,Wire *wire);
void swio_listen(Wire *wire);
void swio_end(void);
#endif
