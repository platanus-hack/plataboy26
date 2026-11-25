#include "link_frame.h"

static uint8_t members(const Link *link){return (1<<link->count)-1;}
uint16_t link_crc16(const uint8_t *bytes,uint8_t size){
    uint16_t crc=65535;
    for(uint8_t i=0;i<size;i++){
        crc^=(uint16_t)bytes[i]<<8;
        for(uint8_t bit=0;bit<8;bit++)crc=(crc<<1)^((crc&32768)?0x1021:0);
    }
    return crc;
}
uint8_t link_packet_length(uint8_t kind){
#ifdef NN_OVERLAYS
    if(kind>=0xb0&&kind<=0xb4)return 16;
#endif
    if(kind==LINK_POLL_PACKET||kind==LINK_RENDER_PACKET)return 15;
    if(kind==LINK_REPLY_PACKET)return 14;
    if(kind==LINK_START_PACKET)return 13;
    if(kind==LINK_INPUT_PACKET)return 16;
    if(kind==LINK_COMMIT_PACKET)return 11;
    if(kind==LINK_ACK_PACKET)return 13;
    if(kind==LINK_STREAM_START_PACKET||kind==LINK_STREAM_ACK_PACKET)return 13;
    if(kind==LINK_STREAM_INPUT_PACKET)return 16;
    return 0;
}
void link_frame_header(const Link *link,uint8_t kind,uint16_t sequence,uint8_t *out){
    out[0]=kind;put16(out+1,sequence);put16(out+7,link->roster);
    for(uint8_t i=0;i<4;i++)out[3+i]=link->session>>(i*8);
}
static uint8_t encode(const Link *link,uint8_t kind,uint8_t *out){
    link_frame_header(link,kind,(kind==LINK_POLL_PACKET||kind==LINK_REPLY_PACKET)?link->poll_id:link->sequence,out);
    uint8_t n=9;
    if(kind==LINK_POLL_PACKET){out[n++]=link->ready;out[n++]=link->running;put16(out+n,link->last);n+=2;}
    if(kind==LINK_REPLY_PACKET){out[n++]=link->self;out[n++]=link->reply_input;out[n++]=link->reply_joined;}
    if(kind==LINK_START_PACKET){put16(out+n,link->seed);n+=2;}
    if(kind==LINK_INPUT_PACKET){for(uint8_t i=0;i<4;i++)out[n++]=link->controls[i];out[n++]=link->steps;}
    put16(out+n,link_crc16(out,n));return n+2;
}
uint8_t link_configure(Link *link,uint32_t session,uint16_t roster,uint8_t count,uint8_t self){
    if(!session||!count||count>LINK_MAX_PEERS||self>=count)return 0;
    // Reconfiguration cannot discard an in-flight or running session.
    if(link->running||link->phase)return 0;
    if(link->session==session){
        if(link->roster!=roster||link->count!=count||link->self!=self)return 0;
        return 1;
    }
    uint8_t joined=link->session?0:link->joined;
    uint8_t *bytes=(uint8_t *)link;
    for(uint8_t i=0;i<sizeof(*link);i++)bytes[i]=0;
    link->session=session;link->roster=roster;link->count=count;link->self=self;
    link->joined=joined;link->ready=joined?(1<<self):0;return 1;
}
void link_sample(Link *link,uint8_t held){
    link->local_input=held&63;
    if(!link->running&&(held&16))link->joined=1;
}
uint8_t link_poll(Link *link,uint8_t packet[LINK_PACKET_MAX]){
    if(link->self||!link->count||link->phase)return 0;
    link->poll_id++;
    link->polled=1;link->controls[0]=link->local_input;
    if(link->joined)link->ready|=1;else link->ready&=~1;
    return encode(link,LINK_POLL_PACKET,packet);
}
uint8_t link_render(const Link *link,uint8_t age,uint8_t packet[LINK_PACKET_MAX]){
    if(link->self||link->count<2||link->phase)return 0;
    if(!link->running&&link->polled!=members(link))return 0;
    if(link->running&&!link->have_last)return 0;
    link_frame_header(link,LINK_RENDER_PACKET,link->poll_id,packet);
    packet[9]=link->ready|(link->running<<4);put16(packet+10,link->last);packet[12]=age;
    put16(packet+13,link_crc16(packet,13));return 15;
}
#ifndef NN_MINIMAL
static uint8_t link_observe(Link *link,uint8_t peer,uint16_t sequence,uint8_t held,uint8_t joined){
    if(link->self||link->phase||!peer||peer>=link->count||!link->polled||sequence!=link->poll_id)return 0;
    uint8_t bit=1<<peer;
    if(link->polled&bit)return link->controls[peer]==(held&63)&&((link->ready>>peer)&1)==joined;
    link->controls[peer]=held&63;link->polled|=bit;
    if(joined)link->ready|=1<<peer;else link->ready&=~(1<<peer);
    if(link->polled==members(link))link->fault=0;
    return 1;
}
#endif
uint8_t link_reply(const Link *link,uint8_t packet[LINK_PACKET_MAX]){
    if(!link->self||!link->polled||link->phase)return 0;
    return encode(link,LINK_REPLY_PACKET,packet);
}
uint8_t link_propose(Link *link,uint16_t seed,uint8_t steps,uint8_t packet[LINK_PACKET_MAX]){
    if(link->self||link->phase||link->count<2||link->polled!=members(link))return 0;
    if(!link->running&&link->ready!=members(link))return 0;
    if(link->running&&(!steps||steps>3))return 0;
    link->sequence=link->have_last?(uint16_t)(link->last+1):0;
    link->kind=link->running?LINK_INPUT_PACKET:LINK_START_PACKET;
    if(!link->running)link->seed=seed?seed:1;
    link->steps=link->running?steps:0;
    link->prepared=1;link->phase=LINK_PREPARE;link->acks=1;link->polled=0;
    return encode(link,link->kind,packet);
}
uint8_t link_retry(const Link *link,uint8_t packet[LINK_PACKET_MAX]){
    if(link->self)return 0;
    if(!link->phase)return link->polled?encode(link,LINK_POLL_PACKET,packet):0;
    return encode(link,link->phase==LINK_PREPARE?link->kind:LINK_COMMIT_PACKET,packet);
}
uint8_t link_ack_packet(const Link *link,uint8_t phase,uint16_t sequence,uint8_t packet[LINK_PACKET_MAX]){
    if(!link->self||link->self>=link->count)return 0;
    if(phase==LINK_PREPARE){
        if(!(link->prepared&&link->sequence==sequence)&&!(link->have_last&&link->last==sequence))return 0;
    }else if(phase==LINK_COMMIT){
        if(!link->have_last||link->last!=sequence)return 0;
    }else return 0;
    link_frame_header(link,LINK_ACK_PACKET,sequence,packet);packet[9]=link->self;packet[10]=phase;
    put16(packet+11,link_crc16(packet,11));return 13;
}
static uint8_t same_payload(const Link *link,const uint8_t *packet){
    if(packet[0]!=link->kind)return 0;
    if(link->kind==LINK_START_PACKET)return get16(packet+9)==link->seed;
    for(uint8_t i=0;i<4;i++)if(packet[9+i]!=link->controls[i])return 0;
    return packet[13]==link->steps;
}
static uint8_t receive_ack(Link *link,uint8_t peer,uint8_t phase,uint16_t sequence){
    if(link->self||!peer||peer>=link->count||sequence!=link->sequence||phase!=link->phase||!phase)return 0;
    if(phase==LINK_COMMIT&&(!link->have_last||link->last!=sequence))return 0;
    link->acks|=1<<peer;
    if(link->acks==members(link)){link->acks=0;link->phase=phase==LINK_PREPARE?LINK_COMMIT:LINK_IDLE;}
    return LINK_ACK;
}
uint8_t link_frame_valid(const Link *link,const uint8_t *packet,uint8_t size){
    if(!link->count||size<11||size!=link_packet_length(packet[0]))return 0;
    if(link_crc16(packet,size-2)!=get16(packet+size-2)||get16(packet+7)!=link->roster)return 0;
    for(uint8_t i=0;i<4;i++)if(packet[3+i]!=(uint8_t)(link->session>>(i*8)))return 0;
    return 1;
}
uint8_t link_control_receive(Link *link,const uint8_t *packet){
    uint16_t sequence=get16(packet+1);uint8_t kind=packet[0];
    if(kind==LINK_RENDER_PACKET){
        if(!link->self||link->phase||!link->polled||sequence!=link->poll_id||
           (packet[9]>>4)!=link->running||(packet[9]&~(members(link)|16))||get16(packet+10)!=link->last)return 0;
        return LINK_RENDER;
    }
#ifndef NN_MINIMAL
    if(kind==LINK_POLL_PACKET){
        if(!link->self||link->phase||packet[10]!=link->running||(packet[9]&~members(link)))return 0;
        if(link->running&&get16(packet+11)!=link->last)return 0;
        if(link->polled&&sequence!=link->poll_id&&(uint16_t)(sequence-link->poll_id)>=32768)return 0;
        if(!link->polled||sequence!=link->poll_id){
            link->reply_input=link->local_input;link->reply_joined=link->joined;
            link->poll_id=sequence;link->polled=1;
        }
        link->ready=packet[9];link->fault=0;return LINK_POLL;
    }
    if(kind==LINK_REPLY_PACKET){
        if((packet[10]&~63)||packet[11]>1)return 0;
        return link_observe(link,packet[9],sequence,packet[10],packet[11])?LINK_REPLY:0;
    }
#endif
    return 0;
}
uint8_t link_receive(Link *link,const uint8_t *packet,uint8_t size){
    if(!link_frame_valid(link,packet,size)||packet[0]>LINK_RENDER_PACKET)return 0;
    uint16_t sequence=get16(packet+1);uint8_t kind=packet[0];
    if(kind==LINK_ACK_PACKET)return receive_ack(link,packet[9],packet[10],sequence);
    if(kind==LINK_POLL_PACKET||kind==LINK_REPLY_PACKET||kind==LINK_RENDER_PACKET)return link_control_receive(link,packet);
    if(kind==LINK_START_PACKET||kind==LINK_INPUT_PACKET){
        if(!link->self)return 0;
        if(link->have_last&&sequence==link->last)return LINK_PREPARE;
        uint16_t expected=link->have_last?(uint16_t)(link->last+1):0;
        if(sequence!=expected)return 0;
        if(link->prepared)return same_payload(link,packet)?LINK_PREPARE:0;
        if(kind==LINK_START_PACKET){
            if(link->running||!link->joined||link->count<2||!get16(packet+9))return 0;
            link->seed=get16(packet+9);link->steps=0;
        }else{
            if(!link->running||!packet[13]||packet[13]>3)return 0;
            for(uint8_t i=0;i<4;i++)if((packet[9+i]&~63)||(i>=link->count&&packet[9+i]))return 0;
            for(uint8_t i=0;i<4;i++)link->controls[i]=packet[9+i];
            link->steps=packet[13];
        }
        link->sequence=sequence;link->kind=kind;link->prepared=1;link->phase=LINK_PREPARE;return LINK_PREPARE;
    }
    if(!link->self&&link->phase!=LINK_COMMIT)return 0;
    if(link->have_last&&sequence==link->last){
        link->fault=0;
        if(!link->self&&link->phase==LINK_COMMIT)link->acks|=1;
        return LINK_COMMIT;
    }
    if(!link->prepared||sequence!=link->sequence)return 0;
    uint8_t event=LINK_APPLIED|LINK_COMMIT;
    if(link->kind==LINK_START_PACKET){link->running=1;event|=LINK_START_APPLIED;}
    link->last=sequence;link->have_last=1;link->prepared=0;link->fault=0;
    if(link->self)link->phase=LINK_IDLE;else link->acks|=1;
    return event;
}
void link_timeout(Link *link){link->fault=1;}
