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
boards/ch32v30x_ob/      board BSP
linkfile/flash_dfu.ld     application linker script (0x00008000, 96K)
shared/boot_protocol.h    bootloader/application contract (partition, BKP)
docs/                     development notes (problem records, design notes)
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

nTRST / nSRST are not wired (empty implementations). The SWD/JTAG pin definitions
live in `bmp_port/jtag_port.h`; the status LED and BOOT button are board pins
(`BOARD_LED_*` / `BOARD_BOOT_*` in `boards/ch32v30x_ob/board_config.h`) reached
through the board primitives `board_led_write()` / `board_read_boot_pin()`.

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

The second CDC function (interfaces 2/3) is wired to USART3 (PB10/PB11). The UART
*hardware* is described by the board, not the port: instance, pins and clocks are
the `BOARD_APP_UART*` macros in `boards/ch32v30x_ob/board_config.h`, brought up by
`board_init_app_uart()`. The port owns the line format and everything above the
wire. The whole path is **DMA + interrupt driven** and never depends on the main
loop:

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
  - holding the BOOT button (PA6): the probe registers a 100 ms tick through
    `board_timer_create()` (`bmp_port/platform.c`), which the board's TIM3
    interrupt calls, so it works while the main loop is parked in
    `gdb_if_getchar()` waiting for a GDB command

## Build

```sh
cmake --preset ch32v30x_ob-release
cmake --build --preset ch32v30x_ob-release
```

Artifacts: `build/ch32v30x_ob-release/bmp-ch32v30x-port.{elf,hex,bin}`.

## Flashing / usage

1. Flash a matching DFU bootloader first (it owns its own 32 KB region at
   `0x08000000`).
2. Write this application. Because it exposes a **DFU runtime interface** (interface 4),
   the **same single command works in both states** - you never have to find out which
   firmware the device is running first:

   ```sh
   dfu-util -d 1a86 -s 0x08008000:leave -D build/ch32v30x_ob-release/bmp-ch32v30x-port.bin
   ```

   - **Device already in the bootloader's DFU mode** (entered by holding BOOT while
     powering up) - plain download.
   - **Device running this application** (`1a86:6018`) - `dfu-util` sends DFU_DETACH
     before the download; the runtime interface hands it to `platform_request_boot()`,
     which writes the BKP hand-shake and resets, so the bootloader comes up in DFU mode
     and the command flashes the image.
   - `-d 1a86` matches by VID only (`dfu-util` also matches DFU-mode devices when only
     run-time IDs are given), which covers the application (`1a86:6018`) and the
     bootloader (`1a86:df11`) alike, so the switch of mode does not lose the device.
   - To only send a running application back to the bootloader without flashing, use
     `dfu-util -e` on its own.

3. Then connect with GDB:
   ```sh
   arm-none-eabi-gdb -ex 'target extended-remote /dev/ttyACM0'   # Linux
   # Windows: COMx / WinUSB, depending on your GDB build
   ```
4. To go back to the bootloader: `dfu-util -e` (or `monitor bootloader` in GDB).

## Development notes

Problem records and design notes live under `docs/`, each in English and Chinese.
This README is the engineering overview only; the reasoning lives in the notes:

- [`docs/gdb-reply-path.md`](docs/gdb-reply-path.md) - the CH32V30x USBHS IN
  transfer problem behind the long GDB reply stalls, the reply-size limit that
  keeps every reply inside one USB packet (`GDB_PACKET_BUFFER_SIZE = 496`), and the
  residual risks.
- [`docs/jtag-clock-duty-cycle.md`](docs/jtag-clock-duty-cycle.md) - why the JTAG
  clocks that measured ~86% duty cycle did so (the tail bits of every bulk shift,
  and the single-clock paths), and how the shift loops are arranged for 50%.

## Notes

- Submodules, both under `third_party_components/`: `blackmagic`
  (`https://codeberg.org/mTOTm/blackmagic.git`, `dev/jiance.zhang/main_test`) and
  `CherryUSB` (`ch32v30x-usbhs`, containing the CH32V30x USBHS device controller
  port).
- The target driver list is trimmed in `bmp_port/CMakeLists.txt` to fit the 96 KB
  application partition; add files there to support more chips.
