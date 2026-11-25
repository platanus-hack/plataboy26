#include "discovery.h"
_Static_assert(LINK_MAX_PEERS==4&&WIRE_ID_BYTES==12,"Catalog packet layout requires four twelve-byte identities");
enum {D_INITIAL,D_OPEN,D_LOW,D_HIGH,D_POLL,D_WAIT_REPLY,D_SCAN,D_SCAN_WAIT,D_RESULT,D_REPLY,D_LISTEN,D_SETTLE};
enum {OPEN=0xb0,UID_LOW,UID_HIGH,POLL,REPLY,RESULT};
enum {TABLE=1,COMMIT=2};
static void copy_bytes(uint8_t *out,const uint8_t *in,uint8_t size){for(uint8_t i=0;i<size;i++)out[i]=in[i];}
static uint8_t different(const uint8_t *a,const uint8_t *b,uint8_t size){for(uint8_t i=0;i<size;i++)if(a[i]!=b[i])return 1;return 0;}
static uint16_t get16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t get32(const uint8_t *p){uint32_t n=0;for(uint8_t i=0;i<4;i++)n|=(uint32_t)p[i]<<(i*8);return n;}
static void put16(uint8_t *p,uint16_t n){p[0]=n;p[1]=n>>8;}
static void put32(uint8_t *p,uint32_t n){for(uint8_t i=0;i<4;i++)p[i]=n>>(i*8);}
static uint8_t all_parts(const Discovery *d){return (1U<<(d->count*2))-1;}
static uint8_t own_slot(const Discovery *d){
    for(uint8_t i=0;i<d->count;i++)if(!different(d->identity,d->ids[i],WIRE_ID_BYTES))return i;
    return 255;
}
static uint16_t catalog_tag(const Discovery *d){return link_crc16((const uint8_t *)&d->ids,d->count*WIRE_ID_BYTES)^d->catalog_rules;}
static uint8_t status(const Discovery *d){return d->ready&&d->phase==D_LISTEN&&!d->tx_size?DISCOVERY_READY:d->error?DISCOVERY_FAILED:0;}
static void listen(Discovery *d,Wire *w,uint32_t now){
    wire_listen(w,now);
    // A committed catalog belongs to the session until an explicit init/reset.
    if(d->committed!=2)wire_scan(w,d->identity,d->self==255,0,now);
}
static void prepare(Discovery *d){
    uint8_t *p=d->tx,n=0,kind=0,index=0;
    if(d->phase==D_OPEN){kind=OPEN;index=d->count;put16(p+6,d->catalog_rules);n=10;}
    else if(d->phase==D_LOW||d->phase==D_HIGH){
        kind=d->phase==D_LOW?UID_LOW:UID_HIGH;index=d->cursor;
        copy_bytes(p+6,d->ids[index]+(kind==UID_LOW?0:6),6);n=14;
    }else if(d->phase==D_POLL){kind=POLL;index=d->cursor;put16(p+6,d->tag);p[8]=d->stage;n=11;}
    else if(d->phase==D_REPLY){kind=REPLY;index=d->self;put16(p+6,d->tag);put16(p+8,d->rules);p[10]=d->stage;n=13;}
    else if(d->phase==D_RESULT){kind=RESULT;index=d->count;put16(p+6,d->tag);p[8]=d->error;n=11;}
    if(n){p[0]=kind;put32(p+1,d->epoch);p[5]=index;put16(p+n-2,link_crc16(p,n-2));}
    d->tx_size=n;
}
static void stream(Discovery *d){
    if(!++d->serial)++d->serial;
    d->epoch=d->serial;d->catalog_rules=d->rules;d->tag=catalog_tag(d);
    d->parts=all_parts(d);d->cursor=0;d->phase=D_OPEN;d->ready=0;d->bound=0;d->committed=0;prepare(d);
}
static void restart(Discovery *d,Wire *w,uint32_t now,uint8_t immediate){
    d->epoch=0;d->count=0;d->self=255;d->parts=0;d->host=0;d->leader_known=0;
    d->ready=d->bound=d->error=d->tx_size=d->retries=d->committed=0;d->phase=D_INITIAL;
    d->send_after=now;
    wire_listen(w,now);
    if(immediate)wire_reselect(w,d->identity,now);else wire_elect(w,d->identity,now);
    d->deadline=now+(uint32_t)w->period*20000;
}
void discovery_init(Discovery *d,Wire *w,const uint8_t identity[WIRE_ID_BYTES],
                    uint16_t rules,uint32_t entropy,uint32_t now){
    uint8_t *bytes=(uint8_t *)d;for(uint16_t i=0;i<sizeof(*d);i++)bytes[i]=0;copy_bytes(d->identity,identity,WIRE_ID_BYTES);d->rules=rules;
    d->serial=entropy?entropy:1;restart(d,w,now,0);
}
static void next_poll(Discovery *d){
    if(d->cursor<d->count){d->phase=D_POLL;prepare(d);}
    else if(d->stage==TABLE)d->phase=D_SCAN;
    else{d->ready=1;d->phase=D_LISTEN;}
}
static void sent(Discovery *d){
    if(d->phase==D_OPEN){d->cursor=0;d->phase=D_LOW;prepare(d);}
    else if(d->phase==D_LOW){d->phase=D_HIGH;prepare(d);}
    else if(d->phase==D_HIGH){
        if(++d->cursor<d->count){d->phase=D_LOW;prepare(d);}
        else{d->stage=TABLE;d->cursor=1;next_poll(d);}
    }else if(d->phase==D_POLL)d->phase=D_WAIT_REPLY;
    else if(d->phase==D_RESULT){
        if(d->error)d->phase=D_LISTEN;
        else if(d->committed==2){d->stage=COMMIT;d->cursor=1;next_poll(d);}
        else{d->committed=1;d->phase=D_SETTLE;}
    }else if(d->phase==D_REPLY){if(d->stage==COMMIT){d->committed=2;d->ready=1;}d->phase=D_LISTEN;}
}
static uint8_t receive(Discovery *d,const uint8_t *p,uint8_t n){
    uint8_t kind=p[0],expected=kind==OPEN?10:kind==UID_LOW||kind==UID_HIGH?14:
                    kind==POLL||kind==RESULT?11:kind==REPLY?13:0;
    if(n!=expected||!expected||get16(p+n-2)!=link_crc16(p,n-2))return 0;
    uint32_t epoch=get32(p+1);uint8_t index=p[5];if(!epoch)return 0;
    if(kind==OPEN&&(!index||index>LINK_MAX_PEERS))return 0;
    if(d->committed==2&&kind==OPEN)return 0;
    if(d->host){
        if(kind==OPEN&&epoch!=d->epoch&&(uint32_t)(epoch-d->epoch)<0x80000000U)return 2;
        if(d->committed!=2&&epoch==d->epoch&&(kind==UID_LOW||kind==UID_HIGH)&&!index&&
           different(p+6,d->identity+(kind==UID_LOW?0:6),6))return 2;
    }
    if(kind==OPEN&&!d->host){
        if(epoch==d->epoch)return index==d->count&&get16(p+6)==d->catalog_rules;
        if(d->epoch&&(uint32_t)(epoch-d->epoch)>=0x80000000U)return 0;
        d->epoch=epoch;d->catalog_rules=get16(p+6);d->count=index;d->parts=0;
        d->self=255;d->ready=d->bound=d->error=d->committed=d->tx_size=0;d->phase=D_LISTEN;return 1;
    }
    if(epoch!=d->epoch)return 0;
    if((kind==UID_LOW||kind==UID_HIGH)&&!d->host){
        if(index>=d->count)return 0;
        uint8_t half=kind==UID_HIGH,bit=1<<(index*2+half),*target=d->ids[index]+half*6;
        if((d->parts&bit)||(d->leader_known&&!index)){
            if(different(target,p+6,6))return 0;
        }else copy_bytes(target,p+6,6);
        d->parts|=bit;
        if((d->parts&3)==3)d->leader_known=1;
        if(d->parts==all_parts(d)){
            for(uint8_t i=0;i<d->count;i++)for(uint8_t j=0;j<i;j++)
                if(!different(d->ids[i],d->ids[j],WIRE_ID_BYTES)){d->error=DISCOVERY_DUPLICATE;return 0;}
            d->self=own_slot(d);d->tag=catalog_tag(d);
        }
        return 1;
    }
    if(kind==POLL&&!d->host){
        if(d->parts!=all_parts(d)||d->self==255||index!=d->self||get16(p+6)!=d->tag||
           (p[8]!=TABLE&&p[8]!=COMMIT)||d->error)return 0;
        // A commit poll is meaningful only after this exact table's OK result.
        if(p[8]==COMMIT&&!d->committed)return 0;
        d->stage=p[8];d->phase=D_REPLY;prepare(d);return 1;
    }
    if(kind==REPLY&&d->host&&d->phase==D_WAIT_REPLY){
        if(index!=d->cursor||get16(p+6)!=d->tag||p[10]!=d->stage)return 0;
        if(get16(p+8)!=d->rules){d->error=DISCOVERY_INCOMPATIBLE|(index<<4);d->phase=D_RESULT;prepare(d);return 1;}
        d->retries=0;d->cursor++;next_poll(d);return 1;
    }
    if(kind==RESULT&&!d->host){
        if(d->parts!=all_parts(d)||index!=d->count||get16(p+6)!=d->tag)return 0;
        uint8_t error=p[8],code=error&15;
        if(code>DISCOVERY_DUPLICATE||(code==DISCOVERY_INCOMPATIBLE&&(error>>4)>=d->count))return 0;
        if(d->committed&&code)return 0;
        if(!code&&d->rules!=d->catalog_rules)return 0;
        // A responsive but unlisted node challenges a missed scan before sealing.
        if(!code&&d->self==255)return 2;
        if(d->committed&&!code)return 1;
        d->error=error;d->tx_size=0;d->phase=D_LISTEN;
        if(code)d->ready=0;else d->committed=1;return 1;
    }
    return 0;
}
// Keep the metadata controller separate from its caller's large render frame.
__attribute__((noinline)) uint8_t discovery_step(Discovery *d,Wire *w,uint32_t now,uint8_t high){
    uint8_t event=wire_step(w,now,high);
    // Metadata receivers validate both frame CRC and the complete identity table.
    // Reserve a protocol turnaround window before any next frame or scan marker.
    if(event)d->send_after=now+(uint32_t)w->period*32;
    if(event==WIRE_ELECTED||event==WIRE_IDENTIFIED){
        d->host=event==WIRE_ELECTED;d->count=1;d->self=d->host?0:255;
        copy_bytes(d->ids[0],w->data,WIRE_ID_BYTES);d->leader_known=1;
        d->epoch=0;d->parts=0;d->ready=d->bound=d->error=d->retries=d->committed=0;d->tx_size=0;
        wire_listen(w,now);
        if(d->host)stream(d);else{d->phase=D_LISTEN;listen(d,w,now);}
        d->deadline=now+(uint32_t)w->period*10000;return status(d);
    }
    if(event==WIRE_SELECTED||event==WIRE_SCANNED){
        if(d->host&&d->phase==D_SCAN_WAIT){
            uint8_t found=0;
            for(uint8_t i=0;i<d->count;i++)if(!different(d->ids[i],w->data,WIRE_ID_BYTES))found=1;
            if(!found&&d->count==LINK_MAX_PEERS){d->error=DISCOVERY_FULL;d->phase=D_RESULT;prepare(d);}
            else{if(!found)copy_bytes(d->ids[d->count++],w->data,WIRE_ID_BYTES);stream(d);}
        }else d->phase=D_LISTEN;
        wire_listen(w,now);if(!d->tx_size)listen(d,w,now);
        d->deadline=now+(uint32_t)w->period*10000;return status(d);
    }
    if(event==WIRE_EMPTY){
        if(d->host&&d->phase==D_SCAN_WAIT){d->error=0;d->phase=D_RESULT;prepare(d);}
        wire_listen(w,now);if(!d->tx_size)listen(d,w,now);
        return status(d);
    }
    if(event==WIRE_SENT){
        sent(d);wire_listen(w,now);if(!d->tx_size)listen(d,w,now);
        // Receivers must allow the coordinator's shorter reply timeout/retry.
        d->deadline=now+(uint32_t)w->period*(d->host?5000:10000);return status(d);
    }
    if(event==WIRE_RECEIVED){
        if(w->length&&w->data[0]<OPEN&&d->ready)return DISCOVERY_DATA;
        uint8_t accepted=w->length?receive(d,w->data,w->length):0;
        if(accepted==2){restart(d,w,now,1);return status(d);}
        wire_listen(w,now);if(!d->tx_size)listen(d,w,now);
        if(accepted)d->deadline=now+(uint32_t)w->period*10000;
        return status(d);
    }
    if(event==WIRE_ERROR){
        wire_listen(w,now);
        if(d->ready){d->phase=D_LISTEN;listen(d,w,now);return status(d);}
        if(d->tx_size)return status(d);
        if(d->host&&d->phase!=D_INITIAL){d->phase=d->committed?D_RESULT:D_OPEN;d->cursor=0;prepare(d);}
        else listen(d,w,now);
        d->deadline=now+(uint32_t)w->period*5000;return status(d);
    }
    // Fast path: preparation happened on an earlier call, before this fresh clock.
    if(d->tx_size){
        if((int32_t)(now-d->send_after)>=0&&wire_send(w,d->tx,d->tx_size,now))d->tx_size=0;
        return status(d);
    }
    if(d->phase==D_SCAN){
        if((int32_t)(now-d->send_after)<0)return status(d);
        wire_listen(w,now);wire_scan(w,d->identity,0,1,now);d->phase=D_SCAN_WAIT;
        d->deadline=now+(uint32_t)w->period*5000;return status(d);
    }
    if(d->phase==D_SETTLE&&wire_quiet(w,now,32)){
        d->committed=2;listen(d,w,now);
        d->stage=COMMIT;d->cursor=1;next_poll(d);return status(d);
    }
    if(!d->ready&&!d->error&&(int32_t)(now-d->deadline)>=0){
        if(++d->retries>=8){d->error=DISCOVERY_TIMEOUT;return status(d);}
        wire_listen(w,now);
        if(d->host){d->phase=d->committed?D_RESULT:D_OPEN;d->cursor=0;prepare(d);}
        else if(d->self<d->count&&d->parts==all_parts(d)){
            // The coordinator may already be sealed and retrying a lost result.
            // Keep the verified table so a restored link can finish confirmation.
            d->phase=D_LISTEN;listen(d,w,now);
        }else restart(d,w,now,1);
        d->deadline=now+(uint32_t)w->period*10000;
    }
    return status(d);
}
uint8_t discovery_bind(Discovery *d,Link *link){
    if(status(d)!=DISCOVERY_READY||d->committed!=2||d->rules!=d->catalog_rules||d->parts!=all_parts(d)||d->self>=d->count)return 0;
    if(d->bound)return link->session==d->epoch&&link->roster==d->tag&&link->count==d->count&&link->self==d->self;
    if(link->session==d->epoch&&link->count)return 0;
    if(!link_configure(link,d->epoch,d->tag,d->count,d->self))return 0;
    d->bound=1;return 1;
}
