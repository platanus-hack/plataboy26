#include "load_gate.h"
#include "overlay.h"
#include "link_frame.h"
#include "connection_internal.h"

enum { OFF, HOST_ID, HOST_GRANT, HOST_POLL, HOST_RESUME, HOST_GUARD,
       HOST_LOADING, CLIENT_OPEN, CLIENT_READY, CLIENT_GUARD, CLIENT_LOADING,
       CLIENT_LOADED, CLIENT_RESUMED, FAILED };
enum { PREPARED=1, LOADED=2, RESUMED=4, ERROR=128, FRAME_SIZE=16,
       REPLY_WAIT=400, LOAD_GUARD=4 };

static NN_OV2_LOCAL uint16_t sequence(const Link *link) {
    return link->have_last?link->last:0;
}
static NN_OV2_LOCAL uint8_t gate_kind(uint8_t kind) {
    return kind>=LOAD_GATE_ID_PACKET&&kind<=LOAD_GATE_REPLY_PACKET;
}
static NN_OV2_LOCAL uint8_t valid(const LoadGate *g,const Link *link,const uint8_t *p,uint8_t size) {
    if(size!=FRAME_SIZE||!gate_kind(p[0])||get16(p+1)!=sequence(link)||
       get16(p+7)!=link->roster||((p[9]>>2)&3)+1!=g->group||
       (p[9]&3)>=link->count||get16(p+14)!=link_crc16(p,14))return 0;
    for(uint8_t i=0;i<4;i++)if(p[3+i]!=(uint8_t)(link->session>>(i*8)))return 0;
    if(p[0]==LOAD_GATE_ID_PACKET)return !(p[9]&128);
    if(p[9]&240)return 0;
    return p[0]==LOAD_GATE_REPLY_PACKET||!(p[10]|p[11]|p[12]|p[13]);
}
static NN_OV2_LOCAL void header(const LoadGate *g,const Link *link,uint8_t kind,uint8_t peer,uint8_t *p) {
    link_frame_header(link,kind,sequence(link),p);
    p[9]=((g->group-1)<<2)|peer;
    for(uint8_t i=10;i<14;i++)p[i]=0;
}
static NN_OV2_LOCAL void seal(Connection *c,uint32_t now,const Wire *w) {
    put16(c->packet+14,link_crc16(c->packet,14));
    c->size=FRAME_SIZE;c->send_after=now+(uint32_t)w->period*TURNAROUND;
}
static NN_OV2_LOCAL uint8_t flags(const LoadGate *g) {
    return (g->fragments==255?PREPARED:0)|
           ((g->state==CLIENT_LOADED||g->state==CLIENT_RESUMED)?LOADED:0)|
           (g->state==CLIENT_RESUMED?RESUMED:0)|(g->error?ERROR:0);
}
static NN_OV2_LOCAL void reply(LoadGate *g,Connection *c,const Link *link,Wire *w,uint32_t now) {
    header(g,link,LOAD_GATE_REPLY_PACKET,link->self,c->packet);
    c->packet[10]=flags(g);c->packet[11]=g->fragment;c->packet[12]=g->error;
    seal(c,now,w);
}
static NN_OV2_LOCAL void resumed(LoadGate *g,Connection *c,uint32_t now) {
    g->state=OFF;c->size=c->waiting=0;c->step_at=now;
    c->send_after=now;c->render_flags&=~RENDER_PAUSED;
}
NN_OV2 uint8_t load_gate_begin(LoadGate *g,Connection *c,Wire *w,const Link *link,
                        uint8_t group,uint32_t now) {
    if(g->state||group<1||group>4||!link->session||link->count<2||
       link->count>LINK_MAX_PEERS||link->self>=link->count)return 0;
    if(!link->self&&(link->phase||c->waiting||c->size||wire_busy(w)||
       (link->running&&link->acks!=(1U<<link->count)-1)))return 0;
    g->group=group;g->peer=1;g->fragment=g->fragments=g->error=0;
    g->state=link->self?CLIENT_OPEN:HOST_ID;
    if(!link->self){c->size=c->waiting=0;c->send_after=now;}
    return 1;
}
NN_OV2 void load_gate_complete(LoadGate *g,Connection *c,Wire *w,uint8_t result,uint32_t now) {
    if(g->state!=HOST_LOADING&&g->state!=CLIENT_LOADING)return;
    g->error=result;
    g->state=g->state==HOST_LOADING?(result?FAILED:HOST_GRANT):CLIENT_LOADED;
    g->peer=1;c->size=c->waiting=0;
    wire_listen(w,now);c->send_after=now+(uint32_t)w->period*TURNAROUND;
}
NN_OV2 uint8_t load_gate_step(LoadGate *g,Connection *c,Wire *w,const Link *link,
                       const uint8_t build_id[32],uint32_t now,uint8_t high) {
    if(!g->state)return LOAD_GATE_NORMAL;
    if(g->state==HOST_LOADING||g->state==CLIENT_LOADING)return 0;
    uint8_t event=wire_step(w,now,high);
    if(event==WIRE_RECEIVED){
        const uint8_t *p=w->data;
        if(!gate_kind(p[0])){
            if(link->self&&g->state==CLIENT_OPEN&&!g->fragments&&!g->error)return LOAD_GATE_NORMAL;
            if(link->self&&g->state==CLIENT_RESUMED&&
               (p[0]==LINK_STREAM_START_PACKET||p[0]==LINK_STREAM_INPUT_PACKET)&&
               link_frame_valid(link,p,w->length)&&
               get16(p+1)==(link->have_last?(uint16_t)(link->last+1):0)){
                resumed(g,c,now);return LOAD_GATE_RESUMED;
            }
            wire_listen(w,now);return 0;
        }
        if(!valid(g,link,p,w->length)){wire_listen(w,now);return 0;}
        uint8_t peer=p[9]&3;
        if(link->self&&peer==link->self){
            if(p[0]==LOAD_GATE_ID_PACKET){
                g->fragment=p[9]>>4;
                for(uint8_t i=0;i<4;i++)if(p[10+i]!=build_id[g->fragment*4+i])g->error=1;
                if(!g->error)g->fragments|=1U<<g->fragment;
                if(g->state==CLIENT_OPEN&&g->fragments==255)g->state=CLIENT_READY;
                reply(g,c,link,w,now);
            }else if(p[0]==LOAD_GATE_GRANT_PACKET&&g->state==CLIENT_READY&&!g->error){
                g->state=CLIENT_GUARD;c->size=0;
                c->send_after=now+(uint32_t)w->period*LOAD_GUARD;
            }else if(p[0]==LOAD_GATE_POLL_PACKET&&g->fragments==255)reply(g,c,link,w,now);
            else if(p[0]==LOAD_GATE_RESUME_PACKET&&!g->error&&
                    (g->state==CLIENT_LOADED||g->state==CLIENT_RESUMED)){
                g->state=CLIENT_RESUMED;reply(g,c,link,w,now);
            }
        }else if(!link->self&&p[0]==LOAD_GATE_REPLY_PACKET&&peer==g->peer){
            uint8_t state=p[10];
            if((state&~(PREPARED|LOADED|RESUMED|ERROR))||p[13]||p[11]>=8||
               ((state&RESUMED)&&((state&(PREPARED|LOADED))!=(PREPARED|LOADED)))||
               ((state&LOADED)&&!(state&PREPARED))||(!(state&ERROR)&&p[12])){
                wire_listen(w,now);return 0;
            }
            if(state&ERROR){g->error=p[12]?p[12]:1;g->state=FAILED;c->size=c->waiting=0;}
            else if(g->state==HOST_ID&&p[11]==g->fragment){
                c->size=c->waiting=0;
                if(++g->fragment==8){
                    if(!(state&PREPARED)){g->fragment=7;}
                    else{g->fragment=0;if(++g->peer==link->count){
                        g->state=HOST_GUARD;
                        c->send_after=now+(uint32_t)w->period*LOAD_GUARD;
                    }}
                }
            }else if(g->state==HOST_POLL){
                c->size=c->waiting=0;
                if(!(state&LOADED))g->state=HOST_GRANT;
                else if(++g->peer==link->count){g->peer=1;g->state=HOST_RESUME;}
                else g->state=HOST_GRANT;
            }else if(g->state==HOST_RESUME&&(state&RESUMED)){
                c->size=c->waiting=0;
                if(++g->peer==link->count){
                    wire_listen(w,now);resumed(g,c,now);return LOAD_GATE_RESUMED;
                }
            }
        }
        wire_listen(w,now);return g->state==FAILED?LOAD_GATE_FAILED:0;
    }
    if(event==WIRE_SENT){
        if(link->self&&g->state==CLIENT_OPEN&&!g->fragments&&!g->error&&!gate_kind(c->packet[0]))return LOAD_GATE_NORMAL;
        c->size=0;wire_listen(w,now);
        c->send_after=now+(uint32_t)w->period*TURNAROUND;
        if(!link->self){
            if(c->packet[0]==LOAD_GATE_GRANT_PACKET)g->state=HOST_POLL;
            else{c->waiting=1;c->deadline=now+(uint32_t)w->period*REPLY_WAIT;}
        }
        return 0;
    }
    if(event==WIRE_ERROR){
        if(link->self&&g->state==CLIENT_OPEN&&!g->fragments&&!g->error)return LOAD_GATE_NORMAL;
        wire_listen(w,now);c->size=0;
        if(!link->self){c->waiting=0;c->send_after=now+(uint32_t)w->period*TURNAROUND;}
        return 0;
    }
    if(g->state==HOST_GUARD||g->state==CLIENT_GUARD){
        if((int32_t)(now-c->send_after)>=0&&wire_quiet(w,now,2)){
            g->state=g->state==HOST_GUARD?HOST_LOADING:CLIENT_LOADING;
            c->size=c->waiting=0;w->low=1;return LOAD_GATE_LOAD;
        }
        return 0;
    }
    if(g->state==FAILED)return LOAD_GATE_FAILED;
    if(link->self){
        if(g->state==CLIENT_OPEN&&!g->fragments&&!g->error)return LOAD_GATE_NORMAL;
        if(c->size&&(int32_t)(now-c->send_after)>=0&&wire_quiet(w,now,TURNAROUND))
            if(wire_send(w,c->packet,c->size,now))c->size=0;
        return 0;
    }
    if(c->waiting){if((int32_t)(now-c->deadline)<0)return 0;c->waiting=0;}
    if((int32_t)(now-c->send_after)<0||!wire_quiet(w,now,TURNAROUND))return 0;
    if(!c->size){
        uint8_t kind=g->state==HOST_ID?LOAD_GATE_ID_PACKET:g->state==HOST_GRANT?
                     LOAD_GATE_GRANT_PACKET:g->state==HOST_POLL?LOAD_GATE_POLL_PACKET:LOAD_GATE_RESUME_PACKET;
        header(g,link,kind,g->peer,c->packet);
        if(kind==LOAD_GATE_ID_PACKET){
            c->packet[9]|=g->fragment<<4;
            for(uint8_t i=0;i<4;i++)c->packet[10+i]=build_id[g->fragment*4+i];
        }
        seal(c,now,w);return 0;
    }
    if(wire_send(w,c->packet,c->size,now))c->size=0;
    return 0;
}
