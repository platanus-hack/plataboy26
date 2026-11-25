#ifndef NITRO_COMPACT_CATALOG_H
#define NITRO_COMPACT_CATALOG_H
#include "wire.h"
#include "link.h"
#define CATALOG_READY 1
#define CATALOG_FAILED 2
#define CATALOG_DATA 4
#define CATALOG_RENDER 8
#if defined(NN_OVERLAYS)
#define CATALOG_RULES 0x010b
#elif defined(NN_MINIMAL)
#define CATALOG_RULES 0x010a
#else
#define CATALOG_RULES 0x0105
#endif
// Pre-race catalog only: no Game or persistent storage access.
// Minimal bound closes admission before the final replies and Stream binding.
typedef struct Catalog {
    uint32_t epoch,serial,deadline,send_after;
    uint16_t rules,remote_rules,tag;
    uint8_t identity[WIRE_ID_BYTES],ids[LINK_MAX_PEERS][WIRE_ID_BYTES],tx[WIRE_FRAME_MAX];
    uint8_t count,self,host,phase,size,retries,sealed,ready,error,cursor,stage,bound;
#ifdef NN_MINIMAL
    uint32_t render_until;
    uint8_t joined,mask,shown;
#endif
} Catalog;
void catalog_init(Catalog *catalog,Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint16_t rules,uint32_t entropy,uint32_t now);
uint8_t catalog_step(Catalog *catalog,Wire *wire,uint32_t now,uint8_t high);
uint8_t catalog_bind(Catalog *catalog,Link *link);
#endif
