/*
 * Black Magic Probe on CH32V305.  Application entry point; the port layer lives
 * in bmp_port/ and the USB stack is CherryUSB v1.6.0 with the WCH USBHS device
 * controller.
 */

#include "stdio.h"
#include "board.h"
#include "usb_config.h"

#include "general.h"
#include "platform.h"
#include "gdb_if.h"
#include "gdb_main.h"
#include "target.h"
#include "target_internal.h" /* target_list (RTT boot-time scan) */
#include "exception.h"
#include "gdb_packet.h"
#include "morse.h"
#include "command.h"
#include <stdint.h>
#include "jtag_port.h"
#ifdef ENABLE_RTT
#include "rtt.h"
#endif

extern void cdc_acm_init(uint8_t busid, uint32_t reg_base);

static void bmp_poll_loop(void)
{
    SET_IDLE_STATE(false);
    while (gdb_target_running && cur_target) {
        gdb_poll_target();

        /* Check again, as `gdb_poll_target()` may alter these variables. */
        if (!gdb_target_running || !cur_target)
            break;
        char c = gdb_if_getchar_to(0);
        if (c == '\x03' || c == '\x04')
            target_halt_request(cur_target);
#ifdef ENABLE_RTT
        else if (rtt_enabled)
            poll_rtt(cur_target);
#endif
        platform_pace_poll();
    }

    SET_IDLE_STATE(true);
    const gdb_packet_s *const packet = gdb_packet_receive();
    /* If port closed and target detached, stay idle */
    if (packet->data[0] != '\x04' || cur_target)
        SET_IDLE_STATE(false);
    gdb_main(packet);
}

int main(void)
{
    board_init();
    /* Probe-owned SWD/JTAG pins; the board layer does not know about them. */
    jtag_port_init();
    cdc_acm_init(0, (uint32_t)USBHS_BASE);

    platform_init();

#ifdef ENABLE_RTT
    /*
     * Enumerate the target once at boot so RTT works with no GDB session at
     * all: opening the RTT port is then enough to see output. Attaching to
     * whatever turns up is done lazily in platform_rtt_target(). A missing or
     * uncooperative target is not an error - the probe simply behaves as
     * before and RTT starts once GDB attaches.
     */
    TRY (EXCEPTION_ALL) {
        jtag_scan();
    }
    CATCH () {
    default:
        break;
    }
#endif

    while (true) {
        TRY (EXCEPTION_ALL) {
            bmp_poll_loop();
        }
        CATCH () {
        default:
            gdb_put_packet_error(0xffU);
            target_list_free();
            gdb_outf("Uncaught exception: %s\n", exception_frame.msg);
            morse("TARGET LOST.", true);
        }
    }

    target_list_free();
    return 0;
}
