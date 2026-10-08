/*
 * cdc_acm_dual.c
 *
 * USB device of the CH32V30x Black Magic Probe port.  Composite device:
 *
 *   interface 0/1  GDB server        (CDC ACM, bulk)
 *   interface 2/3  target UART (aux) (CDC ACM, bulk, USART3 on PB10/PB11)
 *   interface 4    DFU runtime       (no endpoints)
 *
 * The DFU runtime interface exists so the probe can be sent back into
 * ch32_dfu_boot: dfu-util -e (DFU_DETACH) calls platform_request_boot(), which
 * writes the BKP hand-shake and resets; the bootloader then starts in DFU mode
 * instead of booting this application again.  "monitor bootloader" uses the
 * same platform hook.
 *
 * No RTT: this port deliberately provides only the GDB port and the target UART
 * (like bmp-hpm-port minus its RTT multiplexing, and unlike ch32v305_bmp which
 * adds a third CDC port for RTT).
 *
 * MS OS 1.0 compatible IDs (WCID) are attached to interfaces 0, 2 and 4 so
 * Windows binds WinUSB to all three automatically - no Zadig step.
 */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usb_config.h"

#include "board.h"
#include "platform.h"
#include "gdb_if.h"
#include "aux_serial.h"

/* ------------------------------------------------------------------ *
 * Endpoints and interfaces
 * ------------------------------------------------------------------ */
#define GDB_INT_EP 0x81
#define GDB_OUT_EP 0x01
#define GDB_IN_EP  0x82

#define AUX_INT_EP 0x83
#define AUX_OUT_EP 0x03
#define AUX_IN_EP  0x84

#define GDB_CTRL_INTF 0
#define AUX_CTRL_INTF 2
#define DFU_INTF      4

#define USB_XFER_SIZE 512U /* bulk max packet size at high speed */

#define GDB_CDC_DESC_LEN CDC_ACM_DESCRIPTOR_LEN
#define AUX_CDC_DESC_LEN CDC_ACM_DESCRIPTOR_LEN
#define DFU_IF_DESC_LEN  (9 + 9) /* interface + DFU functional */
#define USB_CONFIG_SIZE  (9 + GDB_CDC_DESC_LEN + AUX_CDC_DESC_LEN + DFU_IF_DESC_LEN)

/* One maximum size GDB packet must fit without dropping bytes
 * (GDB_PACKET_BUFFER_SIZE is set from the build system). */
#ifndef GDB_PACKET_BUFFER_SIZE
#define GDB_PACKET_BUFFER_SIZE 2048U
#endif
#define GDB_BUF_SIZE   GDB_PACKET_BUFFER_SIZE
#define GDB_BUF_MASK   (GDB_BUF_SIZE - 1U)

/* GDB, host -> probe */
static USB_MEM_ALIGNX uint8_t gdb_out_xfer[USB_XFER_SIZE];
static volatile uint32_t gdb_out_len;
static volatile uint32_t gdb_out_pos;
static volatile bool gdb_out_armed;

/* GDB, probe -> host (ring buffer, pushed in USB_XFER_SIZE chunks) */
static USB_MEM_ALIGNX uint8_t gdb_in_xfer[USB_XFER_SIZE];
static uint8_t gdb_in_buf[GDB_BUF_SIZE];
static volatile uint32_t gdb_in_head;
static volatile uint32_t gdb_in_tail;
static volatile bool gdb_in_busy;

/* target UART, host -> target */
static USB_MEM_ALIGNX uint8_t aux_out_xfer[USB_XFER_SIZE];
static volatile bool aux_out_armed;

/* target UART, target -> host */
static USB_MEM_ALIGNX uint8_t aux_in_xfer[64];
static volatile bool aux_in_busy;
static volatile bool aux_dtr;

/* ------------------------------------------------------------------ *
 * Descriptors
 * ------------------------------------------------------------------ */
static const uint8_t device_descriptor[] = {
    /* bcdDevice is part of the Windows hardware ID: bump it whenever the
     * interface layout or the WCID data changes, to force a fresh install. */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* GDB server: interfaces 0 (control) and 1 (data) */
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_HS, 0x04),
    /* target UART: interfaces 2 (control) and 3 (data) */
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_HS, 0x05),
    /* DFU runtime: interface 4 */
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

static const uint8_t config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_FS, 0x04),
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_FS, 0x05),
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00,
};

static const uint8_t other_speed_config_descriptor_hs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_FS, 0x04),
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_FS, 0x05),
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

static const uint8_t other_speed_config_descriptor_fs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_HS, 0x04),
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_HS, 0x05),
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

/* Serial number from the CH32V30x unique id (0x1FFFF7E8), filled at init. */
static char serial_string[25];

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },              /* Langid */
    "Blackmagic Debug",                        /* Manufacturer */
    "Black Magic Probe (CH32V30x)",            /* Product */
    serial_string,                             /* Serial Number */
    "Black Magic GDB Server",                  /* iInterface 4 */
    "Black Magic UART",                        /* iInterface 5 */
};

static const uint8_t *device_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_cb(uint8_t speed)
{
    return (speed == USB_SPEED_HIGH) ? config_descriptor_hs : config_descriptor_fs;
}

static const uint8_t *device_quality_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

static const uint8_t *other_speed_descriptor_cb(uint8_t speed)
{
    return (speed == USB_SPEED_HIGH) ? other_speed_config_descriptor_hs
                                     : other_speed_config_descriptor_fs;
}

static const char *string_descriptor_cb(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= (sizeof(string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return string_descriptors[index];
}

/* ------------------------------------------------------------------ *
 * MS OS 1.0 (WCID): bind WinUSB to the GDB, UART and DFU interfaces
 *
 * Windows asks for GET_DESCRIPTOR(String, 0xEE) to learn the vendor code and
 * then fetches the compatible ID feature descriptor, which carries one 24 byte
 * entry per interface.  No extended properties are advertised: the compatible
 * ID alone is what makes Windows install WinUSB.
 * ------------------------------------------------------------------ */
#define WINUSB_VENDOR_CODE 0x20U

static const uint8_t msos_string[] = {
    0x12, 0x03,
    'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00,
    '1', 0x00, '0', 0x00, '0', 0x00,
    WINUSB_VENDOR_CODE,
    0x00,
};

#define WCID_ENTRY(iface)                                            \
    iface, 0x01,                                                     \
    0x57, 0x49, 0x4E, 0x55, 0x53, 0x42, 0x00, 0x00, /* "WINUSB\0\0" */ \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* sub-compatible */ \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00

static const uint8_t msos_compat_id[] = {
    0x58, 0x00, 0x00, 0x00, /* dwLength = 16 + 24 * 3 */
    0x00, 0x01,             /* bcdVersion 1.0 */
    0x04, 0x00,             /* wIndex 0x0004 */
    0x03,                   /* bCount */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* reserved[7] */
    WCID_ENTRY(GDB_CTRL_INTF),
    WCID_ENTRY(AUX_CTRL_INTF),
    WCID_ENTRY(DFU_INTF),
};

static const struct usb_msosv1_descriptor msosv1 = {
    .string = msos_string,
    .vendor_code = WINUSB_VENDOR_CODE,
    .compat_id = msos_compat_id,
    .comp_id_property = NULL,
};

/* ------------------------------------------------------------------ *
 * GDB data path (gdb_if.h)
 *
 * One bulk transfer at a time in each direction: the OUT pipe is re-armed once
 * its staging buffer has been drained (which flow-controls the host), the IN
 * side drains a ring buffer in USB_XFER_SIZE chunks.
 * ------------------------------------------------------------------ */
static void gdb_out_arm(void)
{
    if (gdb_out_armed || (gdb_out_pos < gdb_out_len)) {
        return;
    }
    gdb_out_len = 0U;
    gdb_out_pos = 0U;
    gdb_out_armed = true;
    usbd_ep_start_read(0, GDB_OUT_EP, gdb_out_xfer, sizeof(gdb_out_xfer));
}

static int gdb_out_pop(void)
{
    if (gdb_out_pos >= gdb_out_len) {
        gdb_out_arm();
        return -1;
    }
    return gdb_out_xfer[gdb_out_pos++];
}

static void gdb_in_kick(void)
{
    uint32_t count = 0U;

    if (gdb_in_busy) {
        return;
    }
    while ((count < sizeof(gdb_in_xfer)) && (gdb_in_tail != gdb_in_head)) {
        gdb_in_xfer[count++] = gdb_in_buf[gdb_in_tail];
        gdb_in_tail = (gdb_in_tail + 1U) & GDB_BUF_MASK;
    }
    if (count == 0U) {
        return;
    }
    gdb_in_busy = true;
    usbd_ep_start_write(0, GDB_IN_EP, gdb_in_xfer, count);
}

static void usbd_cdc_acm_bulk_out_gdb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    gdb_out_armed = false;
    gdb_out_len = (nbytes > sizeof(gdb_out_xfer)) ? sizeof(gdb_out_xfer) : nbytes;
    gdb_out_pos = 0U;
}

static void usbd_cdc_acm_bulk_in_gdb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    gdb_in_busy = false;
    gdb_in_kick();
}

int gdb_if_init(void)
{
    return 0;
}

char gdb_if_getchar(void)
{
    for (;;) {
        int c = gdb_out_pop();

        if (c >= 0) {
            return (char)c;
        }
        /* Nothing from the host: keep the target UART flowing while waiting. */
        aux_serial_poll();
    }
}

char gdb_if_getchar_to(uint32_t timeout)
{
    uint32_t start = board_time_ms();

    for (;;) {
        int c = gdb_out_pop();

        if (c >= 0) {
            return (char)c;
        }
        if (timeout == 0U) {
            return (char)-1;
        }
        if ((uint32_t)(board_time_ms() - start) >= timeout) {
            return (char)-1;
        }
        aux_serial_poll();
    }
}

void gdb_if_putchar(char c, bool flush)
{
    if (((gdb_in_head + 1U) & GDB_BUF_MASK) == gdb_in_tail) {
        return; /* full: drop instead of corrupting the packet stream */
    }
    gdb_in_buf[gdb_in_head] = (uint8_t)c;
    gdb_in_head = (gdb_in_head + 1U) & GDB_BUF_MASK;
    if (flush) {
        gdb_in_kick();
    }
}

void gdb_if_flush(bool force)
{
    (void)force;
    gdb_in_kick();
}

/* ------------------------------------------------------------------ *
 * Target UART data path
 * ------------------------------------------------------------------ */
static void aux_out_arm(void)
{
    if (!aux_out_armed) {
        aux_out_armed = true;
        usbd_ep_start_read(0, AUX_OUT_EP, aux_out_xfer, sizeof(aux_out_xfer));
    }
}

static void usbd_cdc_acm_bulk_out_aux(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    (void)aux_serial_write(aux_out_xfer, nbytes);
    aux_out_armed = false;
    aux_out_arm();
}

static void usbd_cdc_acm_bulk_in_aux(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    aux_in_busy = false;
}

/*
 * Forward target UART bytes to the host when the port is open and no transfer
 * is in flight.  Driven from the GDB idle loops: the CH32V30x device controller
 * never raises SOF events, so there is no per-frame tick to hook.
 */
void aux_serial_poll(void)
{
    uint32_t count;

    if (aux_in_busy || !aux_dtr) {
        return;
    }
    count = aux_serial_read(aux_in_xfer, sizeof(aux_in_xfer));
    if (count == 0U) {
        return;
    }
    aux_in_busy = true;
    usbd_ep_start_write(0, AUX_IN_EP, aux_in_xfer, count);
}

/* ------------------------------------------------------------------ *
 * CDC ACM class hooks (weak in the class, overridden here)
 * ------------------------------------------------------------------ */
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;

    if (intf == AUX_CTRL_INTF) {
        aux_dtr = dtr;
    }
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    struct aux_line_coding aux;

    (void)busid;

    if ((intf != AUX_CTRL_INTF) || (coding == NULL)) {
        return;
    }
    aux.baudrate = coding->dwDTERate ? coding->dwDTERate : 115200U;
    aux.data_bits = (coding->bDataBits == 7U) ? 7U : 8U;
    aux.parity = coding->bParityType; /* 0 none, 1 odd, 2 even - same numbering */
    aux.stop_bits = coding->bCharFormat ? 2U : 1U;
    aux_serial_set_encoding(&aux);
}

/* ------------------------------------------------------------------ *
 * DFU runtime interface: hand over to the bootloader
 * ------------------------------------------------------------------ */
enum {
    DFU_DETACH    = 0,
    DFU_DNLOAD    = 1,
    DFU_UPLOAD    = 2,
    DFU_GETSTATUS = 3,
    DFU_CLRSTATUS = 4,
    DFU_GETSTATE  = 5,
    DFU_ABORT     = 6,
};

static int dfu_control_request(uint8_t busid, struct usb_setup_packet *setup,
                               uint8_t **data, uint32_t *len)
{
    (void)busid;

    switch (setup->bRequest) {
        case DFU_DETACH:
            /* dfu-util -e: reset into ch32_dfu_boot.  Never returns. */
            platform_request_boot();
            return 0;
        case DFU_GETSTATUS: {
            static uint8_t status[6] = { 0, 0, 0, 0, 0 /* appIDLE */, 0 };

            *data = status;
            *len = sizeof(status);
            return 0;
        }
        case DFU_GETSTATE: {
            static uint8_t state = 0; /* appIDLE */

            *data = &state;
            *len = 1;
            return 0;
        }
        case DFU_CLRSTATUS:
        case DFU_ABORT:
            return 0;
        default:
            return -1;
    }
}

/* ------------------------------------------------------------------ *
 * Endpoints and initialisation
 * ------------------------------------------------------------------ */
static struct usbd_endpoint gdb_out_ep = { .ep_addr = GDB_OUT_EP, .ep_cb = usbd_cdc_acm_bulk_out_gdb };
static struct usbd_endpoint gdb_in_ep  = { .ep_addr = GDB_IN_EP,  .ep_cb = usbd_cdc_acm_bulk_in_gdb };
static struct usbd_endpoint aux_out_ep = { .ep_addr = AUX_OUT_EP, .ep_cb = usbd_cdc_acm_bulk_out_aux };
static struct usbd_endpoint aux_in_ep  = { .ep_addr = AUX_IN_EP,  .ep_cb = usbd_cdc_acm_bulk_in_aux };

static struct usbd_interface intf_gdb_ctrl;
static struct usbd_interface intf_gdb_data;
static struct usbd_interface intf_aux_ctrl;
static struct usbd_interface intf_aux_data;
static struct usbd_interface intf_dfu;

static void build_serial_string(void)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t *uid = (const uint8_t *)0x1FFFF7E8UL; /* CH32V30x unique id */
    uint32_t i;

    for (i = 0U; i < 12U; i++) {
        serial_string[i * 2U] = hex[(uid[i] >> 4) & 0x0FU];
        serial_string[(i * 2U) + 1U] = hex[uid[i] & 0x0FU];
    }
    serial_string[24] = '\0';
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event) {
        case USBD_EVENT_CONFIGURED:
            gdb_out_pos = gdb_out_len = 0U;
            gdb_out_armed = false;
            aux_out_armed = false;
            gdb_out_arm();
            aux_out_arm();
            break;
        case USBD_EVENT_RESET:
            gdb_out_pos = gdb_out_len = 0U;
            gdb_in_head = gdb_in_tail = 0U;
            gdb_out_armed = false;
            gdb_in_busy = false;
            aux_out_armed = false;
            aux_in_busy = false;
            break;
        default:
            break;
    }
}

void cdc_acm_init(uint8_t busid, uint32_t reg_base)
{
    static const struct usb_descriptor cdc_descriptor = {
        .device_descriptor_callback         = device_descriptor_cb,
        .config_descriptor_callback         = config_descriptor_cb,
        .device_quality_descriptor_callback = device_quality_descriptor_cb,
        .other_speed_descriptor_callback    = other_speed_descriptor_cb,
        .string_descriptor_callback         = string_descriptor_cb,
        .msosv1_descriptor                  = &msosv1,
        .msosv2_descriptor                  = NULL,
        .bos_descriptor                     = NULL,
    };

    build_serial_string();
    aux_serial_init();

    usbd_desc_register(busid, &cdc_descriptor);

    /* GDB function -> interfaces 0 and 1 */
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf_gdb_ctrl));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf_gdb_data));
    /* target UART function -> interfaces 2 and 3 */
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf_aux_ctrl));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf_aux_data));
    /* DFU runtime -> interface 4 */
    intf_dfu.class_interface_handler = dfu_control_request;
    usbd_add_interface(busid, &intf_dfu);

    usbd_add_endpoint(busid, &gdb_out_ep);
    usbd_add_endpoint(busid, &gdb_in_ep);
    usbd_add_endpoint(busid, &aux_out_ep);
    usbd_add_endpoint(busid, &aux_in_ep);

    usbd_initialize(busid, reg_base, usbd_event_handler);
}
