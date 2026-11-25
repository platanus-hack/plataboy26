#include "driver.h"
#include "sysx.h"
#include "eeprom.h"
#include "game.h"
#include "catalog_connection.h"
#include "connection_internal.h"
#include "load_pause.h"
#include "load_gate.h"
#include "link_frame.h"
#include "session_internal.h"
#include "music.inc"
#include "overlay.h"
#include "swio.h"
#include "swio_internal.h"

static Wire wire;
static Catalog catalog;
static Connection connection;
static Stream stream;
static uint8_t solo;
static LoadGate startup_gate;
static uint8_t identity[WIRE_ID_BYTES];

// Local sound observes committed state without changing gameplay or wire bytes.
static void sound_frame(void) {
    static uint8_t boost, cannon, stun;
    const Car *car=&game.cars[solo?0:stream.link.self];
    uint8_t speed=car->speed<0?-car->speed:car->speed;
    nn_music_scene(game.phase==RACE?speed+1:0);
    if(game.phase==RACE) {
        if(car->stun>stun)nn_music_fx(NN_AUDIO_HIT);
        else if(car->cooldown[1]>cannon)nn_music_fx(NN_AUDIO_CANNON);
        else if(car->boost>boost)nn_music_fx(NN_AUDIO_NITRO);
    }
    boost=car->boost;cannon=car->cooldown[1];stun=car->stun;
}

__attribute__((noinline)) static uint8_t read_input(void) {
    // joypad state from SYS_poll (games/common/joycal.h: idle R U UR D DR UD L UL DL none)
    static const uint8_t bits[11] __attribute__((section(".rodata.controls")))={0,RIGHT,UP,UP|RIGHT,DOWN,DOWN|RIGHT,0,LEFT,UP|LEFT,DOWN|LEFT,0};
    // plataboy26 library: calibrated; UP + DOWN held 2 s -> the menu (games/common/sysx.c)
    uint8_t held=bits[SYS_poll(ADC_read())];
    if(JOY_act_pressed())held|=ACT;
    if(JOY_b_pressed())held|=BACK;
    return held;
}

static void display(void) {
    OLED_setpos(0,0);OLED_data_start();
    for(uint16_t i=0;i<1024;i++) {
        I2C_write(framebuffer[i]);nn_music_step(STK->CNT);
    }
    I2C_stop();
}

__attribute__((noreturn)) static void memory_error(uint8_t error) {
    nn_music_pause();
    if(!solo&&stream.link.session){
        connection.render_flags|=LOAD_PAUSED|LOAD_FAILED;
        connection.size=connection.waiting=0;swio_drive(1);
    }
    if(!nn_overlay_current())OLED_init();
    game_draw_loading(error);display();
    if(!solo&&stream.link.session)swio_drive(0);
    // Preserve canonical game state and keep recovery code outside the window.
    while(1){}
}

static void load_solo(uint8_t group) {
    if(!group||nn_overlay_current()==group)return;
    nn_music_quiet();
    uint8_t error=nn_overlay_load(group);
    if(error)memory_error(error);
    nn_music_resume(STK->CNT);
}

__attribute__((noinline)) void nn_music_poll(void) {
    nn_music_step(STK->CNT);
}

void session_load_begin(void) {
    load_pause_begin(&connection,&wire);swio_drive(1);
}
void session_load_group(uint8_t group) { load_solo(group); }
void session_load_end(void) {
    sound_frame();
    load_pause_end(&connection,&wire,0,STK->CNT);swio_drive(wire.low);
}

static void startup_bank(uint8_t preserve_packet) {
    wire.low=1;swio_drive(1);load_solo(NN_OVERLAY_GATE);
    if(!preserve_packet)wire_listen(&wire,STK->CNT);
    else wire.low=0;
    swio_drive(0);
    load_gate_begin(&startup_gate,&connection,&wire,&stream.link,
                    NN_OVERLAY_GATE,STK->CNT);
}

static NN_OV2 uint8_t startup_identity(uint32_t now,uint8_t high) {
    uint8_t event=load_gate_step(&startup_gate,&connection,&wire,&stream.link,
                                nn_overlay_build_id(),now,high);
    if(event&LOAD_GATE_LOAD) {
        // This bank stays active until the complete identity handshake returns.
        if(nn_overlay_current()!=NN_OVERLAY_GATE)memory_error(NN_OVERLAY_INTEGRITY);
        swio_drive(wire.low);
        load_gate_complete(&startup_gate,&connection,&wire,0,STK->CNT);
    }else if(event&LOAD_GATE_FAILED)memory_error(startup_gate.error);
    else if((event&LOAD_GATE_RESUMED)||(!startup_gate.state&&!stream.link.self))
        return 1;
    else if((event&LOAD_GATE_NORMAL)&&wire.event)wire_listen(&wire,now);
    return 0;
}

static NN_OV1 uint8_t startup_catalog(uint32_t now,uint8_t high) {
    uint8_t event=catalog_step(&catalog,&wire,now,high);
    // Closing Catalog updates remain authoritative until this bank is left.
    // A late cohort/epoch update must replace an earlier pre-START binding.
    if(event&(CATALOG_READY|CATALOG_DATA))
        catalog_bind(&catalog,&stream.link);
    return event;
}

static uint8_t startup_service(uint32_t now,uint8_t high) {
    if(nn_overlay_current()==NN_OVERLAY_CATALOG) {
        uint8_t event=NN_BANK_CALL(startup_catalog,now,high);
        if(stream.link.session&&stream.link.count>1) {
            if(!stream.link.self)startup_bank(0);
            else if((event&CATALOG_DATA)&&wire.data[0]==LOAD_GATE_ID_PACKET&&
                    link_frame_valid(&stream.link,wire.data,wire.length)&&
                    !get16(wire.data+1)&&((wire.data[9]>>2)&3)==NN_OVERLAY_GATE-1&&
                    (wire.data[9]&3)&&((wire.data[9]&3)<stream.link.count)&&!(wire.data[9]&128))
                startup_bank(1);
        }
        // A rejected bootstrap frame must not leave RXHOLD swallowing retries.
        if((event&CATALOG_DATA)&&nn_overlay_current()==NN_OVERLAY_CATALOG)
            wire_listen(&wire,now);
        return event&CATALOG_RENDER?CONNECTION_RENDER:0;
    }
    if(NN_BANK_CALL(startup_identity,now,high))
        return connection_stream_service(&connection,&wire,&stream,now,high);
    return 0;
}

static NN_OV4 void boot_game(void) {
    JOY_init();solo=JOY_b_pressed();
    EE_read(JOYCAL_EE,SYS_cal,JOYCAL_N);   // joypad thresholds, measured by the menu (sysx.h)
    game_boot();
    game_draw_loading(0);
    display();
    uint32_t now=STK->CNT;
    connection_init(&connection,(uint16_t)now,now);
    connection_enable_render(&connection);
}

static NN_OV2 void boot_link(void) {
    swio_identity(identity);
    if(!swio_begin(&wire,0))solo=1;
}

// The resident entry loads the title bank before input or music starts.
static NN_OV4 void title_wait(void) {
    nn_music_start(STK->CNT);
    uint8_t help=0,action;
    game_draw();display();
    while(read_input())nn_music_step(STK->CNT);
    for(;;) {
        while(!(action=read_input()&(ACT|BACK)))nn_music_step(STK->CNT);
        if(action&ACT) {
            while(read_input())nn_music_step(STK->CNT);
            return;
        }
        help^=1;
        if(help)game_draw_help();else game_draw();
        display();
        while(read_input())nn_music_step(STK->CNT);
    }
}

static NN_OV4 void solo_lobby_wait(void) {
    game_draw_lobby(1,0,0);display();
    while(!(read_input()&ACT))nn_music_step(STK->CNT);
    while(read_input())nn_music_step(STK->CNT);
}

int main(void) {
    // EEPROM access needs only I2C before the verified boot bank can run.
    I2C_init();load_solo(NN_OVERLAY_CHOICES);
    NN_BANK_CALL(boot_game);
    uint8_t held=0;
    if(!solo) {
        load_solo(NN_OVERLAY_GATE);NN_BANK_CALL(boot_link);
    }
    load_solo(NN_OVERLAY_CHOICES);
    NN_BANK_CALL(title_wait);
    if(solo) {
        NN_BANK_CALL(solo_lobby_wait);
        load_solo(NN_OVERLAY_SETUP);
        NN_BANK_CALL(game_begin,(uint16_t)STK->CNT,1);
    }
    if(!solo) {
        load_solo(NN_OVERLAY_CATALOG);
        uint32_t now=STK->CNT;
        NN_BANK_CALL(catalog_init,&catalog,&wire,identity,CATALOG_RULES,now?now:1,now);
    }
    uint32_t sample_at=STK->CNT,deadline=sample_at;
    while(1) {
        uint32_t now=STK->CNT;if(nn_music_due(now))nn_music_step(now);
        if(solo) {
            load_solo(game_required_group());
            held=read_input();game_tick(held);sound_frame();
            load_solo(game_required_group());game_draw();display();
            deadline=now+33*DLY_MS_TIME;
            while((int32_t)(STK->CNT-deadline)<0)nn_music_step(STK->CNT);
            continue;
        }
        if(!(connection.render_flags&LOAD_PAUSED)&&(int32_t)(now-sample_at)>=0&&
           (wire_quiet(&wire,now,32)||(connection.render_flags&RENDER_PAUSED)||
            (catalog.render_until&&(int32_t)(catalog.render_until-now)>0))) {
            held=read_input();
            if(!stream.link.running&&(held&ACT)&&!catalog.joined) {
                catalog.joined=1;connection.seed=(uint16_t)(now^(now>>16));
            }
            stream_sample(&stream,held|(!stream.link.running&&catalog.joined?ACT:0));
            sample_at=now+2*DLY_MS_TIME;
        }
        uint8_t event=0;
        if(swio_controller_ready(&wire)) {
            // Wire timing must describe this pin sample, after audio/input work.
            uint8_t high=PIN_read(PD1);now=STK->CNT;
            if(!stream.link.running)
                event=startup_service(now,high);
            else event=connection_stream_service(&connection,&wire,&stream,now,high);
            swio_drive(wire.low);
        }
        if(!stream.link.running&&catalog.ready&&catalog.count==1&&catalog.joined) {
            swio_end();solo=1;load_solo(NN_OVERLAY_SETUP);
            NN_BANK_CALL(game_begin,(uint16_t)now,1);deadline=now;continue;
        }
        if(event&CONNECTION_RENDER) {
            if(!stream.link.running)game_draw_lobby(catalog.count,catalog.mask,catalog.self);
            else if(nn_overlay_current()==game_required_group())game_draw();
            else game_draw_loading(0);
            display();
        }
    }
}
