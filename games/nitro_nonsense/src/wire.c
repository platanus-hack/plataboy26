#include "wire.h"
#include "overlay.h"
_Static_assert(WIRE_ID_BYTES<=WIRE_FRAME_MAX&&WIRE_ID_BYTES*8<256,"Identity must fit the wire buffer and bit cursor");
enum {IDLE,RX_GAP,RX_WAIT,RX_BITS,RX_STOP,TX_BREAK,TX_GAP,TX_BITS,HOLD,
      ELECT_IDLE,ELECT_BREAK,ELECT_RISE,ELECT_GAP,ELECT_CLOCK,ELECT_SAMPLE,
      ELECT_RELEASE,ELECT_NEXT,ELECT_WAIT,ELECT_END,SCAN_REQUEST};
#define SCAN_MODE 128

uint8_t wire_busy(const Wire *wire){return wire->state!=IDLE&&wire->state!=ELECT_IDLE;}

uint8_t wire_quiet(const Wire *wire,uint32_t now,uint16_t periods){
    return (wire->state==IDLE||wire->state==ELECT_IDLE)&&wire->seen&&wire->high&&
           (uint32_t)(now-wire->edge)>=(uint32_t)wire->period*periods;
}

void wire_listen(Wire *wire,uint32_t now) {
    wire->state=IDLE;wire->low=0;wire->event=0;wire->seen=0;
    wire->at=wire->edge=now;wire->used=wire->length=0;
}
uint8_t wire_init(Wire *wire,uint16_t period,uint32_t now) {
    if(period<8||(period&1))return 0;
    wire->period=period;wire_listen(wire,now);return 1;
}
uint8_t wire_send(Wire *wire,const uint8_t *packet,uint8_t size,uint32_t now) {
    if(!size||size>WIRE_FRAME_MAX||wire->state!=IDLE||!wire->seen||!wire->high||
       (uint32_t)(now-wire->edge)<(uint32_t)wire->period*2)return 0;
    for(uint8_t i=0;i<size;i++)wire->data[i]=packet[i];
    wire->length=size;wire->used=0;wire->state=TX_BREAK;wire->low=1;
    wire->at=now+(uint32_t)wire->period*12;return 1;
}
static NN_OV1 uint8_t arm_election(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],
                            uint8_t scan,uint8_t participate,uint8_t initiate,uint32_t now) {
    if(wire->state!=IDLE||wire->period<8)return 0;
    for(uint8_t i=0;i<WIRE_ID_BYTES;i++)wire->data[i]=identity[i];
    wire->state=initiate?SCAN_REQUEST:ELECT_IDLE;wire->low=0;wire->seen=0;wire->event=0;
    // UART bit/byte scratch stores the election gap budget, computed once.
    uint16_t margin=((uint32_t)wire->period*2)/3;wire->bit=margin;wire->byte=margin>>8;
    wire->half=!!participate;wire->used=0;wire->length=WIRE_ID_BYTES|(scan?SCAN_MODE:0);
    wire->edge=now;return 1;
}
uint8_t wire_elect(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint32_t now) {
    return arm_election(wire,identity,0,1,0,now);
}
uint8_t wire_reselect(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint32_t now) {
    return arm_election(wire,identity,0,1,1,now);
}
uint8_t wire_scan(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint8_t participate,uint8_t initiate,uint32_t now) {
    return arm_election(wire,identity,1,participate,initiate,now);
}
static uint8_t fail(Wire *wire) {
    wire->low=0;wire->state=HOLD;wire->event=WIRE_ERROR;return WIRE_ERROR;
}
static NN_OV1_LOCAL uint8_t election_clock(Wire *wire,uint32_t now,uint32_t released,int32_t offset) {
    uint32_t phase=(uint32_t)wire->period*2;
    uint16_t margin=(uint16_t)wire->bit|((uint16_t)wire->byte<<8);
    // Reserve an observed high interval despite clock skew and polling latency.
    if(released<margin||offset<-(int32_t)margin||offset>(int32_t)phase)return fail(wire);
    wire->state=ELECT_CLOCK;wire->low=wire->half;wire->at=now+phase;return 0;
}
static NN_OV1 uint8_t election_step(Wire *wire,uint32_t now,uint8_t high,
                             uint8_t falling,uint8_t rising,uint32_t edge_time) {
    uint16_t period=wire->period;
    if(wire->state==ELECT_IDLE||wire->state==SCAN_REQUEST) {
        if(rising&&edge_time>=(uint32_t)period*11) {
            if(edge_time<=(uint32_t)period*13) {
                wire->state=RX_GAP;wire->used=0;wire->at=now+period;
            }else if(edge_time>=(uint32_t)period*15&&edge_time<=(uint32_t)period*18) {
                wire->length=WIRE_ID_BYTES;wire->half=1;
                wire->state=ELECT_GAP;wire->at=now+(uint32_t)period*2;
            }else if(edge_time>=(uint32_t)period*23&&edge_time<=(uint32_t)period*27) {
                wire->length|=SCAN_MODE;
                wire->state=ELECT_GAP;wire->at=now+(uint32_t)period*2;
            }else return fail(wire);
        }else if(!high&&(uint32_t)(now-wire->edge)>(uint32_t)period*30)return fail(wire);
        else if(high&&((wire->state==SCAN_REQUEST&&(uint32_t)(now-wire->edge)>=(uint32_t)period*2)||
                      (!(wire->length&SCAN_MODE)&&(uint32_t)(now-wire->edge)>=(uint32_t)period*WIRE_ELECTION_QUIET_PERIODS))) {
            wire->state=ELECT_BREAK;wire->low=1;
            wire->at=now+(uint32_t)period*((wire->length&SCAN_MODE)?24:16);
        }
        return 0;
    }
    if(wire->state==ELECT_RISE) {
        if(high){wire->state=ELECT_GAP;wire->at=now+(uint32_t)period*2;return 0;}
        return (int32_t)(now-wire->at)>=0?fail(wire):0;
    }
    // Arbitration uses a slower clock than packets: one phase is two bit periods.
    uint32_t phase=(uint32_t)period*2;
    if(wire->state==ELECT_WAIT) {
        if((int32_t)(now-wire->at)>=0) {
            if((wire->length&SCAN_MODE)&&!wire->used&&!wire->half&&high) {
                wire->state=HOLD;wire->length=0;wire->event=WIRE_EMPTY;return WIRE_EMPTY;
            }
            return fail(wire);
        }
        // WAIT opens only after the expected clock deadline has been reached.
        return falling?election_clock(wire,now,edge_time,0):0;
    }
    if((wire->state==ELECT_GAP||wire->state==ELECT_NEXT)&&falling) {
        return election_clock(wire,now,edge_time,(int32_t)(now-wire->at));
    }
    if(wire->state==ELECT_CLOCK&&rising&&edge_time<phase*3/4)return fail(wire);
    int32_t late=(int32_t)(now-wire->at);
    if(late<0)return 0;
    if((uint32_t)late>phase/4)return fail(wire);
    if(wire->state==ELECT_BREAK) {
        wire->low=0;wire->state=ELECT_RISE;wire->at=now+(uint32_t)period*4;
    }else if(wire->state==ELECT_GAP||wire->state==ELECT_NEXT) {
        if(!high)return fail(wire);
        wire->low=wire->half;wire->state=ELECT_WAIT;wire->at=now+phase*2;
    }else if(wire->state==ELECT_CLOCK) {
        wire->low=wire->half&&!(wire->data[wire->used>>3]&(1<<(wire->used&7)));
        wire->state=ELECT_SAMPLE;wire->at+=phase;
    }else if(wire->state==ELECT_SAMPLE) {
        uint8_t mask=1<<(wire->used&7),*byte=&wire->data[wire->used>>3];
        if(wire->half) {
            if(!(*byte&mask)&&high)return fail(wire);
            if((*byte&mask)&&!high)wire->half=0;
        }
        if(high)*byte|=mask;else *byte&=~mask;
        wire->state=ELECT_RELEASE;wire->at+=phase;
    }else if(wire->state==ELECT_RELEASE) {
        wire->low=0;
        wire->state=++wire->used==WIRE_ID_BYTES*8?ELECT_END:ELECT_NEXT;wire->at+=phase;
    }else if(wire->state==ELECT_END) {
        if(!high)return fail(wire);
        wire->state=HOLD;
        wire->event=(wire->length&SCAN_MODE)?(wire->half?WIRE_SELECTED:WIRE_SCANNED):
                    (wire->half?WIRE_ELECTED:WIRE_IDENTIFIED);
        wire->length=WIRE_ID_BYTES;
    }
    return wire->event;
}
uint8_t wire_step(Wire *wire,uint32_t now,uint8_t high) {
    high=!!high;
    uint8_t falling=0,rising=0;uint32_t edge_time=0;
    if(!wire->seen){wire->seen=1;wire->high=high;wire->edge=now;}
    else if(high!=wire->high) {
        rising=high;falling=!high;edge_time=now-wire->edge;
        wire->high=high;wire->edge=now;
    }
    if(wire->state==HOLD)return wire->event;
    if(wire->state>=ELECT_IDLE)return election_step(wire,now,high,falling,rising,edge_time);
    if(wire->state==IDLE) {
        if(rising&&edge_time>=(uint32_t)wire->period*11) {
            if(edge_time>(uint32_t)wire->period*13)return fail(wire);
            wire->state=RX_GAP;wire->used=0;wire->at=now+wire->period;
        }else if(!high&&(uint32_t)(now-wire->edge)>(uint32_t)wire->period*20)return fail(wire);
        return 0;
    }
    if(wire->state==RX_WAIT&&falling) {
        if((int32_t)(now-wire->at)>=0)return fail(wire);
        wire->state=RX_BITS;wire->bit=wire->byte=0;
        wire->at=now+wire->period+wire->period/2;return 0;
    }
    if(wire->state==RX_GAP&&!high)return fail(wire);
    int32_t late=(int32_t)(now-wire->at);
    if(late<0)return 0;
    if(wire->state==RX_WAIT)return fail(wire);
    if((uint32_t)late>wire->period/4)return fail(wire);
    if(wire->state==RX_GAP){wire->state=RX_WAIT;wire->at+=wire->period*3;}
    else if(wire->state==RX_BITS) {
        wire->byte|=high<<wire->bit;
        if(++wire->bit==8)wire->state=RX_STOP;
        wire->at+=wire->period;
    }else if(wire->state==RX_STOP) {
        if(!high)return fail(wire);
        if(!wire->used) {
            if(!wire->byte||wire->byte>WIRE_FRAME_MAX)return fail(wire);
            wire->length=wire->byte;
        }else wire->data[wire->used-1]=wire->byte;
        if(++wire->used>wire->length) {
            wire->state=HOLD;wire->event=WIRE_RECEIVED;return WIRE_RECEIVED;
        }
        wire->state=RX_WAIT;wire->at+=wire->period*3;
    }else if(wire->state==TX_BREAK) {
        wire->low=0;wire->state=TX_GAP;wire->at+=wire->period*2;
    }else if(wire->state==TX_GAP) {
        if(!high)return fail(wire);
        wire->state=TX_BITS;wire->bit=wire->half=0;wire->byte=wire->length;
        wire->low=1;wire->at+=wire->period/2;
    }else if(wire->state==TX_BITS) {
        if(!wire->half) {
            if(high==wire->low)return fail(wire);
            wire->half=1;
        }else {
            wire->half=0;
            if(wire->bit==9) {
                if(++wire->used>wire->length) {
                    wire->low=0;wire->state=HOLD;wire->event=WIRE_SENT;return WIRE_SENT;
                }
                wire->byte=wire->data[wire->used-1];wire->bit=0;wire->low=1;
            }else {
                wire->bit++;
                wire->low=wire->bit==9?0:!(wire->byte&(1<<(wire->bit-1)));
            }
        }
        wire->at+=wire->period/2;
    }
    return wire->event;
}
