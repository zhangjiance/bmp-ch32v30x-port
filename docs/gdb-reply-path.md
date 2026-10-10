# GDB reply path: the USBHS IN transfer problem and the reply-size limit

[English](gdb-reply-path.md) | [简体中文](gdb-reply-path.zh-CN.md)

[Back to the project README](../README.md)

Development note. This is the record of the long-standing "long GDB replies stall"
problem on the CH32V30x USBHS port, the fix, and the reply-size limit that finally
removed the failures. The README only carries the engineering overview; the
reasoning lives here.

## Symptom

Two failure modes were observed on the GDB CDC interface, both looking like a
stalled IN transfer.

**(a) `att 1`, or anything else that produces a long reply, fails:**

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

**(b) Flash readback (`m` reads) intermittently stalls.** The host either receives
**no reply bytes at all** within a whole timeout window, or receives a byte stream
that fails the GDB frame checksum. It is *not* target dependent: whenever the data
did arrive, its CRC32 matched the source image exactly. It also looked
speed-dependent at first (a fixed probe clock appeared to fix it) - that was a red
herring, see the reply-size limit below.

## Root cause

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
formatting and does not touch the target, short replies over the very same
bit-banged path always worked, and failure mode (b) reproduced on a plain `m` read
whose reply never depended on target timing. The SWD/JTAG clock and its duty cycle
were measured separately and are unrelated to this.

## Fix

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
  - `GDB_PACKET_BUFFER_SIZE` is now **496**, so every reply stays inside one USB
    packet - see "The reply-size limit" below.
  - **Full-size replies end with a ZLP** (`c957249` `fix: terminate full-size GDB replies
    with a ZLP`): a bulk transfer is only terminated by a **short packet**, and
    `usbd_ep_start_write()` never appends a zero-length packet by itself. A reply whose
    length is an exact multiple of `USB_XFER_SIZE` (512 bytes) therefore ends on a
    full-size packet, the host assumes more data is coming and the whole reply is only
    handed up at the **next host packet**. GDB hits this exactly:
    `qXfer:features:read:target.xml:0,7fb` frames as `$` + `m` + 2043 + `#` + 2 checksum
    bytes = 2048 = 4 x 512, which is the direct cause of the hang after `attach`. The
    send path now checks whether the length is a multiple of `USB_XFER_SIZE` and, if so,
    sends a terminating **zero-length packet** (ZLP) and waits for it to complete.
- **Client requirement**: a single `m` (hex memory read) must not exceed
  `GDB_PACKET_BUFFER_SIZE / 2`, now **248 bytes** (the check is
  `len > GDB_PACKET_BUFFER_SIZE / 2U` in
  `third_party_components/blackmagic/src/gdb_main.c`), otherwise
  the stub answers `E02`. Clients that chunk by the announced `PacketSize` (libgdb,
  and the webui under `static/blackmagic-utils/`) adapt by themselves; a client that
  speaks the protocol by hand has to respect it or parse `PacketSize`. Also watch
  out for clients that fill the CRC with source data for the chunks they could not
  read back: their checksums then match without anything having been verified.

## The reply-size limit: why `GDB_PACKET_BUFFER_SIZE` is 496

This is what actually removed the failures. A reply is framed as
`'$' + payload + '#' + 2 checksum bytes`, so

```
wire length of a reply  =  GDB_PACKET_BUFFER_SIZE + 4
fits one 512 byte USB packet     <=>  GDB_PACKET_BUFFER_SIZE <= 508
also stays off the ZLP path      <=>  GDB_PACKET_BUFFER_SIZE <= 507
```

- **496** (496 + 4 = 500 < 512) keeps every reply inside a single USB packet *and*
  away from the ZLP path. `gdb_in_send()` then degenerates to exactly one
  `usbd_ep_start_write()` and no ZLP, every single time: the chunked continuation
  path is not merely unlikely, it is **unreachable**.
- This is not a workaround for a broken controller - it is the intended way to use
  this controller, see the next section.
- The cost is the read size (248 instead of 512/1024 bytes per `m`) and therefore a
  few hundred extra round trips for a full memory dump.

## Why one packet at a time: this controller has no TX burst

- CherryUSB's own documentation splits USB IP into **hardware packetization** (the
  IP owns DMA / descriptor DMA and splits a transfer itself) and **software
  packetization** (the IP only has a FIFO and the driver must hand it one packet at
  a time). The CH32V30x USBHS device controller is the second kind - that is the
  "no multi-packet burst" note above, and why the newer IP behind `port/wch/usbhs`
  has `UEP_TX_BURST` while this one does not.
- WCH's own documentation states the contract plainly: the MCU prepares the data and
  the length, the hardware uploads **one packet** when the host sends an IN token,
  and the IN interrupt then sets NAK and flips the toggle. Their examples keep a
  **busy** flag for exactly this, cleared **in the IN interrupt**, and the
  documented pitfall is that *every* place which sets the endpoint to NAK must also
  clear that flag. Long replies are explicitly the caller's problem ("if the
  descriptor is long the host fetches it in several IN packets and the remaining
  data has to be prepared in the IN interrupt"), and so are zero-length packets.

Other BMP ports, for comparison:

| Port | `GDB_PACKET_BUFFER_SIZE` | Endpoint MPS | Packets per reply |
|---|---|---|---|
| upstream BMP (STM32F103 native) | 1024 | 64 (full speed) | up to 17 - the STM32 IP continues them, fine |
| `bmp-hpm-port` (HPM5301) | 16384 | 512 (high speed) | up to 33 - hardware packetization, fine |
| `ch32v305_dap` DAPLink | n/a (`DAP_PACKET_SIZE` 512) | 512 | **always exactly 1** |
| **this port** | **496** | 512 | **always exactly 1** |

DAPLink on the very same controller avoids this by construction: its protocol packet
is 512 bytes = the endpoint MPS, so the continuation branch is never taken there
either.

## Why the missing-terminator case only showed up on Windows

The very same reply has always been fine on Linux, including a device attached into WSL
through `usbipd` - because what decides "when is an IN transfer finished" is the **host
side USB stack**, and Linux and Windows do it differently:

- **Linux**: `cdc_acm` sizes every bulk IN URB buffer to exactly the endpoint's max
  packet size (`acm->readsize = wMaxPacketSize`, 512 bytes here), so one full-size packet
  from the device already fills the whole URB, the URB completes immediately and the data
  is handed to userspace right away - it never needs that zero-length packet. `usbfs` /
  libusb behave the same as long as you read a fixed number of bytes.
- **Windows**: `usbser.sys` (COM port) and WinUSB both submit one bulk IN request for a
  **length of their own choosing** and only consider the transfer finished when that
  length is filled or a short packet arrives. With a reply that is an exact multiple of
  512, the tail carries no short packet, the request never completes and the data is only
  released when a later command supplies a short packet - which is why reading a whole URB
  at a time (usbser.sys/WinUSB) exposes the bug most reliably.
- **Why WSL + usbip is fine too**: with usbip the URBs are still created by Linux
  (`vhci_hcd` + `cdc_acm`, still 512 bytes each) and merely executed on Windows, so the
  transfer-termination semantics stay Linux's and the problem never appears.

Note this only covers the *terminator* case (a reply whose length is an exact multiple
of 512 bytes). The **continuation** failure is host independent: with
`GDB_PACKET_BUFFER_SIZE = 2048` a 256 byte `m` reply is 516 = 512 + 4 bytes, and the
readback stalls reproduced from a Chrome/Web Serial client on Linux just as well.
Keeping replies inside one packet avoids both cases.

## Residual risks

- `gdb_in_send()` still **drops the whole reply** when a chunk's completion does not
  arrive within `GDB_IN_XFER_TIMEOUT_MS` (1000 ms) - it sets `gdb_in_len = 0`, so a
  hiccup on this controller becomes "the client got nothing" instead of "one retry".
  Re-arming the same chunk (bounded) would self-heal instead. With 496 the exposure
  is one chunk per reply instead of 2..9, which is why it no longer shows.
- `usbd_event_handler()` handles `USBD_EVENT_RESET` but not `USBD_EVENT_DISCONNECTED`.
  Nothing blocks unboundedly today, so this is only a precaution.
- If a reply ever has to exceed one packet again, **fix the continuation path rather
  than raising `GDB_PACKET_BUFFER_SIZE`**. The pattern that works on this controller
  (see `ch32v305_dap`) is to send whole protocol packets from an upper-layer queue
  guarded by an idle flag, instead of continuing one long transfer from the IN
  interrupt.
- The DMI `RV_DMI_TOO_SOON` retry loops in the blackmagic submodule's
  `riscv_debug.c` are unbounded, so a marginal link shows up as a hang with no message; a bounded
  retry plus an error message locates it much faster.
- `usbd_get_port_speed()` in this driver hard-codes `USB_SPEED_HIGH` instead of
  reading `USBHS_DEVICE->SPEED_TYPE`, so a fallback to full speed would still be
  served the high-speed descriptors (with their illegal 512 byte bulk MPS).
