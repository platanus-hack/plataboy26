#ifndef NITRO_MUSIC_H
#define NITRO_MUSIC_H
#include <stdint.h>
enum { NN_AUDIO_NITRO=1, NN_AUDIO_CANNON=2, NN_AUDIO_HIT=3 };
// Zero selects melody; 1..19 selects the local engine at speed 0..18.
void nn_music_scene(uint8_t engine);
void nn_music_fx(uint8_t kind);
void nn_music_start(uint32_t now);
void nn_music_pause(void);
void nn_music_quiet(void);
void nn_music_resume(uint32_t now);
void nn_music_step(uint32_t now);
void nn_music_poll(void);

// Bounded work services the same clock; only actual blocking operations mute it.
#if defined(NN_OVERLAYS) && defined(__riscv)
#define NN_MUSIC_POLL() nn_music_poll()
#else
#define NN_MUSIC_POLL() ((void)0)
#endif
#endif
