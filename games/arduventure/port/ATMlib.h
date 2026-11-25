// ATMlib (Arduboy Tracker Music) player interface. The song format and the play
// routine follow ATMlib (BSD 3-Clause, Copyright (c) 2016-2018 TEAM a.r.g., Davey
// Taylor, Joeri Gantois); the 4 channels are mixed down to the console's one buzzer.
#pragma once
#include "Arduino.h"

struct ATMsynth {
  void play(const uint8_t *song);
  void stop();
  void playPause();
};
