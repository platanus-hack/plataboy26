#include "overlay.h"
#include "connection_internal.h"
#include "session.h"
static uint8_t members(const Link *link){return (1U<<link->count)-1;}
static uint32_t lead(uint8_t kind){return kind==LINK_COMMIT_PACKET?APPLY_GUARD:TURNAROUND;}
static uint8_t applied(uint8_t event){
    return event&(CONNECTION_APPLIED|CONNECTION_STARTED);
}
NN_OV4 void connection_init(Connection *c,uint16_t seed,uint32_t now){
    uint8_t *bytes=(uint8_t *)c;for(uint8_t i=0;i<sizeof(*c);i++)bytes[i]=0;
    c->seed=seed?seed:1;c->send_after=c->step_at=now;
}
NN_OV4 void connection_enable_render(Connection *c){c->render_flags|=RENDER_ENABLED;}
uint8_t connection_render_window(Connection *c,const Link *link,const Wire *w,uint32_t now,uint16_t key){
    if(!(c->render_flags&RENDER_ENABLED)||
       ((c->render_flags&RENDER_SEEN)&&c->render_poll==key))return 0;
    c->render_flags|=RENDER_SEEN|RENDER_PAUSED;c->render_poll=key;
    c->render_until=now+(uint32_t)w->period*(link->self?RENDER_WORK:RENDER_HOLD);
    c->send_after=c->render_until;return CONNECTION_RENDER;
}
void connection_fault(Connection *c,Link *link,uint32_t now){
    link_timeout(link);c->step_at=now;
}
static uint8_t completed(const Connection *c,const Link *link){
    if(c->packet[0]==LINK_POLL_PACKET)return link->polled==members(link);
    if(c->packet[0]==LINK_COMMIT_PACKET)return link->phase==LINK_IDLE;
    return link->phase==LINK_COMMIT;
}
uint8_t connection_step(Connection *c,Wire *w,Discovery *catalog,Link *link,uint32_t now,uint8_t high){
    if(!connection_resume(c,w,now))return 0;
    if(!c->active){
        uint8_t discovery=discovery_step(catalog,w,now,high);
        if(!(discovery&(DISCOVERY_READY|DISCOVERY_DATA))||!discovery_bind(catalog,link))return 0;
        if(!catalog->host&&!(discovery&DISCOVERY_DATA))return 0;
        if(catalog->host)c->active=1;
    }
    uint8_t event=wire_step(w,now,high),result=0;
    if(event==WIRE_RECEIVED){
        if(!c->active&&w->data[0]!=LINK_POLL_PACKET){wire_listen(w,now);return 0;}
        uint8_t kind=w->data[0],logical=session_receive(link,w->data,w->length);
        if(!c->active){
            if(!(logical&LINK_POLL)){wire_listen(w,now);return 0;}
            c->active=1;
        }
        result=applied(logical);
        if(logical&(LINK_POLL|LINK_COMMIT))connection_context(c,now);
        if(logical&LINK_RENDER)return connection_render_receive(c,link,w,now,link->poll_id);
        if(link->self&&logical){
            uint8_t size=0;
            if(logical&LINK_POLL)size=link_reply(link,c->packet);
            else if(logical&(LINK_PREPARE|LINK_COMMIT)){
                uint8_t phase=(logical&LINK_COMMIT)?LINK_COMMIT:LINK_PREPARE;
                uint16_t sequence=(uint16_t)w->data[1]|((uint16_t)w->data[2]<<8);
                size=link_ack_packet(link,phase,sequence,c->packet);
            }
            if(size){
                c->size=size;c->send_after=now+(lead(kind)+(link->self-1)*REPLY_SLOT)*(uint32_t)w->period;
                c->deadline=c->send_after+(uint32_t)w->period*REPLY_SLOT;
            }
        }else if(!link->self&&logical&&c->waiting&&completed(c,link))c->waiting=0;
        wire_listen(w,now);return result;
    }
    if(event==WIRE_SENT){
        c->size=0;wire_listen(w,now);
        c->send_after=now+(uint32_t)w->period*TURNAROUND;
        if(!link->self){
            if(c->packet[0]==LINK_RENDER_PACKET)return connection_render_window(c,link,w,now,link->poll_id);
            if(c->packet[0]==LINK_POLL_PACKET||c->packet[0]==LINK_COMMIT_PACKET){
                connection_context(c,now);
            }
            c->waiting=1;
            c->deadline=now+(lead(c->packet[0])+link->count*REPLY_SLOT)*(uint32_t)w->period;
            if(c->packet[0]==LINK_COMMIT_PACKET){
                uint8_t logical=session_receive(link,c->packet,link_packet_length(c->packet[0]));
                result=applied(logical);
                if(logical&LINK_START_APPLIED)c->step_at=now;
                else if(logical&LINK_APPLIED)c->step_at+=(uint32_t)w->period*TICK_PERIODS*link->steps;
            }
        }
        return result;
    }
    if(event==WIRE_ERROR){
        wire_listen(w,now);connection_fault(c,link,now);c->size=0;
        if(!link->self){
            c->waiting=0;c->send_after=now+(uint32_t)w->period*(APPLY_GUARD+link->count*REPLY_SLOT);
            if(c->packet[0]==LINK_RENDER_PACKET){
                connection_render_hold(c,w,now,link->poll_id);
            }
        }
        return 0;
    }
    if(link->self){
        if(c->size&&(int32_t)(now-c->send_after)>=0){
            if((int32_t)(now-c->deadline)>=0){c->size=0;connection_fault(c,link,now);}
            else if(wire_send(w,c->packet,c->size,now))c->size=0;
        }
        return 0;
    }
    if(c->waiting){
        if((int32_t)(now-c->deadline)<0)return 0;
        connection_fault(c,link,now);c->waiting=0;
    }
    if((int32_t)(now-c->send_after)<0)return 0;
    if(!wire_quiet(w,now,TURNAROUND))return 0;
    if(!c->size){
        if((c->render_flags&RENDER_ENABLED)&&link->phase==LINK_IDLE&&
           (!(c->render_flags&RENDER_SEEN)||c->render_poll!=link->poll_id)&&
           (c->packet[0]==LINK_COMMIT_PACKET||
            (c->packet[0]==LINK_POLL_PACKET&&link->polled==members(link)&&!link->running&&link->ready!=members(link)))){
            connection_render_prepare(c,link,w,now,link->poll_id);
            return 0;
        }
        if(link->phase||((link->polled!=members(link))&&link->polled))c->size=link_retry(link,c->packet);
        else if(link->polled==members(link)&&(link->running||link->ready==members(link))){
            int32_t elapsed=(int32_t)(now-c->step_at);
            uint32_t ticks=elapsed>0?(uint32_t)elapsed/((uint32_t)w->period*TICK_PERIODS):0;
            if(link->running&&!ticks)return 0;
            c->size=link_propose(link,c->seed,ticks>3?3:ticks,c->packet);
        }else c->size=link_poll(link,c->packet);
        // CRC/packet preparation must precede a later fresh physical timestamp.
        return 0;
    }
    if(wire_send(w,c->packet,c->size,now))c->size=0;
    return 0;
}

uint8_t connection_resume(Connection *c,Wire *w,uint32_t now){
    if((c->render_flags&RENDER_CONTEXT)&&
       (uint32_t)(now-c->render_context)>(uint32_t)w->period*(255*RENDER_AGE_UNIT+RENDER_AGE_MARGIN))
        c->render_flags&=~RENDER_CONTEXT;
    if(c->render_flags&RENDER_PAUSED){
        if((int32_t)(now-c->render_until)<0)return 0;
        c->render_flags&=~RENDER_PAUSED;wire_listen(w,now);
    }
    return 1;
}
void connection_context(Connection *c,uint32_t now){c->render_context=now;c->render_flags|=RENDER_CONTEXT;}
uint8_t connection_render_receive(Connection *c,Link *link,Wire *w,uint32_t now,uint16_t key){
    wire_listen(w,now);
    uint32_t age=(uint32_t)(now-c->render_context)/w->period;
    uint32_t expected=(uint32_t)w->data[12]*RENDER_AGE_UNIT;
    if(!(c->render_flags&RENDER_CONTEXT)||age>expected+RENDER_AGE_MARGIN||age+RENDER_AGE_MARGIN<expected)return 0;
    link->ready=w->data[9]&15;return connection_render_window(c,link,w,now,key);
}
void connection_render_prepare(Connection *c,const Link *link,const Wire *w,uint32_t now,uint16_t key){
    uint32_t age=((uint32_t)(now-c->render_context)/w->period+WIRE_FRAME_PERIODS(15))/RENDER_AGE_UNIT;
    if((c->render_flags&RENDER_CONTEXT)&&age<=255)c->size=link_render(link,age,c->packet);
    else{c->render_poll=key;c->render_flags|=RENDER_SEEN;}
}
void connection_render_hold(Connection *c,const Wire *w,uint32_t now,uint16_t key){
    // A receiver may have accepted A6 before its sender's terminal timing error.
    c->send_after=now+(uint32_t)w->period*RENDER_HOLD;
    c->render_poll=key;c->render_flags|=RENDER_SEEN;
}
