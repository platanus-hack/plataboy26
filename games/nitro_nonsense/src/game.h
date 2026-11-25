#ifndef NITRO_GAME_H
#define NITRO_GAME_H
#include <stdint.h>
#define UP 1
#define RIGHT 2
#define DOWN 4
#define LEFT 8
#define ACT 16
#define BACK 32
#define CELLS 112
#define ROUTE_MAX 64
#define CAR_COUNT 4
#define EFFECT_COUNT 8
#define ROAD_SIZE 16
#define WORLD_TOP 8
#define WORLD_WIDTH (16*ROAD_SIZE)
#define WORLD_BOTTOM (WORLD_TOP+7*ROAD_SIZE)
#define LAPS 3
#ifdef NN_MINIMAL
#define MATCH_WINS 3
#define GAME_RULES_VERSION 0x010e
#define GAME_STREAM_RULES_VERSION 0x010f
#else
#define MATCH_WINS 5
#define GAME_RULES_VERSION 0x0103
#define GAME_STREAM_RULES_VERSION 0x0104
#endif

// Phase 2 remains the legacy chooser ID; the fixed-power minimal game never emits it.
// Setup requests return through resident dispatch before replacing an overlay.
enum { RACE_SETUP, MATCH_SETUP, POWER_SELECT, COUNTDOWN, RACE, ROUND_RESULTS, TITLE, MATCH_OVER };
// Empty ID 6 is permanent. Future power IDs start at 7 and need explicit catalog index mapping.
enum { NITRO, CANNON, OIL, MINE, SHIELD, WAVE, POWER_COUNT, NO_POWER=6 };
typedef struct {
    int16_t x, y;
    uint16_t steps, stuck;
    int8_t speed;
    uint8_t heading, next, cooldown[2], powers[2];
    // The reserved byte keeps the existing 24-byte car stride.
    uint8_t boost, shield, stun, slip, wave, finish, reserved, sectors;
} Car;
typedef struct {
    int16_t x, y;
    uint8_t type, owner, life, heading;
} Effect;
typedef struct {
    uint8_t tiles[56], route[ROUTE_MAX], route_len, start, gates[4];
    uint16_t seed;
    uint8_t wins[CAR_COUNT], loadouts[CAR_COUNT][2];
    Car cars[CAR_COUNT];
    Effect effects[EFFECT_COUNT];
    uint16_t ticks, race_ticks;
    uint8_t phase, previous[CAR_COUNT], slot, choice, error;
    uint8_t humans, actor, rewards;
    uint8_t winner, round, finish_count, order[CAR_COUNT];
    uint8_t repeat, action_lock;
} Game;
extern Game game;
extern uint8_t framebuffer[1024];
void game_init(void);
void game_init_seed(uint16_t seed);
void track_generate(uint16_t seed);
uint8_t game_begin(uint16_t seed,uint8_t humans);
void game_set_view(uint8_t player);
void game_step(const uint8_t controls[CAR_COUNT]);
void game_tick(uint8_t held);
void game_draw(void);
#ifdef NN_MINIMAL
void game_draw_lobby(uint8_t count,uint8_t ready,uint8_t self);
// Both setup and title boot inside their active bank; keep one reset rule.
static inline __attribute__((always_inline)) void game_boot(void) {
    uint8_t *bytes=(uint8_t *)&game;
    for(int32_t i=0;i<(int32_t)sizeof(game);i++)bytes[i]=0;
    game.seed=1;game.humans=1;game.phase=TITLE;
    for(int32_t i=0;i<CAR_COUNT;i++){game.loadouts[i][0]=NITRO;game.loadouts[i][1]=CANNON;}
}
uint8_t game_required_group(void);
void game_prepare_race(void);
void game_race_tick(const uint8_t held[CAR_COUNT],const uint8_t edges[CAR_COUNT]);
void game_menu_tick(uint8_t action);
void game_draw_loading(uint8_t error);
void game_draw_help(void);
#endif
uint8_t track_get(uint8_t cell);
#endif
