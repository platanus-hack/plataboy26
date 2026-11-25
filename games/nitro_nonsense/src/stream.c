#include "stream.h"
#include "link_frame.h"
#include "overlay.h"
static uint8_t members(const Link *link){return (1U<<link->count)-1;}
void stream_sample(Stream *stream,uint8_t held){
    stream->raw_input=held&63;stream->action_lock&=stream->raw_input;
    link_sample(&stream->link,stream->raw_input&~stream->action_lock);
}
void stream_block_actions(Stream *stream){
    stream->action_lock|=stream->raw_input&LINK_ACTION_MASK;
    stream->link.local_input&=~stream->action_lock;
    stream->link.reply_input&=~stream->action_lock;
}
static uint8_t same_payload(const Link *link,const uint8_t *packet){
    if(link->kind!=packet[0])return 0;
    if(link->kind==LINK_STREAM_START_PACKET)return get16(packet+9)==link->seed;
    for(uint8_t i=0;i<LINK_MAX_PEERS;i++)if(packet[9+i]!=link->controls[i])return 0;
    return packet[13]==link->steps;
}
/* Startup helpers return Link events before the resident session owner applies Game. */
NN_OV2 static uint8_t propose_start(Link *link,uint16_t seed,uint8_t packet[LINK_PACKET_MAX]){
    if(link->polled!=members(link)||link->ready!=members(link))return 0;
    link->sequence=link->have_last?(uint16_t)(link->last+1):0;
    link->kind=LINK_STREAM_START_PACKET;link->steps=0;link->seed=seed?seed:1;
    link->prepared=1;link->phase=STREAM_WAIT;link->acks=0;link->polled=0;
    link_frame_header(link,LINK_STREAM_START_PACKET,link->sequence,packet);
    uint8_t size=link_packet_length(LINK_STREAM_START_PACKET);
    put16(packet+9,link->seed);put16(packet+size-2,link_crc16(packet,size-2));return size;
}
uint8_t stream_propose(Stream *stream,uint16_t seed,uint8_t steps,uint8_t packet[LINK_PACKET_MAX]){
    Link *link=&stream->link;
    if(link->self||link->phase||link->count<2||link->count>LINK_MAX_PEERS)return 0;
    if(!link->running)return NN_BANK_CALL(propose_start,link,seed,packet);
    if(link->acks!=members(link)||!steps||steps>3)return 0;
    for(uint8_t i=1;i<link->count;i++)if(stream->next[i]&~63)return 0;
    link->sequence=link->have_last?(uint16_t)(link->last+1):0;
    link->kind=LINK_STREAM_INPUT_PACKET;link->steps=steps;
    for(uint8_t i=0;i<LINK_MAX_PEERS;i++)
        link->controls[i]=i<link->count?(i?stream->next[i]:link->local_input):0;
    link->prepared=1;link->phase=STREAM_WAIT;link->acks=0;link->polled=0;
    link_frame_header(link,LINK_STREAM_INPUT_PACKET,link->sequence,packet);
    for(uint8_t i=0;i<LINK_MAX_PEERS;i++)packet[9+i]=link->controls[i];
    packet[13]=link->steps;
    uint8_t size=link_packet_length(LINK_STREAM_INPUT_PACKET);
    put16(packet+size-2,link_crc16(packet,size-2));return size;
}
uint8_t stream_ack(const Stream *stream,uint8_t packet[LINK_PACKET_MAX]){
    const Link *link=&stream->link;
    if(!link->self||link->self>=link->count||!link->running||!link->have_last)return 0;
    link_frame_header(link,LINK_STREAM_ACK_PACKET,link->last,packet);
    packet[9]=link->self;packet[10]=link->reply_input;
    put16(packet+11,link_crc16(packet,11));return 13;
}
static uint8_t apply(Link *link,uint16_t sequence,uint8_t own){
    uint8_t event=LINK_APPLIED;
    link->last=sequence;link->have_last=1;link->prepared=0;link->fault=0;
    if(own)link->acks|=1;
    else{link->phase=LINK_IDLE;link->reply_input=link->local_input;event|=STREAM_ACK_NEEDED;}
    return event;
}
NN_OV2 static uint8_t apply_start(Link *link,uint16_t sequence,uint8_t own){
    link->running=1;return apply(link,sequence,own)|LINK_START_APPLIED;
}
NN_OV2 static uint8_t receive_start(Link *link,const uint8_t *packet,uint16_t sequence){
    if(link->running||!link->joined||link->count<2||!get16(packet+9))return 0;
    link->seed=get16(packet+9);link->steps=0;
    link->sequence=sequence;link->kind=LINK_STREAM_START_PACKET;
    return apply_start(link,sequence,0);
}
uint8_t stream_sent(Stream *stream,const uint8_t *packet,uint8_t size){
    Link *link=&stream->link;
    if(!link_frame_valid(link,packet,size)||
       (packet[0]!=LINK_STREAM_START_PACKET&&packet[0]!=LINK_STREAM_INPUT_PACKET)||link->self||link->phase!=STREAM_WAIT||
       get16(packet+1)!=link->sequence||!same_payload(link,packet)||!link->prepared)return 0;
    if(link->kind==LINK_STREAM_START_PACKET){
        if(!link->running)return NN_BANK_CALL(apply_start,link,link->sequence,1);
        return apply(link,link->sequence,1)|LINK_START_APPLIED;
    }
    return apply(link,link->sequence,1);
}
__attribute__((noinline)) uint8_t stream_receive(Stream *stream,const uint8_t *packet,uint8_t size){
    Link *link=&stream->link;
    if(!link_frame_valid(link,packet,size))return 0;
    uint8_t kind=packet[0];uint16_t sequence=get16(packet+1);
#ifdef NN_MINIMAL
    if(kind==LINK_RENDER_PACKET)
#else
    if(kind==LINK_RENDER_PACKET||(!link->running&&(kind==LINK_POLL_PACKET||kind==LINK_REPLY_PACKET)))
#endif
        return link_control_receive(link,packet);
    if(kind==LINK_STREAM_ACK_PACKET){
        uint8_t peer=packet[9],input=packet[10];
        if(link->self||link->phase!=STREAM_WAIT||link->prepared||!link->have_last||sequence!=link->last||
           !peer||peer>=link->count||(input&~63))return 0;
        uint8_t bit=1U<<peer;
        if((link->acks&bit)&&stream->next[peer]!=input)return 0;
        stream->next[peer]=input;link->acks|=bit;
        if(link->acks==members(link)){link->phase=LINK_IDLE;link->fault=0;}
        return LINK_ACK;
    }
    if(!link->self||link->phase||(kind!=LINK_STREAM_START_PACKET&&kind!=LINK_STREAM_INPUT_PACKET))return 0;
    /* A retried START remains ACKable after the startup bank has been replaced. */
    if(link->have_last&&sequence==link->last){
        if(!same_payload(link,packet))return 0;
        link->fault=0;return STREAM_ACK_NEEDED;
    }
    if(sequence!=(link->have_last?(uint16_t)(link->last+1):0))return 0;
    if(kind==LINK_STREAM_START_PACKET){
        if(link->running)return 0;
        return NN_BANK_CALL(receive_start,link,packet,sequence);
    }
    if(!link->running||!packet[13]||packet[13]>3)return 0;
    for(uint8_t i=0;i<LINK_MAX_PEERS;i++)if((packet[9+i]&~63)||(i>=link->count&&packet[9+i]))return 0;
    for(uint8_t i=0;i<LINK_MAX_PEERS;i++)link->controls[i]=packet[9+i];
    link->steps=packet[13];
    link->sequence=sequence;link->kind=kind;return apply(link,sequence,0);
}
