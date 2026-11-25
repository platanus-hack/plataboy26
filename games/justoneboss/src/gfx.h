// ===================================================================================
// Just One Boss: drawing into the current 8-row page (gfx.c)
// ===================================================================================
// Every function draws the part of its shape that falls on rows gy..gy+7 of the
// 128x64 screen into pg[]: the main loop draws the whole scene once per page.
#pragma once
#include <stdint.h>

#define G_FLIPX   1                    // g_spr flags
#define G_DITHER  4                    // lit pixels on a checkerboard only (reflections)

extern int gy;                         // top row of the page

void g_spr(int id, int x, int y, int fl);
void g_pix(int x, int y, int c);
void g_fill(int x0, int y0, int x1, int y1, int c);    // inclusive
int  g_text(const char *s, int x, int y, int scale);   // a line; returns the end x
int  g_num(unsigned n, int x, int y);
void g_ctext(const char *s, int cx, int y, int scale);  // lines ('\n'), centred on cx
