// Arduino core subset for the CH32V003 game console (Arduventure port)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "pgmspace.h"

typedef uint8_t  byte;
typedef uint16_t word;
typedef bool     boolean;

#define HIGH 1
#define LOW  0

long random(long howbig);
long random(long howsmall, long howbig);
void randomSeed(unsigned long seed);
unsigned long millis(void);

#define min(a, b)          ((a) < (b) ? (a) : (b))
#define max(a, b)          ((a) > (b) ? (a) : (b))
#define abs(x)             ((x) > 0 ? (x) : -(x))
#define constrain(x, l, h) ((x) < (l) ? (l) : ((x) > (h) ? (h) : (x)))
#define _BV(b)             (1UL << (b))
#define bitRead(v, b)      (((v) >> (b)) & 0x01)
#define bitSet(v, b)       ((v) |= (1UL << (b)))
#define bitClear(v, b)     ((v) &= ~(1UL << (b)))
#define bitWrite(v, b, x)  ((x) ? bitSet(v, b) : bitClear(v, b))

char *itoa(int value, char *s, int radix);

struct SerialStub { size_t write(const uint8_t *, size_t n) { return n; } };
extern SerialStub Serial;
