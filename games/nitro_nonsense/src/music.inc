#include "music.h"
#include "buzzer.h"   // BUZ_PULSE_US (plataboy26: low phase capped, games/common/buzzer.h)

static uint32_t next_edge, next_beat, beat_remaining;
static uint16_t period_us;
static const uint16_t melody[]={
        7645,0,5102,4545,3822,4545,5102,0,
        6070,5102,4545,0,3822,3405,3034,0,
        3822,4545,5102,6070,6810,6070,5102,0,
        4545,3822,3405,4545,5102,6070,7645,0};

static uint8_t note, enabled, muted, pulse_low, fx_kind, fx_left, engine, voice_shift;
static const uint16_t effects[] __attribute__((section(".rodata.music")))={
    5200,3900,3000,2800,5600,11200,4500,6500,9000};

static uint16_t background_period(void) {
    return engine?24000-((uint16_t)(engine-1)<<9):melody[note];
}

__attribute__((noinline)) void nn_music_scene(uint8_t scene) {
    if(scene==engine)return;
    uint8_t changed=!scene!=!engine;engine=scene;
    if(changed) {
        fx_kind=fx_left=pulse_low=0;
        next_edge=STK->CNT+100*DLY_US_TIME;PIN_high(PIN_BEEP);
    }
    if(!fx_kind){period_us=background_period();voice_shift=engine?6:3;}
}

void nn_music_start(uint32_t now) {
    note=31;next_edge=next_beat=now;beat_remaining=0;
    period_us=0;enabled=1;muted=pulse_low=fx_kind=fx_left=engine=0;voice_shift=3;PIN_high(PIN_BEEP);
}

void nn_music_fx(uint8_t kind) {
    if(!enabled||(uint8_t)(kind-1)>=NN_AUDIO_HIT||kind<fx_kind)return;
    fx_kind=kind;fx_left=24;period_us=effects[(kind-1)*3];voice_shift=4;
    next_edge=STK->CNT+100*DLY_US_TIME;pulse_low=0;PIN_high(PIN_BEEP);
}

void nn_music_pause(void) {
    enabled=0;muted=1;pulse_low=0;PIN_high(PIN_BEEP);
}

void nn_music_quiet(void) {
    if(!enabled)return;
    if(!muted) {
        uint32_t now=STK->CNT;
        beat_remaining=(int32_t)(next_beat-now)>0?next_beat-now:0;
        muted=1;
    }
    pulse_low=0;PIN_high(PIN_BEEP);
}

void nn_music_resume(uint32_t now) {
    if(enabled&&muted) {
        next_beat=now+beat_remaining;next_edge=now+100*DLY_US_TIME;
        muted=pulse_low=0;
    }
}

// The wire loop can avoid a service call until the owner's next deadline.
static inline uint8_t nn_music_due(uint32_t now) {
    return enabled&&!muted&&((int32_t)(now-next_beat)>=0||
        (period_us&&(int32_t)(now-next_edge)>=0));
}

// Keep the phrase across quiet work; poll without delays or timer interrupts.
void nn_music_step(uint32_t now) {

    if(!enabled||muted)return;
    if((int32_t)(now-next_beat)>=0) {
        next_beat=now+280*DLY_MS_TIME;
        if(!engine) {
            note=(note+1)&31;
            if(!fx_kind)period_us=melody[note];
            // Separate adjacent notes with a real HIGH interval.
            next_edge=now+100*DLY_US_TIME;pulse_low=0;PIN_high(PIN_BEEP);
        }
    }
    if(period_us&&(int32_t)(now-next_edge)>=0) {
        uint32_t period=(uint32_t)period_us*DLY_US_TIME;
        pulse_low^=1;
        if(pulse_low)PIN_low(PIN_BEEP);else {
            PIN_high(PIN_BEEP);
            if(fx_kind&&!(--fx_left&7)) {
                if(!fx_left){fx_kind=0;period_us=background_period();voice_shift=engine?6:3;}
                else period_us=effects[(fx_kind-1)*3+(24-fx_left)/8];
            }
        }
        uint32_t low=period>>voice_shift;
        if(low>BUZ_PULSE_US*DLY_US_TIME)low=BUZ_PULSE_US*DLY_US_TIME;
        uint32_t duration=pulse_low?low:period-low;
        next_edge+=duration;
        // Missed edges never replay a burst or hold a stale LOW through work.
        if((int32_t)(now-next_edge)>=0)next_edge=now+duration;
    }
}
