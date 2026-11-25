#ifndef NITRO_DISCOVERY_H
#define NITRO_DISCOVERY_H
#include "wire.h"
#include "link.h"
#define DISCOVERY_READY 1
#define DISCOVERY_FAILED 2
#define DISCOVERY_DATA 4
#define DISCOVERY_FULL 1
#define DISCOVERY_INCOMPATIBLE 2
#define DISCOVERY_TIMEOUT 3
#define DISCOVERY_DUPLICATE 4
// Initial-catalog state only. No Game access, storage writes or player settings.
typedef struct Discovery {
    uint32_t epoch,serial,deadline,send_after;
    uint16_t rules,catalog_rules,tag;
    uint8_t identity[WIRE_ID_BYTES],ids[LINK_MAX_PEERS][WIRE_ID_BYTES],tx[WIRE_FRAME_MAX];
    uint8_t count,self,parts,host,phase,cursor,stage,tx_size,retries,error;
    // committed: 0 unconfirmed, 1 provisional result, 2 sealed session catalog.
    uint8_t leader_known,ready,bound,committed;
} Discovery;
void discovery_init(Discovery *discovery,Wire *wire,const uint8_t identity[WIRE_ID_BYTES],
                    uint16_t rules,uint32_t entropy,uint32_t now);
// Call continuously during exclusive discovery windows; drive wire.low afterwards.
// Heavy packet preparation returns before any outgoing frame is started.
uint8_t discovery_step(Discovery *discovery,Wire *wire,uint32_t now,uint8_t high);
uint8_t discovery_bind(Discovery *discovery,Link *link);
#endif
