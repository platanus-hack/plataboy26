#ifndef NITRO_LINK_H
#define NITRO_LINK_H
#include <stdint.h>
#define LINK_MAX_PEERS 4
#define LINK_ACTION_MASK 48
#define LINK_PACKET_MAX 16
#define LINK_PREPARE 1
#define LINK_COMMIT 2
#define LINK_APPLIED 4
#define LINK_START_APPLIED 8
#define LINK_POLL 16
#define LINK_REPLY 32
#define LINK_ACK 64
#define LINK_RENDER 128
#define LINK_IDLE 0
#define LINK_POLL_PACKET 0xa0
#define LINK_REPLY_PACKET 0xa4
#define LINK_START_PACKET 0xa1
#define LINK_INPUT_PACKET 0xa2
#define LINK_COMMIT_PACKET 0xa3
#define LINK_ACK_PACKET 0xa5
#define LINK_RENDER_PACKET 0xa6
#define LINK_STREAM_START_PACKET 0xa7
#define LINK_STREAM_INPUT_PACKET 0xa8
#define LINK_STREAM_ACK_PACKET 0xa9
// Zero-initialize before first configuration. This is volatile protocol state,
// never a saved-game format. Full roster identities belong to wire discovery.
// Only the session nonce must be nonzero; every 16-bit roster CRC is valid.
// Discovery must choose a new session for any full roster/slot/rules change.
typedef struct {
    uint32_t session;
    uint16_t roster,sequence,last,seed,poll_id;
    uint8_t count,self,joined,ready,polled,local_input,controls[4];
    uint8_t steps,kind,prepared,phase,acks,running,fault,have_last,reply_input,reply_joined;
} Link;
uint8_t link_configure(Link *link,uint32_t session,uint16_t roster,uint8_t count,uint8_t self);
void link_sample(Link *link,uint8_t held);
uint8_t link_poll(Link *link,uint8_t packet[LINK_PACKET_MAX]);
uint8_t link_render(const Link *link,uint8_t age,uint8_t packet[LINK_PACKET_MAX]);
uint8_t link_reply(const Link *link,uint8_t packet[LINK_PACKET_MAX]);
uint8_t link_propose(Link *link,uint16_t seed,uint8_t steps,uint8_t packet[LINK_PACKET_MAX]);
uint8_t link_retry(const Link *link,uint8_t packet[LINK_PACKET_MAX]);
uint8_t link_receive(Link *link,const uint8_t *packet,uint8_t size);
uint8_t link_ack_packet(const Link *link,uint8_t phase,uint16_t sequence,uint8_t packet[LINK_PACKET_MAX]);
void link_timeout(Link *link);
uint8_t link_packet_length(uint8_t kind);
// Shared logical-frame integrity; identities are still compared in full.
uint16_t link_crc16(const uint8_t *bytes,uint8_t size);
#endif
