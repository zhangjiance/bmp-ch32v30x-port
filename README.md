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
blackmagic/               submodule: BMP core and target drivers
third_party_components/CherryUSB/   submodule: USB stack + CH32V30x USBHS port
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
| Status LED | PA8 |
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
- `bcdUSB` = 2.0, `bcdDevice` = `0x0103`. `bcdDevice` is part of the Windows
  hardware ID and of the usbflags cache key, so bump it whenever the descriptors
  or the WCID data change, to force Windows to re-enumerate and reinstall.
- **No RTT**: this port exposes the GDB port and the target UART only.

## Bootloader integration

- The application links at `0x00008000` (`linkfile/flash_dfu.ld`, 96 KB), matching
  `BOOT_APP_OFFSET` in `shared/boot_protocol.h`.
- Two paths return to the bootloader, both going through the BKP handshake in
  `bmp_port/boot_trigger_ch32v30x.c` (it survives `NVIC_SystemReset()`):
  - `dfu-util -e` (DFU_DETACH) calls `platform_request_boot()`
  - `monitor bootloader` from the GDB side

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

## Known issue: long GDB replies stall (worked around, proper fix pending)

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

### Cause

1. **Protocol**: after an attach GDB immediately reads the RISC-V target
   description XML (about 4.5 KB for this target), in chunks sized by the
   `PacketSize` the stub announces. At a packet size of 2048 the first chunk is
   exactly 2048 bytes on the wire.
2. **Controller**: the CH32V30x USBHS IN endpoint only takes one max-packet-size
   chunk (512 bytes) per `usbd_ep_start_write()` (it uses `MIN(len, ep_mps)`);
   everything beyond that has to be continued from the USBHS interrupt, and the
   completion callback comes from that same interrupt.
3. **Port**: the old `gdb_in_send()` handed the packet over and then waited,
   giving up and clearing its state after **250 ms**. The controller had therefore
   sent only the first 512 bytes while the driver still considered the transfer
   unfinished; the next reply called `usbd_ep_start_write()` again, which resets
   the driver's `xfer_buf`/`xfer_len`, and the remaining packets only left once the
   interrupt was serviced (that is, "after the next host packet"). GDB then treated
   the late XML as the answer to its next `$g` request (`Bad register packet`) and
   measured the 33 x 4 byte register packet with its built-in register table
   (`Truncated register 22`).

This is **not** a bit-banging (SWD/JTAG) problem: building that XML is pure string
formatting and does not touch the target, while short replies over the very same
bit-banged path always worked.

### Current workaround

- `bmp_port/CMakeLists.txt`: `GDB_PACKET_BUFFER_SIZE=496`, so a reply plus its
  framing is at most 500 bytes < 512 and **no reply ever crosses a USB packet**,
  which removes the need for the driver's continuation altogether. A build-time
  `#error` in `bmp_port/cdc_acm_dual.c` guards that invariant.
- The send path copies the reply into a dedicated in-flight buffer and lets the
  controller put it on the wire; the remaining 5 ms wait is only there to avoid
  overwriting a buffer that is still being read out. Correctness does not depend on
  the completion callback, and the driver's transfer state is never reset.
- **Client requirement**: a single `m` (hex memory read) must not exceed
  `GDB_PACKET_BUFFER_SIZE / 2` (currently **248 bytes**; the check is
  `len > GDB_PACKET_BUFFER_SIZE / 2U` in `blackmagic/src/gdb_main.c`), otherwise
  the stub answers `E02`. GDB itself chunks its requests by the announced
  `PacketSize`, but a client that speaks the protocol on its own has to do the
  same - a browser flasher that reads back in fixed 256 byte chunks sees a flood
  of `E02` at 496. Also watch out for clients that fill the CRC with source data
  for the chunks they could not read back: their checksums then match without
  anything having been verified.

Cost: `m`/`X` move about 230 bytes per round trip instead of about 1020, so
`load`/`dump` need roughly 4x as many round trips (about 0.5 s more for 100 KB,
which is negligible); RAM usage drops by about 8 KB.

### Proper fix (TODO)

1. **Driver** (`third_party_components/CherryUSB/port/wch/ch32v30x/usb_dc_ch32v30x.c`):
   `USBD_IRQHandler` handles only one endpoint per interrupt (the one in `INT_ST`)
   and then clears the global `USBHS_TRANSFER_FLAG`. With enough endpoints (this
   device has three CDC functions plus DFU, and its OUT endpoint is always armed)
   an IN completion can get lost. Handle all pending endpoints in a loop
   (`while (USBHS_DEVICE->INT_FG & USBHS_TRANSFER_FLAG)`).
   Note that multi-packet OUT transfers do work on this controller (the DFU
   bootloader receives 4 KB transfers), so only the IN direction is unreliable.
2. **Port**: do not reset the driver's transfer state on timeout (or wait forever).
3. **Instrumentation**: add counters in the driver (continuations, completion
   callbacks, dropped events) and print them from a `mon` command or the target
   UART, so the event that gets lost can be identified before changing any code.
4. Once fixed, raise `GDB_PACKET_BUFFER_SIZE` back to 2048 (or more) to cut down
   the number of round trips for `load`/`dump`.
5. Unrelated small improvement: the DMI `RV_DMI_TOO_SOON` retry loops in
   `blackmagic`'s `riscv_debug.c` are unbounded, so a marginal link shows up as a
   hang with no message; a bounded retry plus an error message locates it much
   faster.

## Notes

- Submodules: `blackmagic` (`https://codeberg.org/mTOTm/blackmagic.git`,
  `dev/jiance.zhang/main_test`) and `third_party_components/CherryUSB`
  (`ch32v30x-usbhs`, containing the CH32V30x USBHS device controller port).
- The target driver list is trimmed in `bmp_port/CMakeLists.txt` to fit the 96 KB
  application partition; add files there to support more chips.
