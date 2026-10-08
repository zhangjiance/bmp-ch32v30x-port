#ifndef _PLATFORM_H
#define _PLATFORM_H

#include <stdbool.h>
#include "board.h"

#define PLATFORM_IDENT "(CH32V30x)"

/*
 * True while bmp_poll_loop() is blocked in gdb_packet_receive() waiting for the
 * next GDB request.  Kept for the same purpose as in bmp-hpm-port (a slow host
 * side task may only touch the target while no GDB command is in flight).
 */
extern volatile bool platform_gdb_idle;

#define SET_RUN_STATE(state) \
    do {                     \
        running_status = (state); \
    } while (0)

#define SET_IDLE_STATE(state)        \
    do {                             \
        platform_gdb_idle = (state); \
        if (state) {                 \
            running_status = 0;      \
            board_led_write(!state); \
        } else {                     \
            running_status = 1;      \
        }                            \
    } while (0)

#define SET_ERROR_STATE(state)

#endif /* _PLATFORM_H */
