# bmp-ch32v30x-port

[English](README.md) | [简体中文](README.zh-CN.md)

Black Magic Probe (BMP) ported to the CH32V30x, driving its high-speed USB
device controller (USBHS). The firmware is built as the **application image** of
a DFU bootloader: the bootloader owns the first 32 KB of flash and this
application starts at `0x08008000`.

```
CMakeLists.txt            top level: toolchain, board selection, link, objcopy
main.c                    application entry point
CMakePresets.json         release / debug presets
bmp_port/                 port layer: USB, bit-banged SWD/JTAG, target UART,
                          timing, bootloader handshake, MCU glue
bmp_port/CMakeLists.txt   port sources + blackmagic sources and includes
cmake/wch_riscv.cmake     toolchain file (riscv-wch-elf-)
third_party_components/
  blackmagic/             submodule: BMP core and target drivers
  CherryUSB/              submodule: USB stack + CH32V30x USBHS port
SDK/                      WCH ch32v30x peripheral library
boards/ch32v30x_bmp/      board BSP
linkfile/flash_dfu.ld     application linker script (0x00008000, 96K)
shared/boot_protocol.h    bootloader/application contract (partition, BKP)
```

## Pins

| Function | Pin |
|---|---|
| SWCLK / TCK | PB14 |
| SWDIO / TMS | PB15 |
| TDI | PB13 |
| TDO | PB12 |
| Status LED (active low) | PA5 |
| BOOT button (to GND, active low) | PA6 |
| Target UART TX (USART3) | PB10 |
| Target UART RX (USART3) | PB11 |

nTRST / nSRST are not wired (empty implementations). The pin definitions live in
`bmp_port/jtag_port.h`.

SWD/JTAG is **GPIO bit-banging only** (the `PIN_*` macros in
`bmp_port/jtag_port.h`); there is no SPI acceleration path, so the timing stays
fully deterministic.

## USB device (composite, 1a86:6018)

| Interface | Function |
|---|---|
| 0 / 1 | GDB server (CDC ACM) |
| 2 / 3 | target UART (CDC ACM, USART3) |
| 4 | DFU runtime (no endpoints) |

- **The DFU runtime interface (4) announces itself through Microsoft OS 1.0
  descriptors (WCID)**: the 0xEE string plus `bRequest = 0x20`, Compatible ID
  `WINUSB`, and the `DeviceInterfaceGUIDs` extended property (required - CherryUSB
  dereferences `comp_id_property` for `wIndex = 5`). Windows then **binds WinUSB
  automatically**, so `dfu-util -e` works out of the box and no Zadig is needed.
- Windows caches the WCID result per `VID/PID/bcdDevice` in
  `HKLM\SYSTEM\CurrentControlSet\Control\usbflags`, so `bcdDevice` (or the VID/PID)
  has to change to make it ask again.
- The two CDC functions (GDB and target UART) are **deliberately not listed** in
  the WCID data, so Windows keeps using the inbox usbser.sys driver and they stay
  COM ports.
- The DFU interface `iInterface` points at string 6,
  `"Black Magic Firmware Upgrade"`, so `dfu-util -l` shows a proper `name=`
  instead of `UNKNOWN`.
- VID is WCH's `0x1A86` (same vendor as the matching DFU bootloader's
  `1A86:DF11`); the PID stays Black Magic's `0x6018`.
- `bcdUSB` = 2.0, `bcdDevice` = `0x0104`. `bcdDevice` is part of the Windows
  hardware ID and of the usbflags cache key, so bump it whenever the descriptors
  or the WCID data change, to force Windows to re-enumerate and reinstall.
- **No RTT**: this port exposes the GDB port and the target UART only.

## Target UART (USB2UART)

The second CDC function (interfaces 2/3) is wired to USART3 (PB10/PB11). The whole
path is **DMA + interrupt driven** and never depends on the main loop:

- **target → host**: USART3 RX is written into a 256 byte buffer in a circle by DMA1
  channel 3; the USART **idle interrupt** and the DMA half/full transfer interrupts
  hand whatever arrived straight to the IN endpoint through a USB sink, and the
  endpoint's completion callback (`aux_serial_usb_ready()`) continues with the rest.
- **host → target**: the USB OUT callback queues into a 1 KB ring and DMA1 channel 2
  sends one contiguous run at a time, its transfer complete interrupt picking up the
  next one.
- **No DTR gating**: data is forwarded as long as the host is reading, so terminals
  that never assert DTR work too.
- The UART therefore keeps flowing while a long GDB command is in progress; the old
  implementation forwarded it from the `gdb_if_getchar()` idle loop, which stalled.

## Bootloader integration

- The application links at `0x00008000` (`linkfile/flash_dfu.ld`, 96 KB), matching
  `BOOT_APP_OFFSET` in `shared/boot_protocol.h`.
- Three paths return to the bootloader, all going through the BKP handshake in
  `bmp_port/boot_trigger_ch32v30x.c` (it survives `NVIC_SystemReset()`):
  - `dfu-util -e` (DFU_DETACH) calls `platform_request_boot()`
  - `monitor bootloader` from the GDB side
  - holding the BOOT button (PA6): the 100 ms TIM3 interrupt in
    `boards/ch32v30x_bmp/board.c` samples it, so it works while the main loop is
    parked in `gdb_if_getchar()` waiting for a GDB command

## Build

```sh
cmake --preset ch32v30x_bmp-release
cmake --build --preset ch32v30x_bmp-release
```

Artifacts: `build/ch32v30x_bmp-release/bmp-ch32v30x-port.{elf,hex,bin}`.

## Flashing / usage

1. Flash a matching DFU bootloader first (it owns its own 32 KB region at
   `0x08000000`).
2. Hold BOOT while powering up to enter DFU mode, then write this application:
   ```sh
   dfu-util -d 1a86 -s 0x08008000:leave -D build/ch32v30x_bmp-release/bmp-ch32v30x-port.bin
   ```
3. Then connect with GDB:
   ```sh
   arm-none-eabi-gdb -ex 'target extended-remote /dev/ttyACM0'   # Linux
   # Windows: COMx / WinUSB, depending on your GDB build
   ```
4. To go back to the bootloader: `dfu-util -e` (or `monitor bootloader` in GDB).

## Fixed: long GDB replies stalled (root cause)

### Symptom

`att 1` (or anything else that produces a long reply) fails:

```
$qXfer:features:read:target.xml:0,7fb
getpkt: Timed out.
Ignoring packet error, continuing...
...
Bad register packet; fetching a new packet
Truncated register 22 in remote 'g' packet
```

Short replies (`mon jt`, `mon swd_scan`) work, so it looks like "the target is
detected but attach fails". Raising the GDB timeout (`set remotetimeout 20`) does
not help either: the reply only shows up after the **next host packet**.

### Root cause

Yes - this was a **CherryUSB CH32V30x USBHS device-controller port bug**
(`third_party_components/CherryUSB/port/wch/ch32v30x/usb_dc_ch32v30x.c`), in the
IN direction only, plus a port-layer bug that kept triggering it.

1. **Controller**: the CH32V30x USBHS has no multi-packet burst (unlike the
   newer IP behind `port/wch/usbhs`, which has `UEP_TX_BURST`). `usbd_ep_start_write()`
   loads only `MIN(len, ep_mps)` = 512 bytes; the rest has to be continued from
   `USBD_IRQHandler()`, so one transfer lives across several interrupts.
2. **Driver: no in-flight guard.** `usbd_ep_start_write()` happily accepted a new
   transfer while the previous one was still on the wire. It overwrote
   `xfer_buf`/`xfer_len`/`actual_xfer_len` and re-armed `UEPn_TX_LEN`/`TX_DMA`/
   `TX_CTRL`, i.e. it restarted the data-toggle sequence of a transfer the host was
   still reading. The tail of the old transfer is then stranded on the endpoint and
   only leaves at the next USB event - exactly the observed "the reply shows up
   after the next host packet". Upstream `port/wch/usbhs` rejects this case with
   `-4`; the CH32V30x port did not check anything.
3. **Driver: one event per interrupt.** `USBD_IRQHandler()` snapshotted `INT_FG`,
   serviced exactly one endpoint and cleared the global `USBHS_TRANSFER_FLAG`.
   Anything latched while that flag was pending was dropped. On this composite
   device (two CDC functions plus DFU, both bulk OUT endpoints permanently armed)
   that loses IN completions and leaves a transfer unfinished.
4. **Port: a timed wait that reset its own state.** The old `gdb_in_send()` handed
   the packet over, waited, and cleared `gdb_in_len` after **250 ms** - and the
   intermediate version waited only **5 ms** - then re-armed the endpoint. Both
   feed straight into 2.
5. **Port: arming an IN endpoint from the interrupt.** The old completion callback
   called `gdb_in_kick()` inside `USBD_IRQHandler()`, so chunks 2..n of a reply were
   queued from interrupt context. That is the path that proved unreliable on this
   controller. Multi-packet OUT *is* fine (the DFU bootloader receives 4 KB
   transfers), so only the IN direction is affected.

This is **not** a bit-banging (SWD/JTAG) problem: building that XML is pure string
formatting and does not touch the target, while short replies over the very same
bit-banged path always worked.

### Fix

- **Driver** (`usb_dc_ch32v30x.c`)
  - `usbd_ep_start_write()` returns `-4` when the endpoint still owns a transfer,
    so a running transfer can no longer be clobbered. The flag is released when
    the transaction completes (before the completion callback, which is allowed to
    queue the next one), on stall, and on a new SETUP.
  - `USBD_IRQHandler()` drains every latched event in a loop (transfer / SETUP /
    bus reset) instead of one per interrupt, and latches `INT_ST` once - the
    `TOG_OK` bit used to be re-read *after* `INT_FG` had been cleared.
  - Event counters in `g_ch32_usbhs_stats` (arming, busy rejects, IN transactions,
    continuations, completions, OUT, SETUP/reset, loop overflow). Declare
    `extern struct ch32_usbhs_stats g_ch32_usbhs_stats;` in a debug build to dump
    them before changing any code next time.
- **Port** (`bmp_port/cdc_acm_dual.c`)
  - `gdb_in_send()` cuts a reply into `USB_XFER_SIZE` (512 byte) chunks, arms one
    chunk at a time **from thread context** and waits for that chunk's completion.
    No reply depends on the driver's interrupt continuation any more, and the
    completion callback only clears the busy flag.
  - `GDB_PACKET_BUFFER_SIZE` is back at **2048**.
- **Client requirement**: a single `m` (hex memory read) must still not exceed
  `GDB_PACKET_BUFFER_SIZE / 2`, now **1024 bytes** (the check is
  `len > GDB_PACKET_BUFFER_SIZE / 2U` in
  `third_party_components/blackmagic/src/gdb_main.c`), otherwise
  the stub answers `E02`. GDB chunks by the announced `PacketSize` itself, but a
  client that speaks the protocol on its own has to respect it or parse
  `PacketSize`. Also watch out for clients that fill the CRC with source data for
  the chunks they could not read back: their checksums then match without anything
  having been verified.

### Still open

- The DMI `RV_DMI_TOO_SOON` retry loops in the blackmagic submodule's
  `riscv_debug.c` are unbounded, so a marginal link shows up as a hang with no message; a bounded
  retry plus an error message locates it much faster.
- `usbd_get_port_speed()` in this driver hard-codes `USB_SPEED_HIGH` instead of
  reading `USBHS_DEVICE->SPEED_TYPE`, so a fallback to full speed would still be
  served the high-speed descriptors (with their illegal 512 byte bulk MPS).

## Notes

- Submodules, both under `third_party_components/`: `blackmagic`
  (`https://codeberg.org/mTOTm/blackmagic.git`, `dev/jiance.zhang/main_test`) and
  `CherryUSB` (`ch32v30x-usbhs`, containing the CH32V30x USBHS device controller
  port).
- The target driver list is trimmed in `bmp_port/CMakeLists.txt` to fit the 96 KB
  application partition; add files there to support more chips.
