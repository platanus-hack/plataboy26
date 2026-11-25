#include "catalog_connection.h"
// Keep catalog bookkeeping outside the caller's large render frame.
__attribute__((noinline)) uint8_t connection_catalog_step(Connection *c,Wire *w,Catalog *catalog,Stream *stream,uint32_t now,uint8_t high){
    if(!c->active){
        uint8_t event=catalog_step(catalog,w,now,high);
        if(event&CATALOG_RENDER)return CONNECTION_RENDER;
        if(!(event&(CATALOG_READY|CATALOG_DATA))||!catalog_bind(catalog,&stream->link)||
           (!catalog->host&&!(event&CATALOG_DATA)))return 0;
    }
    return connection_stream_service(c,w,stream,now,high);
}
