// Reference: upstream MicroCity, unmodified, on the host (tools/reftest.py builds it
// from the fetched commit). The Arduboy parts of the sketch (MicroCity.ino) are
// imitated: a 1 KiB frame buffer that is never cleared (the menus are drawn over the
// last frame), Arduboy2's drawPixel and drawBitmap, the power grid in the frame
// buffer, the city saved in memory.
//
//   ref <script> <frames>
// script: a byte per frame (INPUT_* bits). Prints one line per frame: the hash of its
// 8 pages, as the port's test build (port/platform.cpp, MC_TEST) does.
//   ref -
// Interactive (tools/reftest.py writes its script this way): reads each frame's byte from
// stdin, prints the hash and the game's state after it: state selection brush selectX
// selectY money saves loads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Game.h"
#include "Draw.h"
#include "Interface.h"

static uint8_t sBuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];
static uint8_t* script;
static long scriptLen, frame;

static bool interactive;
static int saves, loads;

uint8_t GetInput()
{
	if (interactive) { int c = getchar(); return c == EOF ? 0 : (uint8_t)c; }
	return frame < scriptLen ? script[frame] : 0;
}

void PutPixel(uint8_t x, uint8_t y, uint8_t colour)    // Arduboy2Base::drawPixel
{
	if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
	uint8_t m = 1 << (y & 7);
	if (colour) sBuffer[(y >> 3) * DISPLAY_WIDTH + x] |= m;
	else sBuffer[(y >> 3) * DISPLAY_WIDTH + x] &= ~m;
}

void DrawBitmap(const uint8_t* bmp, uint8_t x, uint8_t y, uint8_t w, uint8_t h)   // drawBitmap(.., WHITE)
{
	int yOffset = y % 8, sRow = y / 8, rows = (h + 7) / 8;
	for (int a = 0; a < rows; a++)
	{
		int bRow = sRow + a;
		if (bRow > DISPLAY_HEIGHT / 8 - 1) break;
		for (int iCol = 0; iCol < w; iCol++)
		{
			if (iCol + x > DISPLAY_WIDTH - 1) break;
			uint8_t b = bmp[a * w + iCol];
			sBuffer[bRow * DISPLAY_WIDTH + x + iCol] |= b << yOffset;
			if (yOffset && bRow < DISPLAY_HEIGHT / 8 - 1)
				sBuffer[(bRow + 1) * DISPLAY_WIDTH + x + iCol] |= b >> (8 - yOffset);
		}
	}
}

static uint8_t saved[4 + sizeof(GameState)];
void SaveCity() { memcpy(saved, "CTY1", 4); memcpy(saved + 4, &State, sizeof(GameState)); saves++; }
bool LoadCity()
{
	if (memcmp(saved, "CTY1", 4)) return false;
	memcpy(&State, saved + 4, sizeof(GameState));
	loads++;
	return true;
}

uint8_t* GetPowerGrid() { return sBuffer; }

int main(int argc, char** argv)
{
	long frames = 0x7FFFFFFF;
	if (argc == 2 && !strcmp(argv[1], "-")) interactive = true;
	else if (argc == 3)
	{
		FILE* f = fopen(argv[1], "rb");
		if (!f) { perror(argv[1]); return 1; }
		script = (uint8_t*)malloc(1 << 20);
		scriptLen = (long)fread(script, 1, 1 << 20, f);
		fclose(f);
		frames = atol(argv[2]);
	}
	else { fprintf(stderr, "usage: ref <script> <frames> | ref -\n"); return 1; }

	InitGame();
	for (frame = 0; frame < frames && !(interactive && feof(stdin)); frame++)
	{
		TickGame();
		uint32_t frameHash = 0;                 // as the port: djb2 per page, combined
		for (int pg = 0; pg < DISPLAY_HEIGHT / 8; pg++)
		{
			uint32_t h = 5381;
			for (int i = 0; i < DISPLAY_WIDTH; i++) h = (h << 5) + h + sBuffer[pg * DISPLAY_WIDTH + i];
			if (!h) h = 1;
			frameHash = frameHash * 33 + h;
		}
		if (getenv("REF_DUMP") && atol(getenv("REF_DUMP")) == frame)   // REF_DUMP=n: frame n as PGM
		{
			FILE* d = fopen("ref_frame.pgm", "wb");
			fprintf(d, "P5\n128 64\n255\n");
			for (int y = 0; y < DISPLAY_HEIGHT; y++)
				for (int x = 0; x < DISPLAY_WIDTH; x++)
					fputc((sBuffer[(y >> 3) * DISPLAY_WIDTH + x] >> (y & 7)) & 1 ? 255 : 0, d);
			fclose(d);
		}
				if (interactive)
		{
			printf("%08x %d %d %d %d %d %ld %d %d\n", frameHash, UIState.state, UIState.selection, UIState.brush,
				UIState.selectX, UIState.selectY, (long)State.money, saves, loads);
			fflush(stdout);
		}
		else printf("%08x\n", frameHash);
	}
	if (getenv("REF_MAP"))                      // REF_MAP=1: the city at the end, on stderr
	{
		for (int y = 0; y < MAP_HEIGHT; y++, fputc('\n', stderr))
			for (int x = 0; x < MAP_WIDTH; x++)
				fputc(GetBuilding(x, y) ? 'B' : ".rpX"[GetConnections(x, y)], stderr);
		for (int n = 0; n < MAX_BUILDINGS; n++)
			if (State.buildings[n].type)
				fprintf(stderr, "building %d at %d,%d power %d density %d\n", State.buildings[n].type,
					State.buildings[n].x, State.buildings[n].y, State.buildings[n].hasPower, State.buildings[n].populationDensity);
	}
	return 0;
}
