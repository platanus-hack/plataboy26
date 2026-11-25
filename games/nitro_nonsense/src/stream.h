#ifndef NITRO_STREAM_H
#define NITRO_STREAM_H
#include "link.h"
#define STREAM_ACK_NEEDED LINK_COMMIT
#define STREAM_WAIT 1
// Zero-initialize before configuration. Identity/lobby fields retain Link ownership.
// Current and next input vectors
// stay separate so retries cannot overwrite an already proposed transaction.
typedef struct Stream {Link link;uint8_t next[LINK_MAX_PEERS],raw_input,action_lock;} Stream;
void stream_sample(Stream *stream,uint8_t held);
// Session routing calls this after an authoritative phase/actor transition.
void stream_block_actions(Stream *stream);
uint8_t stream_propose(Stream *stream,uint16_t seed,uint8_t steps,uint8_t packet[LINK_PACKET_MAX]);
uint8_t stream_ack(const Stream *stream,uint8_t packet[LINK_PACKET_MAX]);
uint8_t stream_receive(Stream *stream,const uint8_t *packet,uint8_t size);
// Only the controller's immutable pending frame may enter this TX-complete path.
uint8_t stream_sent(Stream *stream,const uint8_t *packet,uint8_t size);
#endif
