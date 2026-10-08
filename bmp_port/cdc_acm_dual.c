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
 * WinUSB for the DFU runtime interface is announced through Microsoft OS 1.0
 * (WCID) descriptors, the same way ch32_hello_world, bmp-hpm-port and
 * ch32_dfu_boot do it, so Windows installs the driver automatically (no Zadig
 * step, so "dfu-util -e" works out of the box) while the two CDC functions keep
 * their COM ports.
 */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usb_config.h"

#include "ch32v30x.h" /* NVIC_DisableIRQ()/USBHS_IRQn for the GDB pipe locking */

#include "board.h"
#include "general.h" /* platform_support.h requires this to be included first */
#include "platform.h"
#include "platform_support.h" /* platform_request_boot() for DFU_DETACH */
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

/*
 * Every GDB reply has to fit into a single USB packet, and GDB_PACKET_BUFFER_SIZE
 * is what limits a reply (the stub announces it as PacketSize, so GDB keeps its
 * requests - including the target description XML chunks - inside it).
 *
 * gdb_packet_send() writes, for one packet:
 *     '$' + escaped payload + '#' + 2 checksum digits
 * so a reply is at most GDB_PACKET_BUFFER_SIZE + 4 bytes on the wire.
 *
 * Why it must stay within one packet: the CH32V30x USBHS device driver copies
 * only the first max-packet-size chunk into the endpoint, and a longer transfer
 * has to be continued from its interrupt (as do the completion callbacks).  On
 * this port such transfers have repeatedly stopped after that first chunk and
 * only finished once the host sent the next request, which the host reports as
 * "Ignoring packet error, continuing..." - and, because the packet that got
 * delayed then arrives as the answer to the *next* request, as
 * "Truncated register N in remote 'g' packet" for the register read that
 * followed it.  The first reply large enough to trigger this is the RISC-V
 * target description XML (about 4.5 KB, which GDB asks for in maximum size
 * chunks right after attach), so 'mon jt' always looked fine while 'att 1'
 * always failed.
 *
 * Keeping GDB_PACKET_BUFFER_SIZE at 496 means no reply ever needs more than one
 * packet, so the send path below never depends on that continuation nor on its
 * completion callback.  Raise it once the controller driver is fixed: see
 * README, "已知问题：GDB 长回复卡住", for the analysis and the deferred fix.
 */
#ifndef GDB_PACKET_BUFFER_SIZE
#define GDB_PACKET_BUFFER_SIZE 496U
#endif

/* One escaped maximum size packet plus framing, i.e. the most gdb_if_putchar()
 * can stage for one reply (it can double every payload byte). */
#define GDB_IN_BUF_SIZE (2U * (GDB_PACKET_BUFFER_SIZE + 8U))

/* A reply plus '$', '#' and the two checksum digits must stay inside one USB
 * packet, see the comment above. */
#if (GDB_PACKET_BUFFER_SIZE + 4U) > USB_XFER_SIZE
#error "GDB_PACKET_BUFFER_SIZE must keep a whole reply inside one USB packet"
#endif

/* GDB, host -> probe */
static USB_MEM_ALIGNX uint8_t gdb_out_xfer[USB_XFER_SIZE];
static volatile uint32_t gdb_out_len;
static volatile uint32_t gdb_out_pos;
static volatile bool gdb_out_armed;

/*
 * GDB, probe -> host.  gdb_if_putchar() stages one reply in gdb_in_buf and
 * gdb_in_send() copies it into gdb_in_xfer, so the staging buffer is free again
 * while the reply is still being read out by the host.
 */
static USB_MEM_ALIGNX uint8_t gdb_in_buf[GDB_IN_BUF_SIZE];
static uint32_t gdb_in_len;
static USB_MEM_ALIGNX uint8_t gdb_in_xfer[USB_XFER_SIZE];
static volatile bool gdb_in_busy;

/* target UART, host -> target */
static USB_MEM_ALIGNX uint8_t aux_out_xfer[USB_XFER_SIZE];
static volatile bool aux_out_armed;

/* target UART, target -> host */
static USB_MEM_ALIGNX uint8_t aux_in_xfer[64];
static volatile bool aux_in_busy;
static volatile bool aux_dtr;

/* MS OS 1.0 (WCID) vendor code, used by the 0xEE string descriptor and the
 * vendor requests; the descriptors are further down. */
#define WINUSB_VENDOR_CODE 0x20U

/* ------------------------------------------------------------------ *
 * Descriptors
 * ------------------------------------------------------------------ */
static const uint8_t device_descriptor[] = {
    /* bcdDevice is part of the Windows hardware ID and of the usbflags WCID
     * cache key (VID/PID/bcdDevice), so bump it whenever the interface layout
     * or the WCID data changes: that is what makes Windows redo the MS OS 1.0
     * inquiry instead of reusing a cached, possibly failed, result.
     * Revision history of this port: 0x0100 WCID 1.0 with a NULL
     * comp_id_property, 0x0101 MS OS 2.0 experiment, 0x0102 first 1.0 rework,
     * 0x0103 this one. */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0103, 0x01)
};

static const uint8_t config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* GDB server: interfaces 0 (control) and 1 (data) */
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_HS, 0x04),
    /* target UART: interfaces 2 (control) and 3 (data) */
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_HS, 0x05),
    /* DFU runtime: interface 4 */
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x06,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

static const uint8_t config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_FS, 0x04),
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_FS, 0x05),
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x06,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02,             /* bcdUSB 2.0, matching the device descriptor */
    0x00, 0x00, 0x00, 0x40, 0x01, 0x00,
};

static const uint8_t other_speed_config_descriptor_hs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_FS, 0x04),
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_FS, 0x05),
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x06,
    0x09, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x10, 0x1A, 0x01,
};

static const uint8_t other_speed_config_descriptor_fs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(GDB_CTRL_INTF, GDB_INT_EP, GDB_OUT_EP, GDB_IN_EP, USB_BULK_EP_MPS_HS, 0x04),
    CDC_ACM_DESCRIPTOR_INIT(AUX_CTRL_INTF, AUX_INT_EP, AUX_OUT_EP, AUX_IN_EP, USB_BULK_EP_MPS_HS, 0x05),
    0x09, 0x04, DFU_INTF, 0x00, 0x00, 0xFE, 0x01, 0x01, 0x06,
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
    "Black Magic Firmware Upgrade",            /* iInterface 6: DFU runtime */
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
 * Microsoft OS 1.0 (WCID): WinUSB for the DFU runtime interface
 *
 * Same approach and the same device layout as ch32_hello_world, bmp-hpm-port and
 * ch32_dfu_boot (two CDC functions plus a DFU runtime interface in a
 * 0xEF/0x02/0x01 composite with no interface association descriptors): the DFU
 * runtime is the only interface advertised here, with the "WINUSB" compatible
 * ID, so Windows installs WinUSB for it automatically and "dfu-util -e" works
 * without a manual Zadig step.  The two CDC functions are deliberately NOT
 * listed, so Windows keeps its inbox usbser.sys for them and the GDB server and
 * the target UART stay COM ports.
 *
 * Windows queries:
 *   GET_DESCRIPTOR(String, index 0xEE)      -> msos_string
 *   vendor request bRequest=0x20 wIndex=4   -> msos_compat_id
 *   vendor request bRequest=0x20 wIndex=5   -> msos_ext_prop
 *
 * Windows caches the outcome per VID/PID/bcdDevice in
 * HKLM\SYSTEM\CurrentControlSet\Control\usbflags, so a failed inquiry sticks
 * until one of those changes - which is why bcdDevice is bumped whenever this
 * data changes.
 *
 * The DeviceInterfaceGUIDs extended property below is mandatory: CherryUSB
 * dereferences comp_id_property[] for the wIndex = 5 request
 * (core/usbd_core.c) and a NULL there faults in the middle of enumeration.
 * ------------------------------------------------------------------ */

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
    0x28, 0x00, 0x00, 0x00, /* dwLength = 16 + 24 * 1 */
    0x00, 0x01,             /* bcdVersion 1.0 */
    0x04, 0x00,             /* wIndex 0x0004 */
    0x01,                   /* bCount */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* reserved[7] */
    WCID_ENTRY(DFU_INTF),
};

/*
 * Extended Properties Feature Descriptor (DeviceInterfaceGUIDs), assembled at
 * init time from the plain ASCII GUID below so the UTF-16LE conversion cannot
 * be mistyped.
 *
 * Windows takes the response length from the first four bytes of this buffer,
 * so the declared length must cover exactly the bytes that are written:
 *   10 header + (4 dwSize + 4 dwPropertyDataType + 2 wPropertyNameLength
 *   + 42 L"DeviceInterfaceGUIDs" + 4 dwPropertyDataLength
 *   + 80 REG_MULTI_SZ payload) = 146
 */
#define DFU_INTERFACE_GUID    "{6d2f9a83-4c17-4e05-9b62-7a81c4f30d18}"
#define MSOS_PROP_NAME        "DeviceInterfaceGUIDs"
#define MSOS_PROP_NAME_BYTES  ((uint32_t)sizeof(MSOS_PROP_NAME) * 2U)
/* REG_MULTI_SZ payload: the GUID string, its own NUL, and the list's NUL. */
#define MSOS_PROP_DATA_BYTES  (((uint32_t)sizeof(DFU_INTERFACE_GUID) - 1U + 2U) * 2U)
#define MSOS_PROP_SECTION_LEN (4U + 4U + 2U + MSOS_PROP_NAME_BYTES + 4U + MSOS_PROP_DATA_BYTES)
#define MSOS_EXT_PROP_LEN     (10U + MSOS_PROP_SECTION_LEN)

static uint8_t msos_ext_prop[MSOS_EXT_PROP_LEN];

/* Returned for wValue != 0: a valid but empty property set. */
static const uint8_t msos_ext_prop_empty[] = {
    0x0a, 0x00, 0x00, 0x00, /* dwLength = 10 */
    0x00, 0x01,             /* bcdVersion 1.0 */
    0x05, 0x00,             /* wIndex 0x0005 */
    0x00, 0x00,             /* bCount = 0 */
};

/* CherryUSB indexes this array with setup->wValue, so keep two entries. */
static const uint8_t *msos_ext_prop_list[2];

static void msos_ext_prop_build(void)
{
    static const char prop_name[] = MSOS_PROP_NAME;
    static const char guid[] = DFU_INTERFACE_GUID;
    uint32_t p = 0U;
    uint32_t i;

    /* dwLength / wVersion(0x0100) / wIndex(0x0005) / wCount(1) */
    msos_ext_prop[p++] = (uint8_t)(MSOS_EXT_PROP_LEN);
    msos_ext_prop[p++] = (uint8_t)(MSOS_EXT_PROP_LEN >> 8);
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x01U;
    msos_ext_prop[p++] = 0x05U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x01U;
    msos_ext_prop[p++] = 0x00U;

    /* dwSize / dwPropertyDataType(REG_MULTI_SZ) / wPropertyNameLength */
    msos_ext_prop[p++] = (uint8_t)(MSOS_PROP_SECTION_LEN);
    msos_ext_prop[p++] = (uint8_t)(MSOS_PROP_SECTION_LEN >> 8);
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x07U; /* REG_MULTI_SZ */
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = (uint8_t)(MSOS_PROP_NAME_BYTES);
    msos_ext_prop[p++] = (uint8_t)(MSOS_PROP_NAME_BYTES >> 8);

    /* bPropertyName, UTF-16LE (NUL included by sizeof) */
    for (i = 0U; i < (uint32_t)sizeof(prop_name); i++) {
        msos_ext_prop[p++] = (uint8_t)prop_name[i];
        msos_ext_prop[p++] = 0x00U;
    }

    /* dwPropertyDataLength */
    msos_ext_prop[p++] = (uint8_t)(MSOS_PROP_DATA_BYTES);
    msos_ext_prop[p++] = (uint8_t)(MSOS_PROP_DATA_BYTES >> 8);
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;

    /* bPropertyData, UTF-16LE GUID */
    for (i = 0U; i < (uint32_t)(sizeof(guid) - 1U); i++) {
        msos_ext_prop[p++] = (uint8_t)guid[i];
        msos_ext_prop[p++] = 0x00U;
    }
    /* REG_MULTI_SZ terminator: end of the string, then end of the list. */
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;
    msos_ext_prop[p++] = 0x00U;

    msos_ext_prop_list[0] = msos_ext_prop;
    msos_ext_prop_list[1] = msos_ext_prop_empty;
}

static const struct usb_msosv1_descriptor msosv1 = {
    .string = msos_string,
    .vendor_code = WINUSB_VENDOR_CODE,
    .compat_id = msos_compat_id,
    .comp_id_property = msos_ext_prop_list,
};

/* ------------------------------------------------------------------ *
 * GDB data path (gdb_if.h)
 *
 * One bulk transfer at a time in each direction: the OUT pipe is re-armed once
 * its staging buffer has been drained (which flow-controls the host), the IN
 * side drains a ring buffer in USB_XFER_SIZE chunks.
 *
 * The endpoint bookkeeping and the ring indices are shared with the USB
 * interrupt callbacks below, and both sides arm the next transfer when they see
 * the previous one has finished.  That check-then-arm has to be serialised:
 * without it a completion callback landing inside the GDB loop's version arms
 * the same endpoint twice (the second start_write is rejected) and the chunk the
 * loop just filled can end up never being sent - which the host sees as a reply
 * that stops mid-packet, i.e. "Ignoring packet error, continuing..." followed by
 * "Truncated register N in remote 'g' packet" when it was a register read.
 * Only the USB interrupt is masked here, so the bit-banged SWD/JTAG timing is
 * not affected (the callbacks cannot preempt themselves either).
 * ------------------------------------------------------------------ */
static inline void gdb_usb_enter(void)
{
    NVIC_DisableIRQ(USBHS_IRQn);
}

static inline void gdb_usb_exit(void)
{
    NVIC_EnableIRQ(USBHS_IRQn);
}

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
    int c = -1;

    gdb_usb_enter();
    if (gdb_out_pos >= gdb_out_len) {
        gdb_out_arm();
    } else {
        c = gdb_out_xfer[gdb_out_pos++];
    }
    gdb_usb_exit();

    return c;
}

static void gdb_in_send(void)
{
    if (gdb_in_len == 0U) {
        return;
    }

    if (gdb_in_len > sizeof(gdb_in_xfer)) {
        /* Unreachable: the build keeps every reply inside one USB packet and
         * gdb_if_putchar() stages at most GDB_IN_BUF_SIZE bytes.  Drop rather
         * than hand the controller a length it would read past the buffer. */
        gdb_in_len = 0U;
        return;
    }

    gdb_usb_enter();
    for (uint32_t i = 0U; i < gdb_in_len; i++) {
        gdb_in_xfer[i] = gdb_in_buf[i];
    }
    const uint32_t length = gdb_in_len;
    gdb_in_len = 0U;
    if (usbd_ep_start_write(0, GDB_IN_EP, gdb_in_xfer, length) < 0) {
        /* Nothing was queued (e.g. the endpoint is not enabled yet): the reply
         * is lost, which is no worse than re-sending a packet whose beginning
         * the host has already parsed. */
        gdb_usb_exit();
        return;
    }
    gdb_in_busy = true;
    gdb_usb_exit();

    /*
     * Wait for the completion callback, but never for correctness and never for
     * long: a reply is a single packet, so the controller has already put it on
     * the wire on its own.  This wait only keeps the next reply from
     * overwriting gdb_in_xfer while this one is still being read out.
     */
    const uint32_t start = board_time_ms();
    while (gdb_in_busy && ((uint32_t)(board_time_ms() - start) < 5U)) {
    }
    gdb_in_busy = false;
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
    if (gdb_in_len < sizeof(gdb_in_buf)) {
        gdb_in_buf[gdb_in_len++] = (uint8_t)c;
    }
    /* An over-long packet cannot happen: gdb_put_packet() bounds the payload to
     * GDB_PACKET_BUFFER_SIZE and the buffer above is sized for one of those plus
     * its framing. */
    if (flush) {
        gdb_in_send();
    }
}

void gdb_if_flush(bool force)
{
    (void)force;
    gdb_in_send();
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
    /* Same check-then-arm serialisation as the GDB endpoint bookkeeping. */
    gdb_usb_enter();
    if (!aux_in_busy) {
        aux_in_busy = true;
        usbd_ep_start_write(0, AUX_IN_EP, aux_in_xfer, count);
    }
    gdb_usb_exit();
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
            gdb_in_len = 0U;
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
        /* MS OS 1.0 (WCID) so Windows installs WinUSB for the DFU runtime
         * interface without a manual Zadig step (see the comment above those
         * descriptors). */
        .msosv1_descriptor                  = &msosv1,
        .msosv2_descriptor                  = NULL,
        .bos_descriptor                     = NULL,
    };

    build_serial_string();
    aux_serial_init();

    /* Assemble the MS OS 1.0 extended properties (DeviceInterfaceGUIDs) for the
     * DFU runtime interface before the descriptors are registered. */
    msos_ext_prop_build();

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
