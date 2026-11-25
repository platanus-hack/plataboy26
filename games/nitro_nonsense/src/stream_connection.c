#include "stream_connection.h"
#include "connection_internal.h"
#include "stream_session.h"
#ifdef NN_OVERLAYS
#include "load_pause.h"
#endif
#ifdef NN_MINIMAL
#define BOOT_PACKET LINK_STREAM_START_PACKET
#define BOOT_EVENT LINK_START_APPLIED
#else
#define BOOT_PACKET LINK_POLL_PACKET
#define BOOT_EVENT LINK_POLL
#endif
#ifndef NN_MINIMAL
static uint8_t members(const Link *link){return (1U<<link->count)-1;}
#endif
static uint8_t data_kind(uint8_t kind){return kind==LINK_STREAM_START_PACKET||kind==LINK_STREAM_INPUT_PACKET;}
static uint32_t lead(uint8_t kind){return data_kind(kind)?APPLY_GUARD:TURNAROUND;}
static uint16_t render_key(const Link *link){return link->running?link->last:link->poll_id;}
static uint8_t runtime_step(Connection *c,Wire *w,Stream *stream,uint32_t now,uint8_t high){
    Link *link=&stream->link;
#ifdef NN_OVERLAYS
    if(load_pause_wait(c,w,stream,now,high))return 0;
#endif
    uint8_t event=wire_step(w,now,high),result=0;
    if(event==WIRE_RECEIVED){
        if(!c->active&&w->data[0]!=BOOT_PACKET){wire_listen(w,now);return 0;}
        uint8_t kind=w->data[0],logical=session_stream_receive(stream,w->data,w->length);
        if(!c->active){
            if(!(logical&BOOT_EVENT)){wire_listen(w,now);return 0;}
            c->active=1;
        }
        result=logical&(CONNECTION_APPLIED|CONNECTION_STARTED);
        if(logical&LINK_START_APPLIED)c->render_flags&=~RENDER_SEEN;
        if((logical&LINK_POLL)||(data_kind(kind)&&(logical&STREAM_ACK_NEEDED)))connection_context(c,now);
        if(logical&LINK_RENDER)return connection_render_receive(c,link,w,now,render_key(link));
        if(link->self&&logical){
            uint8_t size=0;
#ifndef NN_MINIMAL
            if(logical&LINK_POLL)size=link_reply(link,c->packet);
            else
#endif
            if(logical&STREAM_ACK_NEEDED)size=stream_ack(stream,c->packet);
            if(size){
                c->size=size;c->send_after=now+(lead(kind)+(link->self-1)*REPLY_SLOT)*(uint32_t)w->period;
                c->deadline=c->send_after+(uint32_t)w->period*REPLY_SLOT;
            }
        }else if(!link->self&&logical&&
                 (
#ifndef NN_MINIMAL
                  ((logical&LINK_REPLY)&&link->polled==members(link))||
#endif
                  ((logical&LINK_ACK)&&link->phase==LINK_IDLE))){
            // A late final ACK also cancels an unsent retry of the same frame.
            c->waiting=0;c->size=0;
        }
        wire_listen(w,now);return result;
    }
    if(event==WIRE_SENT){
        c->size=0;wire_listen(w,now);c->send_after=now+(uint32_t)w->period*TURNAROUND;
        if(!link->self){
            uint8_t kind=c->packet[0];
            if(kind==LINK_RENDER_PACKET)return connection_render_window(c,link,w,now,render_key(link));
            connection_context(c,now);c->waiting=1;
            c->deadline=now+(lead(kind)+link->count*REPLY_SLOT)*(uint32_t)w->period;
            if(data_kind(kind)){
                uint8_t logical=session_stream_sent(stream,c->packet,link_packet_length(kind));
                result=logical&(CONNECTION_APPLIED|CONNECTION_STARTED);
                if(logical&LINK_START_APPLIED){c->step_at=now;c->render_flags&=~RENDER_SEEN;}
                else if(logical&LINK_APPLIED)c->step_at+=(uint32_t)w->period*TICK_PERIODS*link->steps;
            }
        }
        return result;
    }
    if(event==WIRE_ERROR){
#ifdef NN_OVERLAYS
        if(!high)c->render_flags|=LOAD_PAUSED;
#endif
        wire_listen(w,now);connection_fault(c,link,now);c->size=0;
        if(!link->self){
            c->waiting=0;c->send_after=now+(uint32_t)w->period*(APPLY_GUARD+link->count*REPLY_SLOT);
            if(c->packet[0]==LINK_RENDER_PACKET)connection_render_hold(c,w,now,render_key(link));
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
    if((int32_t)(now-c->send_after)<0||!wire_quiet(w,now,TURNAROUND))return 0;
    if(!c->size){
        if(link->phase
#ifndef NN_MINIMAL
           ||(!link->running&&link->polled&&link->polled!=members(link))
#endif
        ){
            // The proposal and cached ACK inputs stay immutable across retries.
            c->size=link_packet_length(c->packet[0]);return 0;
        }
        uint16_t key=render_key(link);
        if((c->render_flags&RENDER_ENABLED)&&
           (!(c->render_flags&RENDER_SEEN)||c->render_poll!=key)&&
           (data_kind(c->packet[0])
#ifndef NN_MINIMAL
            ||(c->packet[0]==LINK_POLL_PACKET&&link->polled==members(link)&&link->ready!=members(link))
#endif
           )){
            connection_render_prepare(c,link,w,now,key);return 0;
        }
#ifndef NN_MINIMAL
        if(link->running||(link->polled==members(link)&&link->ready==members(link)))
#endif
        {
            int32_t elapsed=(int32_t)(now-c->step_at);
            uint32_t ticks=elapsed>0?(uint32_t)elapsed/((uint32_t)w->period*TICK_PERIODS):0;
            if(link->running&&!ticks)return 0;
            c->size=stream_propose(stream,c->seed,ticks>3?3:ticks,c->packet);
        }
#ifndef NN_MINIMAL
        else c->size=link_poll(link,c->packet);
#endif
        return 0;
    }
    if(wire_send(w,c->packet,c->size,now))c->size=0;
    return 0;
}

__attribute__((noinline)) uint8_t connection_stream_step(Connection *c,Wire *w,Discovery *catalog,Stream *stream,uint32_t now,uint8_t high){
    Link *link=&stream->link;
    if(!connection_resume(c,w,now))return 0;
    if(!c->active){
        uint8_t discovery=discovery_step(catalog,w,now,high);
        if(!(discovery&(DISCOVERY_READY|DISCOVERY_DATA))||!discovery_bind(catalog,link))return 0;
        if(!catalog->host&&!(discovery&DISCOVERY_DATA))return 0;
        if(catalog->host)c->active=1;
    }
    return runtime_step(c,w,stream,now,high);
}

// The catalog owner must bind Link before entering this service. Clients still
// wait for the coordinator's first valid POLL before activating the runtime.
__attribute__((noinline)) uint8_t connection_stream_service(Connection *c,Wire *w,Stream *stream,uint32_t now,uint8_t high){
    Link *link=&stream->link;
    if(!link->session||link->count<2||link->count>LINK_MAX_PEERS||link->self>=link->count||
       !connection_resume(c,w,now))return 0;
    if(!link->self)c->active=1;
    return runtime_step(c,w,stream,now,high);
}
