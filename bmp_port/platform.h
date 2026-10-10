#ifndef _PLATFORM_H
#define _PLATFORM_H

#include <stdbool.h>
#include "board.h"

#define PLATFORM_IDENT "(CH32V30x)"

/*
 * True while bmp_poll_loop() is blocked in gdb_packet_receive() waiting for the
 * next GDB request: a slow host side task may only touch the target while no
 * GDB command is in flight.
 */
extern volatile bool platform_gdb_idle;

#define SET_RUN_STATE(state)     \
    do {                         \
        running_status = (state); \
    } while (0)

/*
 * The status LED is owned by board_led_tick() in board.c, which derives every
 * state it shows from two things it can read: cur_target (attached or not) and
 * running_status.  Neither these macros nor SET_RUN_STATE() touch the LED, they
 * only maintain running_status.  Entering the idle state clears the run state
 * so a stale "running" cannot keep the LED blinking.
 */
#define SET_IDLE_STATE(state)        \
    do {                             \
        platform_gdb_idle = (state); \
        if (state) {                 \
            running_status = 0;      \
        }                            \
    } while (0)

#define SET_ERROR_STATE(state)

#endif /* _PLATFORM_H */
