#ifdef NN_MINIMAL
#include "minimal_game.inc"
#else
#include "game.h"

Game game;
uint8_t framebuffer[1024];
static uint8_t camera_angle,rear_flash,rear_type,view_player;
static const int8_t dx[16]={64,59,45,24,0,-24,-45,-59,-64,-59,-45,-24,0,24,45,59};
static const int8_t dy[16]={0,24,45,59,64,59,45,24,0,-24,-45,-59,-64,-59,-45,-24};
static const char *const power_names[]={"NITRO","CANNON","OIL","MINE","SHIELD","SHOCKWAVE","EMPTY"};
// Private calculations use native 32-bit registers; storage and public ABI stay compact.
// Keep byte-angle wrapping and packed temporary arrays explicit.
static const uint8_t durations[]={120,90,135,150,150,120};
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
static void start_race(void) {
    clear_effects();game.finish_count=0;game.race_ticks=0;
    for(int32_t i=0;i<CAR_COUNT;i++) {
        Car *c=&game.cars[i];uint8_t *bytes=(uint8_t *)c;
        for(int32_t j=0;j<(int32_t)sizeof(*c);j++)bytes[j]=0;
        c->x=center_x(game.start)-4*16;
        c->y=center_y(game.start)+((int8_t)i*4-6)*16;c->next=0;
        c->heading=0;
        c->powers[0]=game.loadouts[i][0];c->powers[1]=game.loadouts[i][1];
    }
    camera_angle=game.cars[view_player].heading<<4;rear_flash=0;
    game.actor=first_player(game.humans);game.repeat=0;
    game.phase=COUNTDOWN;game.ticks=0;
}
static void hit(Car *c,int32_t type) {
    if(c->shield){c->shield=0;return;}
    if(type==OIL)c->slip=45;else{c->stun=20;c->speed=-3;steer(c,2);}
}
static void activate(int32_t who,int32_t slot) {
    Car *c=&game.cars[who];int32_t type=c->powers[slot];
    if(type>=POWER_COUNT||c->cooldown[slot]||c->finish)return;
    if(type==NITRO)c->boost=35;
    else if(type==SHIELD)c->shield=100;
    else if(type==WAVE) {
        for(int32_t i=0;i<CAR_COUNT;i++)if(i!=who&&close_to(c->x,c->y,game.cars[i].x,game.cars[i].y,13)) {
            Car *other=&game.cars[i];int32_t blocked=other->shield;hit(other,WAVE);
            if(!blocked){other->x+=other->x>=c->x?24:-24;other->y+=other->y>=c->y?24:-24;}
        }
        c->wave=8;
    } else {
        Effect *e=0;
        for(int32_t i=0;i<EFFECT_COUNT;i++)if(!game.effects[i].life){e=&game.effects[i];break;}
        if(!e)return;
        e->type=type;e->owner=who;e->heading=c->heading;e->x=c->x;e->y=c->y;
        e->life=type==CANNON?45:180;
        if(type==CANNON){e->x+=dx[c->heading];e->y+=dy[c->heading];}
        else{e->x-=dx[c->heading];e->y-=dy[c->heading];}
    }
    if(who==view_player&&(type==OIL||type==MINE)){rear_flash=8;rear_type=type;}
    c->cooldown[slot]=durations[type];
}
static void update_effects(void) {
    for(int32_t n=0;n<EFFECT_COUNT;n++) {
        Effect *e=&game.effects[n];
        if(!e->life)continue;
        e->life--;
        if(e->type==CANNON){e->x+=dx[e->heading]>>1;e->y+=dy[e->heading]>>1;}
        if(e->x<0||e->x>=WORLD_WIDTH*16||e->y<WORLD_TOP*16||e->y>=WORLD_BOTTOM*16){e->life=0;continue;}
        if(e->type==MINE&&e->life>165)continue;
        for(int32_t i=0;i<CAR_COUNT;i++) {
            Car *c=&game.cars[i];
            if(i==e->owner||c->finish)continue;
            if(close_to(c->x,c->y,e->x,e->y,e->type==OIL?4:3)) {
                hit(c,e->type);
                if(e->type!=OIL){e->life=0;break;}
            }
        }
    }
}
static void update_car(int32_t who,int32_t held,int32_t edges) {
    Car *c=&game.cars[who];
    if(c->finish)return;
    for(int32_t s=0;s<2;s++)if(c->cooldown[s])c->cooldown[s]--;
    if(c->boost)c->boost--;
    if(c->shield)c->shield--;
    if(c->wave)c->wave--;
    if(c->stun)c->stun--;
    if(c->slip)c->slip--;
    int32_t human=(game.humans>>who)&1;
    int32_t turn=0,target=human?11:10;
    if(!human) {
        int32_t tx=center_x(game.route[c->next])-c->x,ty=center_y(game.route[c->next])-c->y;
        if(c->sectors<=1&&c->next<=2)ty+=((int8_t)who*4-6)*16;
        int32_t diff=((heading_to(tx,ty)-c->heading+8)&15)-8;
        turn=diff>0?1:diff<0?-1:0;
        if(magnitude(diff)>2)target=6;
        if(((game.race_ticks+who*11)&63)==0)for(int32_t s=0;s<2;s++) {
            int32_t type=c->powers[s];
            if(type>=POWER_COUNT)continue;
            int32_t use=(type==NITRO&&diff==0)||(type==SHIELD&&c->stun);
            for(int32_t j=0;j<CAR_COUNT;j++)if(j!=who&&!game.cars[j].finish&&
                close_to(c->x,c->y,game.cars[j].x,game.cars[j].y,type==WAVE?12:20))use=1;
            if(use&&!c->cooldown[s]){activate(who,s);break;}
        }
    } else {
        turn=((held&RIGHT)?1:0)-((held&LEFT)?1:0);
        if(held&DOWN)target=-6;
        if(edges&ACT)activate(who,0);
        if(edges&BACK)activate(who,1);
    }
    if(turn&&(game.race_ticks&1)==0&&(!c->slip||(game.race_ticks&3)==0))steer(c,turn);
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
static int32_t crossed_gate(int32_t station,int32_t x,int32_t y,const Car *c) {
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
static void credit_route(int32_t who,int32_t old_x,int32_t old_y) {
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
static void grant_bot_power(int32_t who) {
    int32_t slot=game.loadouts[who][0]==NO_POWER?0:game.loadouts[who][1]==NO_POWER?1:(game.round+who)&1;
    int32_t power=(game.round+who)%POWER_COUNT;
    for(int32_t tries=0;tries<POWER_COUNT;tries++,power=(power+1)%POWER_COUNT)
        if(power!=game.loadouts[who][0]&&power!=game.loadouts[who][1])break;
    game.loadouts[who][slot]=power;
}
static void next_reward(void) {
    game.error=0;game.repeat=0;
    if(!game.rewards){start_race();return;}
    game.actor=first_player(game.rewards);game.phase=POWER_SELECT;
    uint8_t *powers=game.loadouts[game.actor];
    game.slot=powers[0]==NO_POWER?0:powers[1]==NO_POWER?1:0;
    game.choice=powers[game.slot^1]==NITRO?CANNON:NITRO;
}
static void finish_round(void) {
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
    game.rewards=game.humans&~(1<<game.winner);
    for(int32_t i=0;i<CAR_COUNT;i++)if(i!=game.winner&&!(game.humans&(1<<i)))grant_bot_power(i);
}
static void race_tick(const uint8_t held[CAR_COUNT],const uint8_t edges[CAR_COUNT]) {
    int16_t old_x[CAR_COUNT],old_y[CAR_COUNT];game.race_ticks++;
    for(int32_t i=0;i<CAR_COUNT;i++){old_x[i]=game.cars[i].x;old_y[i]=game.cars[i].y;}
    for(int32_t i=0;i<CAR_COUNT;i++)update_car(i,held[i],edges[i]);
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
void game_init_seed(uint16_t seed) {
    uint8_t *bytes=(uint8_t *)&game;
    for(int32_t i=0;i<(int32_t)sizeof(game);i++)bytes[i]=0;
    game.seed=seed?seed:1;game.humans=1;
    track_generate(game.seed);
    for(int32_t i=0;i<CAR_COUNT;i++)for(int32_t slot=0;slot<2;slot++)game.loadouts[i][slot]=NO_POWER;
    game.phase=TITLE;
}
void game_init(void) {view_player=0;game_init_seed(1);}
uint8_t game_begin(uint16_t seed,uint8_t humans) {
    if(!humans||(humans&~15))return 0;
    game_init_seed(seed);game.humans=humans;start_race();return 1;
}
void game_set_view(uint8_t player) {
    if(player>=CAR_COUNT)return;
    view_player=player;camera_angle=game.cars[player].heading<<4;rear_flash=0;
}
void game_tick(uint8_t held) {
    uint8_t controls[CAR_COUNT]={0};controls[view_player]=held;game_step(controls);
}
// Keep the shared tick as a callable boundary for link integration and stack budgeting.
__attribute__((noinline)) void game_step(const uint8_t controls[CAR_COUNT]) {
    uint8_t held[CAR_COUNT],edges[CAR_COUNT];
    for(int32_t who=0;who<CAR_COUNT;who++) {
        int32_t input=(game.humans&(1<<who))?controls[who]:0,bit=1<<who;
        if(input&ACT)game.action_lock|=bit;
        if(!(input&(ACT|BACK)))game.action_lock&=~bit;
        if(game.action_lock&bit)input&=~BACK;
        held[who]=input;edges[who]=input&~game.previous[who];game.previous[who]=input;
    }
    int32_t input=held[game.actor],action=edges[game.actor],navigation=action&15;
    if(input&15){if(++game.repeat>=9){navigation=input&15;game.repeat=6;}}else game.repeat=0;
    if(game.phase==TITLE) {
        game.ticks++;
        if(action&ACT) {
            int32_t seed=game.seed+game.ticks;
            game_begin(seed,game.humans);game.previous[game.actor]=ACT;game.action_lock=1<<game.actor;
        }
        return;
    }
    if(game.phase==RACE) {
        race_tick(held,edges);
        if(rear_flash)rear_flash--;
        uint8_t wrapped=(game.cars[view_player].heading<<4)-camera_angle;
        int32_t delta=wrapped>=128?(int32_t)wrapped-256:wrapped;
        if(delta>8)delta=8;
        if(delta<-8)delta=-8;
        camera_angle+=delta;return;
    }
    if(game.phase==COUNTDOWN){if(++game.ticks>=90){game.phase=RACE;game.ticks=0;}return;}
    if(game.phase==MATCH_OVER){if(action&ACT){game_begin(game.seed+game.race_ticks+1,game.humans);}return;}
    if(game.phase==ROUND_RESULTS) {
        if(action&ACT) {
            game.error=0;
            if(game.winner==255)start_race();
            else next_reward();
        }
        return;
    }
    if(game.phase==POWER_SELECT) {
        if(navigation)game.error=0;
        if(navigation&UP){if(game.choice>=2)game.choice-=2;}
        if(navigation&DOWN){if(game.choice<4)game.choice+=2;}
        if(navigation&LEFT)game.choice&=~1;
        if(navigation&RIGHT)game.choice|=1;
        if(action&BACK){game.slot^=1;game.error=0;}
        if(action&ACT) {
            if(game.choice==game.loadouts[game.actor][game.slot^1])game.error=1;
            else{game.loadouts[game.actor][game.slot]=game.choice;game.rewards&=~(1<<game.actor);next_reward();}
        }
    }
}

// Rendering uses the real 128x64 framebuffer, without hidden debug presentation.
static const uint8_t font[36][3]={
{30,5,30},{31,21,10},{14,17,17},{31,17,14},{31,21,17},{31,5,1},{14,17,29},
{31,4,31},{17,31,17},{8,16,15},{31,4,27},{31,16,16},{31,2,31},{31,1,30},
{14,17,14},{31,5,2},{14,25,30},{31,13,18},{18,21,9},{1,31,1},{15,16,15},
{7,24,7},{31,8,31},{27,4,27},{3,28,3},{25,21,19},
{31,17,31},{0,31,0},{29,21,23},{21,21,31},{7,4,31},{23,21,29},{31,21,29},
{1,1,31},{31,21,31},{23,21,31}};
static void pixel(int32_t x,int32_t y,int32_t white) {
    if(x<0||x>127||y<0||y>63)return;
    int32_t index=(y>>3)*128+x;int32_t mask=1<<(y&7);
    if(white)framebuffer[index]|=mask;else framebuffer[index]&=~mask;
}
static void box(int32_t x,int32_t y,int32_t w,int32_t h,int32_t fill) {
    for(int32_t yy=0;yy<h;yy++)for(int32_t xx=0;xx<w;xx++)
        if(fill||!xx||!yy||xx==w-1||yy==h-1)pixel(x+xx,y+yy,1);
}
static void label(int32_t x,int32_t y,const char *s,int32_t scale) {
    for(;*s;s++,x+=4*scale) {
        int32_t c=*s,index;
        if(c>='A'&&c<='Z')index=c-'A';else if(c>='0'&&c<='9')index=c-'0'+26;
        else {if(c==':'){pixel(x+1,y+1,1);pixel(x+1,y+3,1);}if(c=='-')box(x,y+2,3,1,1);continue;}
        for(int32_t xx=0;xx<3;xx++)for(int32_t yy=0;yy<5;yy++)if(font[index][xx]&(1<<yy))
            for(int32_t sy=0;sy<scale;sy++)for(int32_t sx=0;sx<scale;sx++)pixel(x+xx*scale+sx,y+yy*scale+sy,1);
    }
}
static void text(int32_t x,int32_t y,const char *s) {label(x,y,s,1);}
// Compact static captions: x, y, NUL-terminated text; 255 ends the list.
static void captions(const char *record) {
    while((uint8_t)*record!=255) {
        int32_t x=*record++,y=*record++;
        text(x,y,record);
        while(*record++);
    }
}
// Car identities, lap/countdown values and first-to-five scores are single digits.
_Static_assert(CAR_COUNT<10&&LAPS<10&&MATCH_WINS<10,"HUD counters must fit one digit");
static void number(int32_t x,int32_t y,int32_t value) {
    char digit[2]={'0'+value,0};text(x,y,digit);
}

static const uint32_t car_ink[16]={0xff00,0x18180,0x300c0,0x60060,0x7ffe0,0xc0030,0x180018,0x11ff88,0x30000c,0xe00007,0xe700e7,0xe700e7,0xe00007,0x3ffffc,0x1ffff8,0x60060};
static const uint32_t car_mask[16]={0xff00,0x1ff80,0x3ffc0,0x7ffe0,0x7ffe0,0xffff0,0x1ffff8,0x1ffff8,0x3ffffc,0xffffff,0xffffff,0xffffff,0xffffff,0x3ffffc,0x1ffff8,0x7ffe0};

// Ground-plane projection is independent of the simulation and edited map format.
// Angles use 1/256 turns; the shared 16-sector directions are interpolated at Q6.
#define VIEW_HORIZON 18
#define VIEW_FOCAL 64
#define VIEW_HEIGHT 4
#define VIEW_BACK 8
#define VIEW_CAR_WIDTH 3
#define VIEW_NEAR 48
#define FLOOR_DEPTH(row) (VIEW_HEIGHT*VIEW_FOCAL*256UL/(row))
static const uint16_t floor_depth[64]={
    0,0,0,FLOOR_DEPTH(3),FLOOR_DEPTH(4),FLOOR_DEPTH(5),
    FLOOR_DEPTH(6),FLOOR_DEPTH(7),FLOOR_DEPTH(8),FLOOR_DEPTH(9),FLOOR_DEPTH(10),FLOOR_DEPTH(11),
    FLOOR_DEPTH(12),FLOOR_DEPTH(13),FLOOR_DEPTH(14),FLOOR_DEPTH(15),FLOOR_DEPTH(16),FLOOR_DEPTH(17),
    FLOOR_DEPTH(18),FLOOR_DEPTH(19),FLOOR_DEPTH(20),FLOOR_DEPTH(21),FLOOR_DEPTH(22),FLOOR_DEPTH(23),
    FLOOR_DEPTH(24),FLOOR_DEPTH(25),FLOOR_DEPTH(26),FLOOR_DEPTH(27),FLOOR_DEPTH(28),FLOOR_DEPTH(29),
    FLOOR_DEPTH(30),FLOOR_DEPTH(31),FLOOR_DEPTH(32),FLOOR_DEPTH(33),FLOOR_DEPTH(34),FLOOR_DEPTH(35),
    FLOOR_DEPTH(36),FLOOR_DEPTH(37),FLOOR_DEPTH(38),FLOOR_DEPTH(39),FLOOR_DEPTH(40),FLOOR_DEPTH(41),
    FLOOR_DEPTH(42),FLOOR_DEPTH(43),FLOOR_DEPTH(44),FLOOR_DEPTH(45),FLOOR_DEPTH(46),FLOOR_DEPTH(47),
    FLOOR_DEPTH(48),FLOOR_DEPTH(49),FLOOR_DEPTH(50),FLOOR_DEPTH(51),FLOOR_DEPTH(52),FLOOR_DEPTH(53),
    FLOOR_DEPTH(54),FLOOR_DEPTH(55),FLOOR_DEPTH(56),FLOOR_DEPTH(57),FLOOR_DEPTH(58),FLOOR_DEPTH(59),
    FLOOR_DEPTH(60),FLOOR_DEPTH(61),FLOOR_DEPTH(62),FLOOR_DEPTH(63),
};

typedef struct {
    int16_t side, depth;
    int32_t index, kind;
} ViewObject;

static void view_line(int32_t x,int32_t y,int32_t xx,int32_t yy) {
    int32_t ax=magnitude(xx-x),ay=-magnitude(yy-y),error=ax+ay;
    int32_t sx=x<xx?1:-1,sy=y<yy?1:-1;
    // Callers only use bounded screen-sized lines, including clipped sprites.
    while(1) {
        pixel(x,y,1);
        if(x==xx&&y==yy)break;
        int32_t twice=error*2;
        if(twice>=ay){error+=ay;x+=sx;}
        if(twice<=ax){error+=ax;y+=sy;}
    }
}
static void view_ring(int32_t x,int32_t y,int32_t radius) {
    for(int32_t i=0;i<16;i++) {
        int32_t j=(i+1)&15;
        view_line(x+((dx[i]*radius)>>6),y+((dy[i]*radius)>>8),
                  x+((dx[j]*radius)>>6),y+((dy[j]*radius)>>8));
    }
}
static void view_car(int32_t x,int32_t foot,int32_t width,int32_t who,int32_t lean) {
    int32_t height=(width*2)/3;
    if(height<3)height=3;
    if(x+width/2<0||x-width/2>127||foot<9||foot-height>63)return;
    int32_t ustep=(24U<<8)/width,vstep=(16U<<8)/height;
    int32_t v=0;
    for(int32_t y=0;y<height;y++,v+=vstep) {
        int32_t row=v>>8;
        int32_t left=x-width/2+(row<8?lean:0);
        int32_t u=0;
        for(int32_t column=0;column<width;column++,u+=ustep) {
            int32_t source=u>>8;
            if(!(car_mask[row]&(1UL<<source)))continue;
            int32_t ink=(car_ink[row]>>source)&1;
            if(who&&row>=8&&row<13&&source>5&&source<18)
                ink=((source+row+who)&3)==0;
            pixel(left+column,foot-height+1+y,ink);
        }
    }
}
static void view_power(int32_t x,int32_t foot,int32_t width,int32_t type) {
    if(type==CANNON) {
        int32_t h=width/2;
        if(h<3)h=3;
        box(x-1,foot-h,h>6?3:2,h,1);
    } else if(type==OIL) {
        int32_t h=width/4+1;
        for(int32_t y=-(int8_t)h;y<=0;y++)for(int32_t xx=-(int8_t)(width/2);xx<=(int8_t)(width/2);xx++)
            if(magnitude(xx)+magnitude(y+(h>>1))<width/2)
                pixel(x+xx,foot+y,((xx-y)&3)!=0);
    } else {
        int32_t r=width/4;
        if(r<2)r=2;
        box(x-r,foot-r*2,r*2+1,r*2+1,0);
        pixel(x,foot-r,1);pixel(x+1,foot-r*2-2,1);
    }
}
static int32_t ground_ink(int32_t x,int32_t y,int32_t horizontal_start) {
    int32_t wx=x>>8,wy=y>>8;
    int32_t tile=0,cell=255;
    if(wx>=0&&wx<WORLD_WIDTH&&wy>=WORLD_TOP&&wy<WORLD_BOTTOM) {
        cell=((wy-WORLD_TOP)>>4)*16+(wx>>4);
        tile=track_get(cell);
    }
    if(!tile)return (((x>>7)^(y>>7))&7)==0;
    int32_t lx=x&(ROAD_SIZE*256-1),ly=(y-WORLD_TOP*256)&(ROAD_SIZE*256-1);
    // Road edges use the exact connectivity owned by the fixed circuit.
    if((!(tile&LEFT)&&lx<96)||(!(tile&RIGHT)&&lx>=ROAD_SIZE*256-96)||
       (!(tile&UP)&&ly<96)||(!(tile&DOWN)&&ly>=ROAD_SIZE*256-96))return 1;
    if(cell==game.start) {
        int32_t along=horizontal_start?lx:ly,across=horizontal_start?ly:lx;
        if(along>=ROAD_SIZE*128-128&&along<ROAD_SIZE*128+128)return ((across>>7)^(along>>7))&1;
    }
    int32_t center=((lx>=ROAD_SIZE*128-48&&lx<ROAD_SIZE*128+48)&&((ly<ROAD_SIZE*128&&(tile&UP))||(ly>=ROAD_SIZE*128&&(tile&DOWN))))||
                   ((ly>=ROAD_SIZE*128-48&&ly<ROAD_SIZE*128+48)&&((lx<ROAD_SIZE*128&&(tile&LEFT))||(lx>=ROAD_SIZE*128&&(tile&RIGHT))));
    return center&&(((x+y)>>9)&1);
}
// The flat traversal order is also the stable tie-breaker for equal depths.
static int32_t view_world_object(int32_t flat,ViewObject *object) {
    int32_t kind,index;int32_t x,y;
    if(flat<CAR_COUNT) {
        if(flat==view_player)return 0;
        kind=1;index=flat;x=game.cars[index].x;y=game.cars[index].y;
    } else if(flat<CAR_COUNT+EFFECT_COUNT) {
        kind=0;index=flat-CAR_COUNT;
        if(!game.effects[index].life)return 0;
        x=game.effects[index].x;y=game.effects[index].y;
    } else return 0;
    object->side=x;object->depth=y;object->index=index;object->kind=kind;return 1;
}
static void chase_draw(void) {
    Car *player=&game.cars[view_player];
    int32_t sector=camera_angle>>4,part=camera_angle&15,next=(sector+1)&15;
    int32_t c=dx[sector]+(((dx[next]-dx[sector])*part)>>4);
    int32_t s=dy[sector]+(((dy[next]-dy[sector])*part)>>4);
    int32_t camera_x=player->x-((c*VIEW_BACK)>>2),camera_y=player->y-((s*VIEW_BACK)>>2);
    // Frame the whole four-wide grid, then ease into the player's rear view.
    int32_t intro=game.phase==COUNTDOWN?60:game.race_ticks<60?60-game.race_ticks:0;
    int32_t grid_offset=((3-(int8_t)view_player*2)*32*intro)/60;
    camera_x-=((int32_t)s*grid_offset)>>6;camera_y+=((int32_t)c*grid_offset)>>6;
    int32_t rear_x=64-(grid_offset>>1),rear_foot=59-(intro*9)/60;
    int32_t horizontal_start=game.route_len>1&&
        (game.route[0]>>4)==(game.route[1]>>4);
    // Sky and a fixed world-bearing horizon provide orientation without a HUD map.
    for(int32_t x=0;x<128;x++) {
        int32_t ridge=((x+(camera_angle>>1))&31);
        int32_t y=18-(ridge<16?ridge:31-ridge)/8;
        pixel(x,y,1);
    }
    // Two-pixel ground samples amortize map lookup; sprites remain single-pixel.
    for(int32_t y=VIEW_HORIZON+3;y<64;y++) {
        int32_t depth=floor_depth[y-VIEW_HORIZON];
        int32_t wx=((int32_t)camera_x*16)+(((int32_t)(c+s)*depth)>>6);
        int32_t wy=((int32_t)camera_y*16)+(((int32_t)(s-c)*depth)>>6);
        int32_t step_x=(-(int32_t)s*depth)>>11;
        int32_t step_y=((int32_t)c*depth)>>11;
        int32_t row=(y>>3)*128;
        int32_t bit=1<<(y&7);
        for(int32_t x=0;x<128;x+=2,wx+=step_x,wy+=step_y)if(ground_ink(wx,wy,horizontal_start)) {
            framebuffer[row+x]|=bit;framebuffer[row+x+1]|=bit;
        }
    }
    // Cache only IDs/depths, saving half the former sort-record stack without
    // repeatedly projecting every object while looking for the next layer.
    uint8_t ids[CAR_COUNT-1+EFFECT_COUNT],count=0;
    int16_t depths[CAR_COUNT-1+EFFECT_COUNT];
    ViewObject object;
    for(int32_t i=0;i<CAR_COUNT+EFFECT_COUNT;i++) {
        if(!view_world_object(i,&object))continue;
        int32_t x=object.side-camera_x,y=object.depth-camera_y;
        int32_t depth=((int32_t)x*c+(int32_t)y*s)>>6;
        if(depth<VIEW_NEAR)continue;
        if(count>=(int32_t)sizeof(ids))break;
        ids[count]=i;depths[count++]=depth;
    }
    for(int32_t layer=0;layer<count;layer++) {
        int32_t best=0;
        for(int32_t i=1;i<count;i++)if(depths[i]>depths[best])best=i;
        view_world_object(ids[best],&object);
        int32_t wx=object.side-camera_x,wy=object.depth-camera_y;
        object.side=((int32_t)-wx*s+(int32_t)wy*c)>>6;
        object.depth=depths[best];depths[best]=0;
        ViewObject *o=&object;
        int32_t x=64+((int32_t)o->side*VIEW_FOCAL)/o->depth;
        int32_t foot=VIEW_HORIZON+(VIEW_HEIGHT*VIEW_FOCAL*16)/o->depth;
        int32_t size=(VIEW_CAR_WIDTH*VIEW_FOCAL*16)/o->depth;
        if(size>32)size=32;
        if(size<2)size=2;
        if(x<-(int32_t)size||x>128+size||foot-size>63)continue;
        if(o->kind==1) {
            Car *car=&game.cars[o->index];
            if(car->wave)view_ring(x,foot,size>24?48:size*2);
            if(car->shield) {
                int32_t h=(size*2)/3;
                for(int32_t yy=-(int8_t)h-2;yy<=2;yy++)for(int32_t xx=-(int8_t)(size/2)-2;xx<=(int8_t)(size/2)+2;xx++)pixel(x+xx,foot+yy,0);
                box(x-size/2-2,foot-h-2,size+5,h+5,0);
            }
            view_car(x,foot,size,o->index+1,0);
            if(size>=7&&foot-(size*2)/3>=15&&x>=2&&x<124)number(x-1,foot-(size*2)/3-6,o->index+1);
        } else view_power(x,foot,size,game.effects[o->index].type);
    }
    // The player's rear is a foreground silhouette, not an overhead sprite.
    if(player->wave)view_ring(rear_x,rear_foot-2,12+(8-player->wave)*3);
    if(player->shield) {
        for(int32_t y=rear_foot-19;y<rear_foot+4;y++)for(int32_t x=rear_x-15;x<rear_x+15;x++)pixel(x,y,0);
        box(rear_x-15,rear_foot-19,30,23,0);
    }
    uint8_t difference=(player->heading<<4)-camera_angle;
    int32_t lean=difference&&difference<128?1:difference>=128?-1:0;
    view_car(rear_x,rear_foot,24,0,lean);
    if(player->boost) {
        int32_t flame=1+(game.race_ticks&3);
        for(int32_t y=rear_foot+1;y<rear_foot+1+flame;y++){pixel(rear_x-5,y,1);pixel(rear_x+4,y,1);}
    }
    // A visible next-sector cue makes an off-road recovery possible without
    // the private route knowledge used by the automated regression driver.
    int32_t rx=center_x(game.route[game.gates[player->sectors&3]])-player->x;
    int32_t ry=center_y(game.route[game.gates[player->sectors&3]])-player->y;
    int32_t side=((int32_t)-rx*s+(int32_t)ry*c)>>6;
    int32_t forward=((int32_t)rx*c+(int32_t)ry*s)>>6;
    int32_t ax=0,ay=0;
    if(magnitude(side)>magnitude(forward)/2)ax=side>0?1:-1;
    else ay=forward>0?-1:1;
    text(42,10,"NEXT");
    view_line(64-ax*2,12-ay*2,64+ax*2,12+ay*2);
    pixel(64+ax+ay,12+ay+ax,1);pixel(64+ax-ay,12+ay-ax,1);
    if(!on_road(player->x,player->y))text(0,10,"OFF ROAD");
    else{text(0,10,"P");number(5,10,view_player+1);text(12,10,"W");number(17,10,game.wins[view_player]);}
    if(player->speed<0)text(112,10,"REV");
    // Personal deployment feedback complements the actual hazard behind the car.
    if(rear_flash) {
        for(int32_t yy=56;yy<64;yy++)for(int32_t xx=0;xx<18;xx++)pixel(xx,yy,0);
        text(1,58,rear_type==OIL?"OIL":"MINE");
        pixel(49,60,1);pixel(79,60,1);
        if(rear_flash&1){pixel(47,62,1);pixel(81,62,1);}
    }
}

static int32_t lap_of(const Car *c) {
    int32_t lap=c->sectors?1+((c->sectors-1)>>2):1;
    return lap>LAPS?LAPS:lap;
}
static void draw_scores(int32_t y) {
    for(int32_t i=0;i<CAR_COUNT;i++) {
        text(15,y+i*7,i==view_player?"YOU":(game.humans&(1<<i))?"P":"BOT");
        if(i!=view_player)number(31,y+i*7,i+1);
        number(75,y+i*7,game.wins[i]);text(84,y+i*7,"OF 5");
    }
}
void game_draw(void) {
    for(int32_t i=0;i<1024;i++)framebuffer[i]=0;
    if(view_player!=game.actor&&game.phase==POWER_SELECT) {
        text(24,3,"WAIT FOR PLAYER");number(85,3,game.actor+1);
        text(30,12,"PICKING POWER");
        draw_scores(25);return;
    }
    if(game.phase==TITLE) {
        label(44,3,"NITRO",2);label(32,17,"NONSENSE",2);
        captions(
            "\021\041" "FIRST TO FIVE WINS\0" // (17, 33)
            "\021\051" "DOWN REVERSE\0" // (17, 41)
            "\021\061" "LEFT RIGHT STEER\0" // (17, 49)
            "\005\072" "A RACE   A B POWERS\0" // (5, 58)
            "\377");
        box(0,56,128,8,0);
    } else if(game.phase==POWER_SELECT) {
        captions(
            "\001\001" "PICK ONE POWER\0" // (1, 1)
            "\001\013" "A\0" // (1, 11)
            "\102\013" "B\0" // (66, 11)
            "\134\071" "B SLOT\0" // (92, 57)
            "\377");
        text(89,1,game.slot?"SLOT B":"SLOT A");
        text(9,11,power_names[game.loadouts[view_player][0]]);
        text(74,11,power_names[game.loadouts[view_player][1]]);
        for(int32_t i=0;i<POWER_COUNT;i++) {
            int32_t x=(i&1)*64,y=22+(i>>1)*10;
            if(i==game.choice)box(x,y,63,9,0);
            text(x+3,y+2,power_names[i]);
        }
        text(3,57,game.error?(game.slot?"IN SLOT A":"IN SLOT B"):"A CHOOSE");
    } else if(game.phase==RACE||game.phase==COUNTDOWN) {
        chase_draw();
        captions(
            "\000\001" "LAP\0" // (0, 1)
            "\026\001" "OF 3\0" // (22, 1)
            "\052\001" "A\0" // (42, 1)
            "\126\001" "B\0" // (86, 1)
            "\377");
        number(15,1,lap_of(&game.cars[view_player]));
        text(48,1,power_names[game.loadouts[view_player][0]]);
        text(92,1,power_names[game.loadouts[view_player][1]]);
        if(game.cars[view_player].cooldown[0])box(48,7,game.cars[view_player].cooldown[0]>>3,1,1);
        if(game.cars[view_player].cooldown[1])box(92,7,game.cars[view_player].cooldown[1]>>3,1,1);
        if(game.phase==COUNTDOWN) {
            box(54,20,20,10,1);
            for(int32_t yy=21;yy<29;yy++)for(int32_t xx=56;xx<72;xx++)pixel(xx,yy,0);
            number(62,22,3-game.ticks/30);
        }
    } else {
        text(28,1,game.phase==MATCH_OVER?"MATCH CHAMPION":"ROUND RESULT");
        if(game.winner==255)text(42,10,"NO FINISH");
        else {text(31,10,game.winner==view_player?"YOU":(game.humans&(1<<game.winner))?"P":"BOT");
        if(game.winner!=view_player)number(47,10,game.winner+1);
        text(58,10,"WINS");}
        draw_scores(21);
        if(view_player!=game.actor){text(23,58,"WAIT FOR PLAYER");number(85,58,game.actor+1);}
        else if(game.phase==MATCH_OVER)text(34,58,"A NEW MATCH");
        else if(game.winner==255)text(44,58,"A RETRY");
        else text(44,58,"A NEXT");
    }
}

#endif
