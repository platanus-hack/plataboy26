#include "session.h"
#include "session_internal.h"
#include "game.h"
#ifdef NN_OVERLAYS
#include "overlay.h"
#endif
_Static_assert(CAR_COUNT==LINK_MAX_PEERS,"Link slots must match the canonical car limit");
uint8_t session_apply(Link *link,uint8_t event){
    if(event&LINK_APPLIED){
#ifdef NN_OVERLAYS
        session_load_begin();
#endif
        if(event&LINK_START_APPLIED){
            // Count/self were validated before the START could be prepared.
#ifdef NN_OVERLAYS
            session_load_group(NN_OVERLAY_SETUP);
#endif
            game_set_view(link->self);game_begin(link->seed,(1<<link->count)-1);
#ifdef NN_OVERLAYS
            session_load_group(game_required_group());
#endif
        }else for(uint8_t tick=0;tick<link->steps;tick++){
#ifdef NN_OVERLAYS
            session_load_group(game_required_group());
#endif
            game_step(link->controls);
#ifdef NN_OVERLAYS
            session_load_group(game_required_group());
#endif
        }
#ifdef NN_OVERLAYS
        session_load_end();
#endif
    }
    return event;
}
uint8_t session_receive(Link *link,const uint8_t *packet,uint8_t size){
    return session_apply(link,link_receive(link,packet,size));
}
