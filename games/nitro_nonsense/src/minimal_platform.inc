#ifdef NN_OVERLAYS
#include "overlay_platform.inc"
#else
// Minimal native platform: no configuration, persistent writes or debug-pin changes at boot when B is held.
#include "driver.h"
#include "game.h"
#include "catalog_connection.h"
#include "connection_internal.h"
#include "swio.h"
#include "swio_internal.h"
static Wire wire;
static Catalog catalog;
static Connection connection;
static Stream stream;
static uint8_t identity[WIRE_ID_BYTES];
__attribute__((noinline)) static uint8_t read_input(void){
    static const uint16_t values[8]={JOY_N,JOY_NE,JOY_E,JOY_SE,JOY_S,JOY_SW,JOY_W,JOY_NW};
    static const uint8_t bits[8]={UP,UP|RIGHT,RIGHT,RIGHT|DOWN,DOWN,DOWN|LEFT,LEFT,LEFT|UP};
    uint16_t raw=ADC_read();uint8_t held=0;
    for(uint8_t i=0;i<8;i++)if(raw>values[i]-JOY_DEV&&raw<values[i]+JOY_DEV)held=bits[i];
    if(JOY_act_pressed())held|=ACT;
    if(JOY_b_pressed())held|=BACK;
    return held;
}
static void reset_link(uint32_t now){
    uint8_t *p=(uint8_t *)&stream;for(uint8_t i=0;i<sizeof(stream);i++)p[i]=0;
    catalog_init(&catalog,&wire,identity,CATALOG_RULES,now?now:1,now);
    connection_init(&connection,(uint16_t)now,now);connection_enable_render(&connection);
}
static void display(void){
    OLED_setpos(0,0);OLED_data_start();
    for(uint16_t i=0;i<1024;i++)I2C_write(framebuffer[i]);
    I2C_stop();
}
int main(void){
    JOY_init();game_init();game_draw();display();
    uint8_t solo=JOY_b_pressed(),held=0;
    if(!solo){swio_identity(identity);if(!swio_begin(&wire,0))solo=1;else reset_link(STK->CNT);}
    uint32_t sample_at=STK->CNT,deadline=sample_at;
    while(1){
        uint32_t now=STK->CNT;
        if(solo){
            held=read_input();game_tick(held);game_draw();display();
            deadline+=33*DLY_MS_TIME;
            if((int32_t)(now-deadline)>3*33*DLY_MS_TIME)deadline=now;
            while((int32_t)(STK->CNT-deadline)<0){}
            continue;
        }
        if((int32_t)(now-sample_at)>=0&&(wire_quiet(&wire,now,32)||(connection.render_flags&RENDER_PAUSED)||(catalog.render_until&&(int32_t)(catalog.render_until-now)>0))){
            held=read_input();
            if(!stream.link.running&&(held&ACT)&&!catalog.joined){catalog.joined=1;connection.seed=(uint16_t)(now^(now>>16));}
            stream_sample(&stream,held|(!stream.link.running&&catalog.joined?ACT:0));
            sample_at=now+2*DLY_MS_TIME;
        }
        uint8_t event=0;
        if(swio_controller_ready(&wire)){
            now=STK->CNT;
            event=connection_catalog_step(&connection,&wire,&catalog,&stream,now,PIN_read(PD1));
            swio_drive(wire.low);
        }
        if(!stream.link.running&&catalog.ready&&catalog.count==1&&catalog.joined){
            swio_end();solo=1;game_begin((uint16_t)now,1);deadline=now;continue;
        }
        if(event&CONNECTION_RENDER){
            if(!stream.link.running)game_draw_lobby(catalog.count,catalog.mask,catalog.self);
            else game_draw();
            display();
        }
    }
}

#endif
