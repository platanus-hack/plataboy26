#include "overlay.h"
#include "game.h"
#include "music.h"

static NN_OV2_LOCAL uint16_t random_step(uint16_t *state) {
    *state^=*state<<7;*state^=*state>>9;*state^=*state<<8;
    return *state;
}
static NN_OV2_LOCAL void connect(uint8_t a,uint8_t b) {
    uint8_t direction=b==a+1?RIGHT:b+1==a?LEFT:b==a+16?DOWN:UP;
    uint8_t reverse=((direction<<2)|(direction>>2))&15;
    game.tiles[a>>1]|=direction<<((a&1)*4);
    game.tiles[b>>1]|=reverse<<((b&1)*4);
}
static NN_OV2_LOCAL void walk_to(uint8_t destination) {
    uint8_t cell=game.route[game.route_len-1];
    while(cell!=destination) {
        NN_MUSIC_POLL();
        uint8_t next;
        if((cell>>4)!=(destination>>4))next=cell<destination?cell+16:cell-16;
        else next=cell<destination?cell+1:cell-1;
        connect(cell,next);cell=next;
        if(cell!=game.start)game.route[game.route_len++]=cell;
    }
}
NN_OV2 void track_generate(uint16_t seed) {
    uint16_t rng=seed?seed:1;
    uint8_t left=1+(random_step(&rng)&1),right=13+(random_step(&rng)&1);
    uint8_t top=1+(random_step(&rng)&1),bottom=4+(random_step(&rng)&1);
    uint8_t start=left+2;
    game.start=top*16+start;game.route[0]=game.start;game.route_len=1;
    // Two optional broad detours preserve a long, straight starting corridor.
    if(random_step(&rng)&1) {
        uint8_t begin=left+5,end=right-1;
        walk_to(top*16+begin);walk_to((top-1)*16+begin);
        walk_to((top-1)*16+end);walk_to(top*16+end);
    }
    walk_to(top*16+right);walk_to(bottom*16+right);
    if(random_step(&rng)&1) {
        uint8_t begin=right-2,end=left+2;
        walk_to(bottom*16+begin);walk_to((bottom+1)*16+begin);
        walk_to((bottom+1)*16+end);walk_to(bottom*16+end);
    }
    walk_to(bottom*16+left);walk_to(top*16+left);walk_to(game.start);
    game.gates[0]=0;
    for(uint8_t sector=1;sector<4;sector++) {
        uint8_t at=(game.route_len*sector)>>2;
        // Prefer a straight road cell so corner cutting does not miss the plane.
        for(uint8_t n=0;n<4;n++) {
            uint8_t tile=track_get(game.route[at]);
            if(tile==(LEFT|RIGHT)||tile==(UP|DOWN))break;
            at++;
        }
        game.gates[sector]=at;
    }
}
