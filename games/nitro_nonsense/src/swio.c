#include "overlay.h"
#include "swio.h"
#include "swio_internal.h"
#include "discovery.h"
#include "connection.h"
#include "system.h"
#include "gpio.h"

static Wire *owner;
static uint8_t last_low,original_mode,original_high;
NN_OV2 void swio_identity(uint8_t identity[WIRE_ID_BYTES]) {
    // Read all three signature words; the byte order is shared by all consoles.
    const uint32_t words[3]={ESIG_UID1,ESIG_UID2,ESIG_UID3};
    for(uint8_t i=0;i<WIRE_ID_BYTES;i++)identity[i]=words[i>>2]>>((i&3)*8);
}
void swio_drive(uint8_t low) {
    if(low==last_low)return;
    if(low){PIN_output_OD(PD1);PIN_low(PD1);}
    else{PIN_high(PD1);PIN_input_PU(PD1);}
    last_low=low;
}
NN_OV2 uint8_t swio_begin(Wire *wire,uint8_t preserve_debug) {
    if(preserve_debug||owner)return 0;
    RCC->APB2PCENR|=RCC_AFIOEN|RCC_IOPDEN;
    // Refuse ownership when another component has already remapped the pin.
    if(AFIO->PCFR1&AFIO_PCFR1_SWJ_CFG)return 0;
    if(!wire_init(wire,WIRE_BIT_US*DLY_US_TIME,STK->CNT))return 0;
    original_mode=(GPIOD->CFGLR>>4)&15;original_high=!!(GPIOD->OUTDR&2);
    // Select input before changing the output latch; never drive a high level.
    PIN_input_FL(PD1);PIN_high(PD1);PIN_input_PU(PD1);
    AFIO->PCFR1=(AFIO->PCFR1&~AFIO_PCFR1_SWJ_CFG)|AFIO_PCFR1_SWJ_CFG_DISABLE;
    owner=wire;last_low=0;return 1;
}
uint8_t swio_send(Wire *wire,const uint8_t *packet,uint8_t size) {
    if(wire!=owner||!owner)return 0;
    uint8_t sent=wire_send(wire,packet,size,STK->CNT);swio_drive(wire->low);return sent;
}
uint8_t swio_elect(Wire *wire,const uint8_t identity[WIRE_ID_BYTES]) {
    if(wire!=owner||!owner)return 0;
    uint8_t armed=wire_elect(wire,identity,STK->CNT);swio_drive(wire->low);return armed;
}
uint8_t swio_scan(Wire *wire,const uint8_t identity[WIRE_ID_BYTES],uint8_t participate,uint8_t initiate) {
    if(wire!=owner||!owner)return 0;
    uint8_t armed=wire_scan(wire,identity,participate,initiate,STK->CNT);swio_drive(wire->low);return armed;
}
uint8_t swio_poll(Wire *wire) {
    if(wire!=owner||!owner)return WIRE_ERROR;
    uint8_t event=wire_step(wire,STK->CNT,PIN_read(PD1));swio_drive(wire->low);return event;
}
uint8_t swio_discover(Discovery *discovery,Wire *wire) {
    if(wire!=owner||!owner)return DISCOVERY_FAILED;
    uint8_t event=discovery_step(discovery,wire,STK->CNT,PIN_read(PD1));swio_drive(wire->low);return event;
}
uint8_t swio_controller_ready(Wire *wire){
    if(wire!=owner||!owner)return 0;
    if(wire_busy(wire)){
        uint8_t physical=wire_step(wire,STK->CNT,PIN_read(PD1));swio_drive(wire->low);
        if(!physical)return 0;
    }
    return 1;
}
uint8_t swio_connect(Connection *connection,Discovery *discovery,Link *link,Wire *wire) {
    if(!swio_controller_ready(wire))return 0;
    uint8_t event=connection_step(connection,wire,discovery,link,STK->CNT,PIN_read(PD1));
    swio_drive(wire->low);return event;
}
void swio_listen(Wire *wire) {
    if(owner&&wire==owner){wire_listen(wire,STK->CNT);swio_drive(0);}
}
void swio_end(void) {
    if(!owner)return;
    swio_drive(0);PIN_input_FL(PD1);
    // Restore debug ownership before restoring the saved GPIO configuration.
    AFIO->PCFR1&=~AFIO_PCFR1_SWJ_CFG;
    if(original_high)PIN_high(PD1);else PIN_low(PD1);
    GPIOD->CFGLR=(GPIOD->CFGLR&~(15U<<4))|((uint32_t)original_mode<<4);
    owner=0;
}
