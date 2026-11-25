#ifdef NN_MINIMAL
#include "lobby_catalog.inc"
#else
#include "catalog.h"
#include "link_frame.h"
enum {C_INITIAL,C_OPEN,C_POLL,C_WAIT,C_SCAN,C_SCAN_WAIT,C_RESULT,C_REPLY,C_LISTEN,C_SETTLE,C_RESET};
enum {CAT_OPEN=0xc0,CAT_POLL,CAT_REPLY,CAT_RESULT,CAT_RESET};
// Let an unknown peer finish the existing quiet-time election even if it lost
// the only RESULT advertisement. Known peers remain observers during this gap.
enum {CATALOG_SETTLE_PERIODS=WIRE_ELECTION_QUIET_PERIODS+1000};
static void copy(uint8_t *a,const uint8_t *b,int n){for(int i=0;i<n;i++)a[i]=b[i];}
static int different(const uint8_t *a,const uint8_t *b,int n){for(int i=0;i<n;i++)if(a[i]!=b[i])return 1;return 0;}
static uint32_t epoch_of(const uint8_t *p){uint32_t n=0;for(int i=0;i<4;i++)n|=(uint32_t)p[i+1]<<(8*i);return n;}
static int status(const Catalog *c){return c->ready&&c->phase==C_LISTEN&&!c->size?CATALOG_READY:c->error?CATALOG_FAILED:0;}
static void index_table(Catalog *c){
    c->self=255;for(int i=0;i<c->count;i++)if(!different(c->identity,c->ids[i],WIRE_ID_BYTES))c->self=i;
    c->tag=link_crc16(&c->ids[0][0],c->count*WIRE_ID_BYTES)^c->remote_rules;
}
static void listen(Catalog *c,Wire *w,uint32_t now){wire_listen(w,now);if(c->sealed!=2)wire_scan(w,c->identity,c->self==255,0,now);}
static void prepare(Catalog *c){
    uint8_t *p=c->tx;int n=12;
    p[0]=c->phase==C_OPEN?CAT_OPEN:c->phase==C_POLL?CAT_POLL:c->phase==C_REPLY?CAT_REPLY:CAT_RESULT;
    for(int i=0;i<4;i++)p[i+1]=c->epoch>>(8*i);
    p[5]=c->count;put16(p+6,c->tag);
    if(c->phase==C_OPEN)put16(p+8,c->remote_rules);
    else if(c->phase==C_POLL||c->phase==C_REPLY){p[8]=c->phase==C_POLL?c->cursor:c->self;p[9]=c->stage;if(c->phase==C_REPLY){put16(p+10,c->rules);n=14;}}
    else{p[8]=c->error;put16(p+9,c->remote_rules);n=13;}
    put16(p+n-2,link_crc16(p,n-2));c->size=n;
}
static void restart(Catalog *c,Wire *w,uint32_t now){
    c->epoch=0;c->count=0;c->self=255;c->host=c->size=c->ready=c->error=c->sealed=c->bound=c->retries=0;c->phase=C_INITIAL;
    wire_listen(w,now);wire_elect(w,c->identity,now);
    c->send_after=now;c->deadline=now+(uint32_t)w->period*20000;
}
void catalog_init(Catalog *c,Wire *w,const uint8_t id[WIRE_ID_BYTES],uint16_t rules,uint32_t entropy,uint32_t now){
    uint8_t *p=(uint8_t *)c;for(unsigned i=0;i<sizeof(*c);i++)p[i]=0;
    copy(c->identity,id,WIRE_ID_BYTES);c->rules=rules;c->serial=entropy?entropy:1;restart(c,w,now);
}
static void next_poll(Catalog *c){
    if(c->cursor<c->count){c->phase=C_POLL;prepare(c);}
    else if(!c->stage)c->phase=C_SCAN;
    else{c->sealed=2;c->ready=1;c->phase=C_LISTEN;}
}
static int challenge(Catalog *c,const uint8_t *p,uint16_t rules){
    copy(c->tx,p,8);c->tx[0]=CAT_RESET;put16(c->tx+8,rules);
    put16(c->tx+10,link_crc16(c->tx,10));c->size=12;c->phase=C_RESET;return 1;
}
static int receive(Catalog *c,const uint8_t *p,int n){
    int kind=p[0],expected=kind==CAT_OPEN||kind==CAT_POLL||kind==CAT_RESET?12:kind==CAT_REPLY?14:kind==CAT_RESULT?13:0;
    if(n!=expected||!expected||get16(p+n-2)!=link_crc16(p,n-2))return 0;
    uint32_t epoch=epoch_of(p);if(!epoch)return 0;
    if(kind==CAT_OPEN){
        if(!p[5]||p[5]>LINK_MAX_PEERS||c->sealed)return 0;
        if(c->host)return epoch!=c->epoch&&(uint32_t)(epoch-c->epoch)<0x80000000U?2:0;
        if(c->epoch&&epoch!=c->epoch&&(uint32_t)(epoch-c->epoch)>=0x80000000U)return 0;
        if(c->epoch==epoch&&(p[5]<c->count||get16(p+8)!=c->remote_rules))return 0;
        if(!c->count||p[5]!=c->count){
            return challenge(c,p,get16(p+8));
        }
        c->remote_rules=get16(p+8);index_table(c);
        if(get16(p+6)!=c->tag){
            return challenge(c,p,get16(p+8));
        }
        if(c->epoch==epoch)return 1; // Duplicate OPEN cannot cancel a queued reply.
        c->epoch=epoch;c->phase=C_LISTEN;return 1;
    }
    // A newly attached peer can challenge the provisional result before any
    // commit reply seals the group, even if it missed the preceding identities.
    if(kind==CAT_RESULT&&!c->host&&!c->sealed&&!p[8]&&p[5]&&p[5]<=LINK_MAX_PEERS&&
       (!c->epoch||epoch!=c->epoch||p[5]!=c->count||get16(p+6)!=c->tag||c->self==255))
        return challenge(c,p,get16(p+9));
    if(epoch!=c->epoch||p[5]!=c->count||get16(p+6)!=c->tag)return 0;
    if(kind==CAT_RESET)return get16(p+8)!=c->remote_rules||c->sealed==2||(c->sealed&&c->phase!=C_SETTLE)?0:2;
    if(kind==CAT_POLL&&!c->host){
        if(p[8]!=c->self||p[9]>1||(p[9]&&!c->sealed)||(!p[9]&&c->sealed)||c->error)return 0;
        c->stage=p[9];c->phase=C_REPLY;prepare(c);return 1;
    }
    if(kind==CAT_REPLY&&c->host&&c->phase==C_WAIT){
        if(p[8]!=c->cursor||p[9]!=c->stage)return 0;
        if(get16(p+10)!=c->rules){c->error=2|(c->cursor<<4);c->phase=C_RESULT;prepare(c);return 1;}
        c->retries=0;c->cursor++;next_poll(c);return 1;
    }
    if(kind==CAT_RESULT&&!c->host){
        if((p[8]&15)>4||get16(p+9)!=c->remote_rules)return 0;
        if(!p[8]&&c->self==255)return challenge(c,p,get16(p+9));
        if(c->sealed==2)return !p[8];
        c->error=p[8];c->phase=C_LISTEN;c->size=0;
        if(!c->error&&c->rules==c->remote_rules)c->sealed=1;
        return 1;
    }
    return 0;
}
// Keep catalog bookkeeping outside the caller's large render frame.
__attribute__((noinline)) uint8_t catalog_step(Catalog *c,Wire *w,uint32_t now,uint8_t high){
    int event=wire_step(w,now,high);
    if(event)c->send_after=now+(uint32_t)w->period*32;
    if(event==WIRE_ELECTED||event==WIRE_IDENTIFIED){
        c->host=event==WIRE_ELECTED;c->count=1;c->self=c->host?0:255;c->sealed=c->ready=c->size=c->error=c->bound=0;
        copy(c->ids[0],w->data,WIRE_ID_BYTES);c->remote_rules=c->rules;index_table(c);c->stage=0;c->cursor=1;
        if(c->host){if(!++c->serial)++c->serial;c->epoch=c->serial;c->phase=C_OPEN;prepare(c);}else{c->epoch=0;c->phase=C_LISTEN;}
    }else if(event==WIRE_SELECTED||event==WIRE_SCANNED){
        if(c->sealed==2)return status(c);
        int duplicate=0;for(int i=0;i<c->count;i++)if(!different(c->ids[i],w->data,WIRE_ID_BYTES))duplicate=1;
        if(duplicate){restart(c,w,now);return 0;}
        if(c->count==LINK_MAX_PEERS)c->error=1;
        else{copy(c->ids[c->count++],w->data,WIRE_ID_BYTES);index_table(c);}
        if(c->host){c->phase=c->error?C_RESULT:C_OPEN;prepare(c);}else c->phase=C_LISTEN;
    }else if(event==WIRE_EMPTY){if(c->host&&c->phase==C_SCAN_WAIT){c->phase=C_RESULT;prepare(c);}}
    else if(event==WIRE_SENT){
        if(c->phase==C_OPEN){c->cursor=1;c->stage=0;next_poll(c);}
        else if(c->phase==C_POLL)c->phase=C_WAIT;
        else if(c->phase==C_RESULT){if(c->error)c->phase=C_LISTEN;else{c->sealed=1;c->stage=1;c->cursor=1;c->phase=C_SETTLE;}}
        else if(c->phase==C_REPLY){if(c->stage){c->sealed=2;c->ready=1;}c->phase=C_LISTEN;}
    }else if(event==WIRE_RECEIVED){
        if(w->length&&w->data[0]<CAT_OPEN&&c->ready)return CATALOG_DATA;
        int accepted=w->length?receive(c,w->data,w->length):0;
        if(accepted==2){restart(c,w,now);return 0;}
        if(!accepted){wire_listen(w,now);if(!c->size)listen(c,w,now);return status(c);}
    }else if(event==WIRE_ERROR){
        // A valid commit POLL seals the client's catalog even if local TX stop
        // service is late. The caller still serves metadata until a valid POLL
        // activates the session; the host's all-ACK barrier remains authoritative.
        if(c->phase==C_REPLY&&c->stage){c->sealed=2;c->ready=1;c->phase=C_LISTEN;}
        if(c->phase==C_RESET){c->size=12;wire_listen(w,now);return 0;}
        if(c->phase==C_INITIAL||(c->host&&!c->sealed)){restart(c,w,now);return 0;}
        if(c->host&&!c->ready&&c->phase!=C_INITIAL){c->phase=c->sealed?C_RESULT:C_OPEN;prepare(c);}
    }
    if(event){wire_listen(w,now);if(!c->size)listen(c,w,now);c->deadline=now+(uint32_t)w->period*(c->host&&c->phase!=C_SETTLE?5000:10000);return status(c);}
    if(c->size){if((int32_t)(now-c->send_after)>=0&&wire_send(w,c->tx,c->size,now))c->size=0;return status(c);}
    if(c->phase==C_SETTLE&&wire_quiet(w,now,CATALOG_SETTLE_PERIODS)){wire_listen(w,now);next_poll(c);return status(c);}
    if(c->phase==C_SCAN){
        if((int32_t)(now-c->send_after)<0)return status(c);
        wire_listen(w,now);wire_scan(w,c->identity,0,1,now);c->phase=C_SCAN_WAIT;c->deadline=now+(uint32_t)w->period*5000;return status(c);
    }
    if(!c->ready&&!c->error&&(int32_t)(now-c->deadline)>=0){
        if(++c->retries>=8){c->error=3;return status(c);}
        if(c->host){c->phase=c->sealed?C_RESULT:C_OPEN;prepare(c);}
        else if(!c->epoch||c->self>=c->count){restart(c,w,now);return 0;}
        wire_listen(w,now);if(!c->size)listen(c,w,now);c->deadline=now+(uint32_t)w->period*10000;
    }
    return status(c);
}
uint8_t catalog_bind(Catalog *c,Link *link){
    if(!c->ready||c->error||c->sealed!=2||c->rules!=c->remote_rules||c->self>=c->count)return 0;
    if(c->bound)return link->session==c->epoch&&link->roster==c->tag&&link->count==c->count&&link->self==c->self;
    if(!link_configure(link,c->epoch,c->tag,c->count,c->self))return 0;
    c->bound=1;return 1;
}

#endif
