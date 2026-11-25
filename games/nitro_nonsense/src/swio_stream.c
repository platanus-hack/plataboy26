#include "swio.h"
#include "swio_internal.h"
#include "stream_connection.h"
#include "system.h"
#include "gpio.h"
uint8_t swio_stream_connect(Connection *connection,Discovery *catalog,Stream *stream,Wire *wire){
    if(!swio_controller_ready(wire))return 0;
    uint8_t event=connection_stream_step(connection,wire,catalog,stream,STK->CNT,PIN_read(PD1));
    swio_drive(wire->low);return event;
}
