#include "game.h"
#include "overlay.h"
#include "music.h"
#if defined(NN_OVERLAYS) && defined(__riscv)
#include "system.h"
#endif
#ifdef NN_OVERLAYS
#define GAME_ROM __attribute__((section(".rodata.game")))
#define GAME_MENU_DATA __attribute__((section(".ov4.rodata")))
#define GAME_RACE_DATA __attribute__((section(".ov3.rodata")))
#else
#define GAME_ROM
#define GAME_MENU_DATA
#define GAME_RACE_DATA
#endif

Game game;
uint8_t framebuffer[1024] NN_FRAMEBUFFER;
static uint8_t view_player;
static const int8_t dx[16]={64,59,45,24,0,-24,-45,-59,-64,-59,-45,-24,0,24,45,59};
static const int8_t dy[16]={0,24,45,59,64,59,45,24,0,-24,-45,-59,-64,-59,-45,-24};
#include "power_catalog.h"
#define POWER_DURATION(id, duration, hud, label) [id]=duration,
#define POWER_HUD_LABEL(id, duration, hud, label) [id]=hud,
static const uint8_t power_cooldowns[] GAME_ROM={NN_POWER_CATALOG(POWER_DURATION)};
static const char power_hud_labels[][7]={NN_POWER_CATALOG(POWER_HUD_LABEL)};
#undef POWER_DURATION
#undef POWER_HUD_LABEL
_Static_assert(sizeof(power_cooldowns)==NN_FIXED_POWER_COUNT,"Each fixed power needs a catalog definition");
_Static_assert(NITRO==0&&CANNON==1&&NO_POWER==6,"Existing power and empty IDs must remain stable");
// Private calculations use native 32-bit registers; storage and public ABI stay compact.
// Keep byte-angle wrapping and packed temporary arrays explicit.
static int32_t magnitude(int32_t x) {return x<0?-x:x;}
static int32_t next_index(int32_t i) {return i+1==game.route_len?0:i+1;}
static int32_t previous_index(int32_t i) {return i?i-1:game.route_len-1;}
static int32_t center_x(int32_t cell) {return ((cell&15)*ROAD_SIZE+ROAD_SIZE/2)*16;}
static int32_t center_y(int32_t cell) {return ((cell>>4)*ROAD_SIZE+WORLD_TOP+ROAD_SIZE/2)*16;}
uint8_t track_get(uint8_t cell) {
    if(cell>=CELLS)return 0;
    return (game.tiles[cell>>1]>>((cell&1)*4))&15;
}
static int32_t heading_to(int32_t x,int32_t y) {
    int32_t ax=magnitude(x),ay=magnitude(y);int32_t h;
    if(ay*5<ax)h=0;else if(ay*2<ax)h=1;else if(ax*5<ay)h=4;
    else if(ax*2<ay)h=3;else h=2;
    if(x<0)h=8-h;
    if(y<0)h=(16-h)&15;
    return h;
}
static void steer(Car *c,int32_t direction) {c->heading=(c->heading+direction)&15;}
static int32_t cell_at(int32_t x,int32_t y) {
    if(x<0||x>=WORLD_WIDTH*16||y<WORLD_TOP*16||y>=WORLD_BOTTOM*16)return 255;
    return ((y-WORLD_TOP*16)>>8)*16+(x>>8);
}
static int32_t on_road(int32_t x,int32_t y) {return track_get(cell_at(x,y))!=0;}
static int32_t close_to(int32_t x,int32_t y,int32_t xx,int32_t yy,int32_t radius) {
    return magnitude(x-xx)<radius*16&&magnitude(y-yy)<radius*16;
}
static void clear_effects(void) {for(int32_t i=0;i<EFFECT_COUNT;i++)game.effects[i].life=0;}
// Callers supply a nonempty validated human/reward mask.
static int32_t first_player(int32_t mask) {
    int32_t who=0;
    while(!(mask&1)){mask>>=1;who++;}
    return who;
}
NN_OV2 static void start_race(void) {
    clear_effects();game.finish_count=0;game.race_ticks=0;
    for(int32_t i=0;i<CAR_COUNT;i++) {
        Car *c=&game.cars[i];uint8_t *bytes=(uint8_t *)c;
        for(int32_t j=0;j<(int32_t)sizeof(*c);j++)bytes[j]=0;
        c->x=center_x(game.start)-4*16;
        c->y=center_y(game.start)+((int8_t)i*4-6)*16;c->next=0;
        c->heading=0;
        c->powers[0]=game.loadouts[i][0];c->powers[1]=game.loadouts[i][1];
    }
    game.actor=first_player(game.humans);game.repeat=0;
    game.phase=COUNTDOWN;game.ticks=0;
}
NN_OV3_LOCAL static void hit(Car *c) {
    c->stun=20;c->speed=-3;steer(c,2);
}
NN_OV3_LOCAL static void activate(int32_t who,int32_t slot) {
    Car *c=&game.cars[who];int32_t type=c->powers[slot];
    if(type>=NN_FIXED_POWER_COUNT||c->cooldown[slot]||c->finish)return;
    if(type==NITRO)c->boost=35;
    else {
        Effect *e=0;
        for(int32_t i=0;i<EFFECT_COUNT;i++)if(!game.effects[i].life){e=&game.effects[i];break;}
        if(!e)return;
        e->type=CANNON;e->owner=who;e->heading=c->heading;
        e->x=c->x+dx[c->heading];e->y=c->y+dy[c->heading];e->life=45;
    }
    c->cooldown[slot]=power_cooldowns[type];
}
NN_OV3_LOCAL static void update_effects(void) {
    for(int32_t n=0;n<EFFECT_COUNT;n++) {
        NN_MUSIC_POLL();
        Effect *e=&game.effects[n];
        if(!e->life)continue;
        if(e->type!=CANNON){e->life=0;continue;}
        e->life--;
        e->x+=dx[e->heading]>>1;e->y+=dy[e->heading]>>1;
        if(e->x<0||e->x>=WORLD_WIDTH*16||e->y<WORLD_TOP*16||e->y>=WORLD_BOTTOM*16){e->life=0;continue;}
        for(int32_t i=0;i<CAR_COUNT;i++) {
            Car *c=&game.cars[i];
            if(i==e->owner||c->finish)continue;
            if(close_to(c->x,c->y,e->x,e->y,3)) {
                hit(c);e->life=0;break;
            }
        }
    }
}
NN_OV3_LOCAL static void update_car(int32_t who,int32_t held,int32_t edges) {
    Car *c=&game.cars[who];
    if(c->finish)return;
    for(int32_t s=0;s<2;s++)if(c->cooldown[s])c->cooldown[s]--;
    if(c->boost)c->boost--;
    if(c->stun)c->stun--;
    int32_t human=(game.humans>>who)&1;
    int32_t turn=0,target=11;
    if(!human) {
        int32_t tx=center_x(game.route[c->next])-c->x,ty=center_y(game.route[c->next])-c->y;
        if(c->sectors<=1&&c->next<=2)ty+=((int8_t)who*4-6)*16;
        int32_t diff=((heading_to(tx,ty)-c->heading+8)&15)-8;
        turn=diff>0?1:diff<0?-1:0;
        if(magnitude(diff)>2)target=6;
        if(((game.race_ticks+who*11)&63)==0)for(int32_t s=0;s<2;s++) {
            int32_t type=c->powers[s];
            if(type>=NN_FIXED_POWER_COUNT)continue;
            int32_t use=type==NITRO&&diff==0;
            for(int32_t j=0;j<CAR_COUNT;j++)if(j!=who&&!game.cars[j].finish&&
                close_to(c->x,c->y,game.cars[j].x,game.cars[j].y,20))use=1;
            if(use&&!c->cooldown[s]){activate(who,s);break;}
        }
    } else {
        turn=((held&RIGHT)?1:0)-((held&LEFT)?1:0);
        if(held&DOWN)target=-6;
        if(edges&ACT)activate(who,0);
        if(edges&BACK)activate(who,1);
    }
    if(turn&&(game.race_ticks&1)==0)steer(c,turn);
    if(c->boost>6&&target>0)target=18;
    if(c->stun)target=2;
    if(!on_road(c->x,c->y)&&target>4)target=4;
    if(c->speed<target)c->speed++;
    if(c->speed>target)c->speed--;
    int32_t nx=c->x+((dx[c->heading]*c->speed)>>6),ny=c->y+((dy[c->heading]*c->speed)>>6);
    if(nx<32)nx=32;
    if(nx>WORLD_WIDTH*16-33)nx=WORLD_WIDTH*16-33;
    if(ny<160)ny=160;
    if(ny>WORLD_BOTTOM*16-33)ny=WORLD_BOTTOM*16-33;
    c->x=nx;c->y=ny;
}
// Lap sectors are independent of road membership. The gate spans the road and
// a full road-width shoulder on each side; all crossings use swept positions.
NN_OV3_LOCAL static int32_t crossed_gate(int32_t station,int32_t x,int32_t y,const Car *c) {
    int32_t previous=previous_index(station),cell=game.route[station];
    int32_t direction=(int32_t)cell-game.route[previous];
    int32_t plane,from,to,side,delta,center;
    if(direction==1||direction==-1) {
        plane=center_x(cell);from=x;to=c->x;side=y;delta=c->y-y;center=center_y(cell);
    } else {
        plane=center_y(cell);from=y;to=c->y;side=x;delta=c->x-x;center=center_x(cell);
    }
    if(direction>0?!(from<plane&&to>=plane):!(from>plane&&to<=plane))return 0;
    int32_t at=side+((int32_t)delta*(plane-from))/(to-from);
    return magnitude(at-center)<=ROAD_SIZE*24;
}
NN_OV3_LOCAL static void credit_route(int32_t who,int32_t old_x,int32_t old_y) {
    Car *c=&game.cars[who];
    if(c->finish)return;
    int32_t nearest=0;int32_t distance=65535;
    for(int32_t i=0;i<game.route_len;i++) {
        int32_t d=magnitude(center_x(game.route[i])-c->x)+magnitude(center_y(game.route[i])-c->y);
        if(d<distance){distance=d;nearest=i;}
    }
    int32_t next=next_index(nearest);
    if(next!=c->next)c->stuck=0;else if(!(game.humans&(1<<who)))c->stuck++;
    c->next=next;
    if(crossed_gate(game.gates[c->sectors&3],old_x,old_y,c))c->sectors++;
    int32_t previous=game.gates[(c->sectors+3)&3];
    int32_t offset=(nearest+game.route_len-previous)%game.route_len;
    int32_t span=(game.gates[c->sectors&3]+game.route_len-previous)%game.route_len;
    if(offset>=span)offset=0;
    c->steps=(uint16_t)c->sectors*ROUTE_MAX+offset;
    if(!(game.humans&(1<<who))&&c->stuck>180) {
        c->x=center_x(game.route[nearest]);c->y=center_y(game.route[nearest]);
        c->heading=heading_to(center_x(game.route[c->next])-c->x,center_y(game.route[c->next])-c->y);
        c->speed=0;c->stuck=0;
    }
    if(c->sectors==1+4*LAPS) {
        c->finish=++game.finish_count;game.order[c->finish-1]=who;
    }
}
NN_OV3_LOCAL static void finish_round(void) {
    int32_t finished=game.finish_count;
    while(game.finish_count<CAR_COUNT) {
        int32_t best=255;
        for(int32_t i=0;i<CAR_COUNT;i++)if(!game.cars[i].finish&&
            (best==255||game.cars[i].steps>game.cars[best].steps))best=i;
        game.order[game.finish_count++]=best;game.cars[best].finish=game.finish_count;
    }
    clear_effects();game.ticks=0;game.winner=finished?game.order[0]:255;
    game.phase=ROUND_RESULTS;
    game.actor=first_player(game.humans);game.rewards=0;
    if(!finished)return;
    game.round++;game.wins[game.winner]++;
    if(game.humans&(1<<game.winner))game.actor=game.winner;
    if(game.wins[game.winner]>=MATCH_WINS){game.phase=MATCH_OVER;return;}
}
NN_OV3 void game_race_tick(const uint8_t held[CAR_COUNT],const uint8_t edges[CAR_COUNT]) {
    int16_t old_x[CAR_COUNT],old_y[CAR_COUNT];game.race_ticks++;
    for(int32_t i=0;i<CAR_COUNT;i++){old_x[i]=game.cars[i].x;old_y[i]=game.cars[i].y;}
    for(int32_t i=0;i<CAR_COUNT;i++){
        NN_MUSIC_POLL();update_car(i,held[i],edges[i]);
    }
    for(int32_t i=0;i<CAR_COUNT;i++)for(int32_t j=i+1;j<CAR_COUNT;j++) {
        Car *a=&game.cars[i],*b=&game.cars[j];
        if(a->finish||b->finish)continue;
        if(close_to(a->x,a->y,b->x,b->y,4)) {
            int32_t px=a->x>=b->x?8:-8,py=a->y>=b->y?8:-8;
            a->x+=px;b->x-=px;a->y+=py;b->y-=py;
            if(a->speed>6)a->speed--;
            if(b->speed>6)b->speed--;
        }
    }
    update_effects();
    for(int32_t i=0;i<CAR_COUNT;i++) {
        credit_route(i,old_x[i],old_y[i]);
    }
    if(game.finish_count||game.race_ticks>=5400)finish_round();
}
NN_OV2 void game_init_seed(uint16_t seed) {
    game_boot();game.seed=seed?seed:1;track_generate(game.seed);
}
NN_OV2 void game_init(void) {view_player=0;game_init_seed(1);}
NN_OV2 uint8_t game_begin(uint16_t seed,uint8_t humans) {
    if(!humans||(humans&~15))return 0;
    game_init_seed(seed);game.humans=humans;start_race();return 1;
}
void game_set_view(uint8_t player) {
    if(player>=CAR_COUNT)return;
    view_player=player;
}
void game_tick(uint8_t held) {
    uint8_t controls[CAR_COUNT]={0};controls[view_player]=held;game_step(controls);
}
// This setup entry owns match/round course generation and race-grid resets.
NN_OV2 void game_prepare_race(void) {
    if(game.phase==MATCH_SETUP){uint8_t humans=game.humans;game_init_seed(game.seed);game.humans=humans;}
    else {
        uint8_t previous_column=game.start&15;
        game.seed+=0x9e37;
        if(!game.seed)game.seed=1;
        // The first track PRNG bit owns the left boundary. Flipping seed bit0
        // flips that boundary, so at most two generations ensure a new course.
        for(uint8_t attempt=0;attempt<2;attempt++) {
            if(attempt){game.seed^=1;if(!game.seed)game.seed=2;}
            for(uint8_t i=0;i<sizeof(game.tiles);i++)game.tiles[i]=0;
            for(uint8_t i=0;i<sizeof(game.route);i++)game.route[i]=0;
            track_generate(game.seed);
            if((game.start&15)!=previous_column)break;
        }
    }
    start_race();
}
_Static_assert(NN_OVERLAY_SETUP==NN_OVERLAY_GATE,"First START reuses the verified startup bank");
uint8_t game_required_group(void) {
    if(game.phase==RACE_SETUP||game.phase==MATCH_SETUP)return NN_OVERLAY_SETUP;
    return game.phase==COUNTDOWN||game.phase==RACE?NN_OVERLAY_RACE:NN_OVERLAY_CHOICES;
}

NN_OV4 void game_menu_tick(uint8_t action) {
    if(game.phase==TITLE) {
        game.ticks++;
        if(action&ACT){game.seed+=game.ticks;game.phase=MATCH_SETUP;}
        return;
    }
    if(game.phase==MATCH_OVER) {
        if(action&ACT){game.seed+=game.race_ticks+1;game.phase=MATCH_SETUP;}
        return;
    }
    if(game.phase==ROUND_RESULTS) {
        if(action&ACT){game.error=0;game.phase=RACE_SETUP;}
    }
}
// Input history and phase requests stay resident across every overlay replacement.
__attribute__((noinline)) void game_step(const uint8_t controls[CAR_COUNT]) {
    uint8_t held[CAR_COUNT],edges[CAR_COUNT];
    for(int32_t who=0;who<CAR_COUNT;who++) {
        int32_t input=(game.humans&(1<<who))?controls[who]:0,bit=1<<who;
        if(input&ACT)game.action_lock|=bit;
        if(!(input&(ACT|BACK)))game.action_lock&=~bit;
        if(game.action_lock&bit)input&=~BACK;
        held[who]=input;edges[who]=input&~game.previous[who];game.previous[who]=input;
    }
    if(game.phase==RACE_SETUP||game.phase==MATCH_SETUP){game_prepare_race();return;}
    if(game.phase==RACE){game_race_tick(held,edges);return;}
    if(game.phase==COUNTDOWN){if(++game.ticks>=90){game.phase=RACE;game.ticks=0;}return;}
    game_menu_tick(edges[game.actor]);
}
static const uint8_t font[38][3]={
{30,5,30},{31,21,10},{14,17,17},{31,17,14},{31,21,17},{31,5,1},{14,17,29},
{31,4,31},{17,31,17},{8,16,15},{31,4,27},{31,16,16},{31,2,31},{31,1,30},
{14,17,14},{31,5,2},{14,25,30},{31,13,18},{18,21,9},{1,31,1},{15,16,15},
{7,24,7},{31,8,31},{27,4,27},{3,28,3},{25,21,19},
{31,17,31},{0,31,0},{29,21,23},{21,21,31},{7,4,31},{23,21,29},{31,21,29},
{1,1,31},{31,21,31},{23,21,31},{0,10,0},{4,4,4}};
static void pixel(int32_t x,int32_t y,int32_t white) {
    if(x<0||x>127||y<0||y>63)return;
    int32_t index=(y>>3)*128+x;int32_t mask=1<<(y&7);
    if(white)framebuffer[index]|=mask;else framebuffer[index]&=~mask;
}
static void box(int32_t x,int32_t y,int32_t w,int32_t h,int32_t fill) {
    for(int32_t yy=0;yy<h;yy++) {
        for(int32_t xx=0;xx<w;xx++) {
            if(!(xx&15))NN_MUSIC_POLL();
            if(fill||!xx||!yy||xx==w-1||yy==h-1)pixel(x+xx,y+yy,fill>=0);
        }
    }
}
__attribute__((noinline)) static void glyph(int32_t x,int32_t y,int32_t index) {
    for(int32_t xx=0;xx<3;xx++)for(int32_t yy=0;yy<5;yy++)if(font[index][xx]&(1<<yy))
        pixel(x+xx,y+yy,1);
}
static void text(int32_t x,int32_t y,const char *s) {
    for(;*s;s++,x+=4) {
        NN_MUSIC_POLL();
        int32_t c=*s,index;
        if(c>='A'&&c<='Z')index=c-'A';else if(c>='0'&&c<='9')index=c-'0'+26;
        else if(c==':')index=36;else if(c=='-')index=37;else continue;
        glyph(x,y,index);
    }
}
// Magnified title words contain uppercase letters only.
NN_OV4_LOCAL static void big_text(int32_t x,int32_t y,const char *s) {
    for(;*s;s++,x+=8) {
        int32_t index=*s-'A';
        for(int32_t xx=0;xx<3;xx++)for(int32_t yy=0;yy<5;yy++)if(font[index][xx]&(1<<yy))
            box(x+xx*2,y+yy*2,2,2,1);
    }
}
// Car identities, lap/countdown values and first-to-three scores are single digits.
_Static_assert(CAR_COUNT<10&&LAPS<10&&MATCH_WINS<10,"HUD counters must fit one digit");
static void number(int32_t x,int32_t y,int32_t value) {
    glyph(x,y,value+26);
}

#define REAR_CAR_ROWS {0x07e0,0x0ff0,0x0810,0x0810,0x1ff8,0x3ffc,0xdffb,0xd3cb,0xffff,0xdffb,0xdffb,0x1ff8}
static const uint16_t rear_car[12] GAME_RACE_DATA=REAR_CAR_ROWS;
static const uint16_t title_car[12] GAME_MENU_DATA=REAR_CAR_ROWS;
#undef REAR_CAR_ROWS
static const uint16_t front_car[12] GAME_RACE_DATA={0x07e0,0x0ff0,0x0810,0x0810,0x1ff8,0x3ffc,0xdbdb,0xd81b,0xdffb,0xdbdb,0xd81b,0x1ff8};
static void car_draw(int32_t x,int32_t foot,int32_t width,int32_t who){
    int32_t relative=(game.cars[who].heading-game.cars[view_player].heading)&15;
    int32_t h=game.phase==TITLE?width*2/3:width*3/4;if(h<3)h=3;
    const uint16_t *sprite=game.phase==TITLE?title_car:relative>=6&&relative<=10?front_car:rear_car;
    int32_t left=x-width/2,top=foot-h,row=0,row_error=0;
    // Heading-relative 16x12 art keeps body, glass, wheels and lights distinct.
    for(int32_t y=0;y<h;y++) {
        NN_MUSIC_POLL();
        int32_t column=0,column_error=0;
        for(int32_t xx=0;xx<width;xx++) {
            pixel(left+xx,top+y,(sprite[row]>>column)&1);
            column_error+=16;
            while(column_error>=width){column_error-=width;column++;}
        }
        row_error+=12;
        while(row_error>=h){row_error-=h;row++;}
    }
    if(width>=9) {
        if(game.phase!=TITLE&&top>=26)number(x-1,top-7,who+1);
        Car *car=&game.cars[who];
        if(game.phase!=TITLE){
            box(x,top-2,1,3,1);
            if(car->cooldown[1]>86)box(x-1,top-4,3,2,1);
        }
        if(car->boost){
            int32_t flame=3+(game.race_ticks&1);
            box(x-4,foot,2,flame,1);box(x+3,foot,2,flame,1);
        }
    }
}
NN_OV3 static void effect_draw(int32_t x,int32_t foot,int32_t width,int32_t type) {
    (void)width;(void)type;
    box(x-1,foot-5,3,3,1);
}
NN_OV3 static void power_hud(void) {
    // Distant treetops and opponent labels must not cross the power row.
    box(0,9,128,7,-1);
    Car *car=&game.cars[view_player];
    for(int32_t slot=0;slot<2;slot++) {
        int32_t x=slot*64,type=car->powers[slot];
        text(x,10,slot?"B":"A");
        text(x+8,10,type<NN_FIXED_POWER_COUNT?power_hud_labels[type]:"EMPTY");
        if(type<NN_FIXED_POWER_COUNT) {
            box(x+49,10,13,5,0);
            box(x+50,11,11-(car->cooldown[slot]*11)/power_cooldowns[type],3,1);
        }
    }
}
// Scenery is derived from the existing course, without consuming simulation RNG.
// Trees never occupy a road cell and draw behind cars, leaving the center clear.
NN_OV3 static void scenery_draw(int32_t camera_x,int32_t camera_y,int32_t c,int32_t s) {
    for(int32_t i=0;i<game.route_len;i+=2) {
        NN_MUSIC_POLL();
        int32_t cell=game.route[i],horizontal=track_get(cell)&(LEFT|RIGHT);
        int32_t side=(i&2)?-1:1;
        {
            int32_t x=center_x(cell)+(horizontal?0:side*ROAD_SIZE*16);
            int32_t y=center_y(cell)+(horizontal?side*ROAD_SIZE*16:0);
            if(on_road(x,y))continue;
            int32_t xx=x-camera_x,yy=y-camera_y;
            int32_t depth=(xx*c+yy*s)>>6;
            if(depth<80)continue;
            int32_t offset=(-xx*s+yy*c)>>6;
            int32_t screen=64+offset*64/depth,foot=18+4096/depth;
            if(screen<0||screen>127||(screen>40&&screen<88)||foot<24||foot>62)continue;
            int32_t width=2048/depth;if(width<2)width=2;if(width>12)width=12;
            int32_t height=width*2,top=foot-height;
            box(screen-width/2-1,top-1,width+2,height+2,-1);
            box(screen,foot-width/2,1,width/2,1);
            for(int32_t row=0;row<width;row++) {
                int32_t span=1+row;
                box(screen-span/2,top+row,span,1,1);
            }
            box(screen-width/2,top+width,width,2,1);
        }
    }
}
NN_OV3 static void chase_draw(void){
    Car *player=&game.cars[view_player];
    int32_t c=dx[player->heading],s=dy[player->heading];
    int32_t camera_x=player->x-((c*8)>>2),camera_y=player->y-((s*8)>>2);
    int32_t offset=game.phase==COUNTDOWN?(3-(int32_t)view_player*2)*32:0;
    camera_x-=(s*offset)>>6;camera_y+=(c*offset)>>6;
    for(int32_t y=21;y<64;y++){
        NN_MUSIC_POLL();
        int32_t depth=65536/(y-18);
        int32_t wx=camera_x*16+(((c+s)*depth)>>6),wy=camera_y*16+(((s-c)*depth)>>6);
        int32_t sx=(-s*depth)>>11,sy=(c*depth)>>11;
        for(int32_t x=0;x<128;x+=2,wx+=sx,wy+=sy){
            int32_t tile=0,cell=255,xx=wx>>8,yy=wy>>8;
            if(xx>=0&&xx<WORLD_WIDTH&&yy>=WORLD_TOP&&yy<WORLD_BOTTOM){cell=((yy-WORLD_TOP)>>4)*16+(xx>>4);tile=track_get(cell);}
            // Sparse world-space grain leaves scenery and the road silhouette clear.
            int32_t ink=((xx^(yy*4))&63)==0;
            if(tile) {
                int32_t lx=wx&4095,ly=(wy-WORLD_TOP*256)&4095;
                // The existing connection bits identify genuine outer edges.
                int32_t boundary=(lx<256?LEFT:lx>=3840?RIGHT:0)|
                                 (ly<256?UP:ly>=3840?DOWN:0);
                ink=boundary&~tile;
                int32_t line=tile==(LEFT|RIGHT)?ly:tile==(UP|DOWN)?lx:-1;
                if(line>=1984&&line<2112&&((tile&LEFT?wx:wy)&1024))ink=1;
            }
            if(tile&&cell==game.start&&((wx&4095)>=1920&&(wx&4095)<2176))ink=((wy>>7)^(wx>>7))&1;
            if(ink){int32_t at=(y>>3)*128+x,bit=1<<(y&7);framebuffer[at]|=bit;framebuffer[at+1]|=bit;}
        }
    }
    scenery_draw(camera_x,camera_y,c,s);
    uint16_t drawn=1<<view_player;
    for(int32_t layer=0;layer<CAR_COUNT+EFFECT_COUNT-1;layer++){
        int32_t best=-1,depth=48;
        for(int32_t i=0;i<CAR_COUNT+EFFECT_COUNT;i++)if(!(drawn&(1<<i))){
            if(i>=CAR_COUNT&&!game.effects[i-CAR_COUNT].life)continue;
            int32_t xx=(i<CAR_COUNT?game.cars[i].x:game.effects[i-CAR_COUNT].x)-camera_x;
            int32_t yy=(i<CAR_COUNT?game.cars[i].y:game.effects[i-CAR_COUNT].y)-camera_y;
            int32_t d=(xx*c+yy*s)>>6;if(d>depth){depth=d;best=i;}
        }
        if(best<0)break;
        drawn|=1<<best;
        int32_t xx=(best<CAR_COUNT?game.cars[best].x:game.effects[best-CAR_COUNT].x)-camera_x;
        int32_t yy=(best<CAR_COUNT?game.cars[best].y:game.effects[best-CAR_COUNT].y)-camera_y;
        int32_t side=(-xx*s+yy*c)>>6,width=3072/depth;if(width>24)width=24;if(width<3)width=3;
        if(best<CAR_COUNT)car_draw(64+side*64/depth,18+4096/depth,width,best);
        else effect_draw(64+side*64/depth,18+4096/depth,width,game.effects[best-CAR_COUNT].type);
    }
    car_draw(64-(offset>>1),(game.phase==COUNTDOWN?50:59),24,view_player);
    text(0,1,"LAP");number(16,1,player->sectors?1+((player->sectors-1)>>2):1);
    text(42,1,"P");number(47,1,view_player+1);text(91,1,"WINS");number(113,1,game.wins[view_player]);
    power_hud();
    if(game.phase==RACE&&player->stun) {
        box(49,18,30,9,-1);box(49,18,30,9,0);
        text(58,20,"HIT");
    }
    if(game.phase==COUNTDOWN)number(62,25,3-game.ticks/30);
}
void game_draw_lobby(uint8_t count,uint8_t ready,uint8_t self){
    for(int32_t i=0;i<1024;i++)framebuffer[i]=0;
    text(24,2,"LINK LOBBY");
    for(int32_t i=0;i<count;i++){
        int32_t y=14+i*9;text(8,y,"P");number(13,y,i+1);
        if(i==self)text(28,y,"YOU");
        text(69,y,(ready&(1<<i))?"READY":"WAIT");
    }
    text(4,56,(ready&(1<<self))?"WAITING FOR PLAYERS":"A JOIN AND READY");
}
static const char menu_wait[] GAME_MENU_DATA="WAIT FOR PLAYER";
static const char menu_nano[] GAME_MENU_DATA="NANO";
static const char menu_nitro[] GAME_MENU_DATA="NITRO";
static const char menu_nonsense[] GAME_MENU_DATA="NONSENSE";
static const char menu_controls[] GAME_MENU_DATA="AUTO DRIVE DOWN REVERSE";
static const char menu_start[] GAME_MENU_DATA="A START  B HELP";
static const char menu_champion[] GAME_MENU_DATA="MATCH CHAMPION";
static const char menu_results[] GAME_MENU_DATA="ROUND PODIUM";
static const char menu_last_race[] GAME_MENU_DATA="LAST RACE";
static const char menu_winner[] GAME_MENU_DATA="WINNER P";
static const char menu_no_finish[] GAME_MENU_DATA="NO FINISH";
static const char menu_of[] GAME_MENU_DATA="OF 3";
static const char menu_continue[] GAME_MENU_DATA="A CONTINUE";
static const char menu_new_match[] GAME_MENU_DATA="A NEW MATCH";
void game_draw_loading(uint8_t error) {
    for(int32_t i=0;i<1024;i++)framebuffer[i]=0;
    text(38,24,error?"LOAD FAILED":"LOADING");
    text(38,36,error?"TRY RESTART":"PLEASE WAIT");
}
static const char menu_help[] GAME_MENU_DATA=
    "LINK 2-4 PLATABOYS\0"
    "OWN POWER ON EACH\0"
    "GND TO GND\0"
    "SWIO TO SWIO IN PARALLEL\0"
    "A IN EACH LOBBY TO READY\0"
    "ALL READY START BOTS FILL 4\0"
    "A NITRO B CANNON\0"
    "A START B BACK\0";
NN_OV4 void game_draw_help(void) {
    for(int32_t i=0;i<1024;i++)framebuffer[i]=0;
    int32_t y=0;
    for(const char *line=menu_help;*line;y+=8) {
        text(0,y,line);
        while(*line++);
    }
}
// Visual time never advances the simulation or consumes the course PRNG.
NN_OV4_LOCAL static void fireworks_draw(uint8_t tick) {
    for(int32_t burst=0;burst<2;burst++) {
        int32_t phase=(tick+burst*7)&15;
        if(phase>12)continue;
        int32_t x=10+burst*108,y=27+((tick>>4)&1)*5,radius=1+(phase>>1);
        if(phase<5)pixel(x,y,1);
        for(int32_t ray=0;ray<16;ray+=2) {
            pixel(x+((dx[ray]*radius)>>6),y+((dy[ray]*radius)>>6),1);
        }
    }
}
NN_OV4 static void menu_draw(void){
    if(game.phase==TITLE) {
        text(56,1,menu_nano);big_text(45,10,menu_nitro);big_text(33,22,menu_nonsense);
        for(int32_t row=0;row<3;row++)for(int32_t col=0;col<3;col++)if(!((row+col)&1)) {
            box(24+col*3,10+row*3,3,3,1);box(96+col*3,10+row*3,3,3,1);
        }
        box(23,10,1,12,1);box(105,10,1,12,1);
        for(int32_t line=0;line<3;line++) {
            box(9+line*5,36+line*5,30-line*5,1,1);
            box(89,36+line*5,30-line*5,1,1);
        }
        car_draw(64,51,28,view_player);
        text(20,52,menu_controls);text(34,59,menu_start);return;
    }
    uint8_t visual_tick=0;
#if defined(NN_OVERLAYS) && defined(__riscv)
    visual_tick=STK->CNT>>22;
#endif
    if(game.phase==MATCH_OVER&&game.winner<CAR_COUNT)fireworks_draw(visual_tick);
    text(game.phase==MATCH_OVER?36:40,1,game.phase==MATCH_OVER?menu_champion:menu_results);
    if(game.winner<CAR_COUNT){text(45,9,menu_winner);number(77,9,game.winner+1);}
    else text(46,9,menu_no_finish);
    box(22,15,84,34,0);
    for(int32_t i=0;i<CAR_COUNT;i++){
        int32_t who=game.order[i];
        number(28,17+i*8,i+1);text(44,17+i*8,"P");number(49,17+i*8,who+1);
        number(76,17+i*8,game.wins[who]);text(84,17+i*8,menu_of);
    }
    if(game.phase==MATCH_OVER)text(45,50,menu_last_race);
    if(game.actor!=view_player)text(34,57,menu_wait);
    else if(game.phase==MATCH_OVER)text(42,57,menu_new_match);
    else text(44,57,menu_continue);
}
void game_draw(void){
    if(game.phase==RACE_SETUP||game.phase==MATCH_SETUP){game_draw_loading(0);return;}
    for(int32_t i=0;i<1024;i++)framebuffer[i]=0;
    if(game.phase==RACE||game.phase==COUNTDOWN)chase_draw();
    else menu_draw();
}
