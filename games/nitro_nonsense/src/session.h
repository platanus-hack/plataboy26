#ifndef NITRO_SESSION_H
#define NITRO_SESSION_H
#include "link.h"
// Legacy codec entry: only a newly validated COMMIT reaches session_apply.
uint8_t session_receive(Link *link,const uint8_t *packet,uint8_t size);
#endif
