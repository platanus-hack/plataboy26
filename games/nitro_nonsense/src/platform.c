#ifdef NN_MINIMAL
#include "minimal_platform.inc"
#else
// Platform glue uses the pinned devkit drivers without modifying their contracts.
#include "driver.h"
#include "game.h"

static uint8_t input_sample(void) {
    static const uint16_t values[8] = {JOY_N,JOY_NE,JOY_E,JOY_SE,JOY_S,JOY_SW,JOY_W,JOY_NW};
    static const uint8_t bits[8] = {UP,UP|RIGHT,RIGHT,RIGHT|DOWN,DOWN,DOWN|LEFT,LEFT,LEFT|UP};
    uint16_t raw = ADC_read();
    uint8_t held = 0;
    for(uint8_t i=0;i<8;i++)
        if(raw > values[i]-JOY_DEV && raw < values[i]+JOY_DEV) held=bits[i];
    if(JOY_act_pressed()) held |= ACT;
    else if(JOY_b_pressed()) held |= BACK;
    return held;
}
int main(void) {
    JOY_init();
    game_init();
    uint32_t deadline=STK->CNT;
    const uint32_t step=33*DLY_MS_TIME;
    while(1) {
        uint8_t held=input_sample(),catch_up=0;
        // Reuse one sampled input for bounded fixed steps. game_tick owns edges,
        // so a catch-up step cannot activate the same held power a second time.
        do {
            game_tick(held);
            deadline+=step;
            catch_up++;
        } while((int32_t)(STK->CNT-deadline)>=0&&catch_up<3);
        game_draw();
        OLED_setpos(0,0);
        OLED_data_start();
        for(uint16_t i=0;i<1024;i++) I2C_write(framebuffer[i]);
        I2C_stop();
        while((int32_t)(STK->CNT-deadline)<0) {}
    }
}

#endif
