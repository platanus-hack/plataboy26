// ===================================================================================
// plataboy26: buzzer pulse limit                                         (buzzer.h)
// ===================================================================================
//
// The buzzer (MLT-5030, ~4 kHz resonance) hangs from PA1 through only 22R, active low:
// while the pin is low the coil draws tens of mA. On board rev. 1.3.x that comes from a
// CR2032 behind a 10 uF capacitor; the cell's internal resistance (20-50 R) and the
// capacitor answer in ~0.2-0.5 ms, so a low phase of that length or longer is carried by
// the cell and its voltage dips by I * R (~1 V on a tired cell): enough for a reset.
// The sound comes from the edges (the coil current settles within a few us, the
// diaphragm rings at its resonance): a pulse of half the resonance period (~125 us)
// excites it most, the rest of a long low phase is DC that only drains the cell.
//
// So every sound routine keeps the pin low for at most BUZ_PULSE_US per period (same
// period = same pitch) - mostly carried by the capacitor: 40 mA * 80 us / 10 uF = 0.3 V
// instead of a ~1 V dip - and Arduventure's PWM synthesizer centres its wave on the
// lowest level the wave needs instead of on 50 %.

#pragma once

#ifndef BUZ_PULSE_US
#define BUZ_PULSE_US  80                // longest low phase of the buzzer pin, in us
#endif
