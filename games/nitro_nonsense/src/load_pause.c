#include "load_pause.h"
#include "connection_internal.h"
void load_pause_begin(Connection *c,Wire *w){c->render_flags|=LOAD_PAUSED;w->low=1;}
void load_pause_end(Connection *c,Wire *w,uint8_t result,uint32_t now){if(result){c->render_flags|=LOAD_PAUSED|LOAD_FAILED;c->size=c->waiting=0;}wire_listen(w,now);}
uint8_t load_pause_wait(Connection *c,Wire *w,Stream *s,uint32_t now,uint8_t high){
 if(!(c->render_flags&(LOAD_PAUSED|LOAD_FAILED)))return 0;
 uint8_t event=wire_step(w,now,high);
 if(event){wire_listen(w,now);return 1;}
 if(c->render_flags&LOAD_FAILED)return 1;
 if(!wire_quiet(w,now,TURNAROUND))return 1;
 c->render_flags&=~LOAD_PAUSED;c->step_at=now;
 uint32_t at=w->edge+(uint32_t)w->period*TURNAROUND;
 Link *link=&s->link;
 if(link->self){
  c->size=stream_ack(s,c->packet);
  c->send_after=at+(APPLY_GUARD+(link->self-1)*REPLY_SLOT)*(uint32_t)w->period;
  c->deadline=c->send_after+(uint32_t)w->period*REPLY_SLOT;
 }else{
  c->size=0;c->waiting=link->phase&&!link->prepared;
  c->send_after=at+(uint32_t)w->period*TURNAROUND;
  c->deadline=at+(APPLY_GUARD+link->count*REPLY_SLOT)*(uint32_t)w->period;
 }
 return 0;
}
