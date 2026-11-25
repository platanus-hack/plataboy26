#include "stream_session.h"
#include "session_internal.h"
#include "game.h"
_Static_assert((ACT|BACK)==LINK_ACTION_MASK,"Stream action routing must match the Game controls");
static uint8_t apply(Stream *stream,uint8_t event){
    uint8_t phase=game.phase,actor=game.actor;
    session_apply(&stream->link,event);
    if((event&LINK_APPLIED)&&(phase!=game.phase||actor!=game.actor))stream_block_actions(stream);
    return event;
}
uint8_t session_stream_receive(Stream *stream,const uint8_t *packet,uint8_t size){
    return apply(stream,stream_receive(stream,packet,size));
}
uint8_t session_stream_sent(Stream *stream,const uint8_t *packet,uint8_t size){
    return apply(stream,stream_sent(stream,packet,size));
}
