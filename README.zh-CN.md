# bmp-ch32v30x-port

[English](README.md) | [简体中文](README.zh-CN.md)

Black Magic Probe（BMP）在 CH32V30x 上的端口工程，使用其高速 USB 设备控制器（USBHS）。
固件作为 DFU bootloader 的**应用程序镜像**运行：bootloader 占前 32 KB，本应用从
`0x08008000` 开始。

```
CMakeLists.txt            顶层：工具链、板级选择、链接、objcopy
main.c                    应用入口
CMakePresets.json         release / debug 预设
bmp_port/                 端口层：USB、GPIO 位操作 SWD/JTAG、目标串口、定时、boot 握手、MCU glue
bmp_port/CMakeLists.txt   端口源清单 + blackmagic 源清单与 include
cmake/wch_riscv.cmake     工具链（riscv-wch-elf-）
blackmagic/               子仓：BMP 核心与 target 驱动
third_party_components/CherryUSB/  子仓：USB 协议栈 + CH32V30x USBHS 端口
SDK/                      WCH ch32v30x 外设库
boards/ch32v30x_bmp/      板级 BSP
linkfile/flash_dfu.ld     应用链接脚本（0x00008000，96K）
shared/boot_protocol.h    boot 与 app 的约定（分区、BKP 触发）
```

## 引脚

| 功能 | 引脚 |
|---|---|
| SWCLK / TCK | PB14 |
| SWDIO / TMS | PB15 |
| TDI | PB13 |
| TDO | PB12 |
| 状态 LED | PA8 |
| 目标串口 TX (USART3) | PB10 |
| 目标串口 RX (USART3) | PB11 |

nTRST / nSRST 未接线（空实现）。引脚定义在 `bmp_port/jtag_port.h`。

SWD/JTAG 只做 **GPIO 位操作**（`bmp_port/jtag_port.h` 提供 `PIN_*` 宏），没有 SPI 加速
路径，时序完全确定。

## USB 设备（复合设备，1a86:6018）

| 接口 | 功能 |
|---|---|
| 0 / 1 | GDB server（CDC ACM） |
| 2 / 3 | 目标串口 UART（CDC ACM，USART3） |
| 4 | DFU runtime（无端点） |

- **DFU runtime（接口 4）用 Microsoft OS 1.0（WCID）描述符**：0xEE 字符串 +
  `bRequest = 0x20`，Compatible ID = `WINUSB`，并带 `DeviceInterfaceGUIDs` 扩展属性
  （必须，CherryUSB 对 `wIndex = 5` 会直接解引用 `comp_id_property`）。Windows
  **自动装 WinUSB**，`dfu-util -e` 开箱可用，不需要 Zadig。
- Windows 把 WCID 结果按 `VID/PID/bcdDevice` 缓存在
  `HKLM\SYSTEM\CurrentControlSet\Control\usbflags`，所以**改 `bcdDevice`（或 VID/PID）
  才会让它重新询问**。
- GDB / 目标串口两个 CDC 功能**故意不列入 WCID**，Windows 继续用 inbox usbser.sys，
  因此它们仍是 COM 口。
- DFU 接口的 `iInterface` 指向字符串 6 = `"Black Magic Firmware Upgrade"`，所以
  `dfu-util -l` 里 `name=` 不再显示 `UNKNOWN`。
- VID 用 WCH 的 `0x1A86`（与配套 DFU bootloader 的 `1A86:DF11` 同厂商），PID 保留
  Black Magic 的 `0x6018`。
- `bcdUSB` = 2.0；`bcdDevice` = `0x0103`。`bcdDevice` 属于 Windows 硬件 ID 与 usbflags
  缓存键，每次改描述符/WCID 数据就 +1，用来强制 Windows 重新枚举并重装。
- **不含 RTT**：本端口只提供 GDB 口和目标串口。

## 与 bootloader 配合

- 应用链接在 `0x00008000`（`linkfile/flash_dfu.ld`，96 KB），与
  `shared/boot_protocol.h` 里的 `BOOT_APP_OFFSET` 一致。
- 回到 bootloader 有两条路径，都走 `bmp_port/boot_trigger_ch32v30x.c` 的
  BKP 握手（跨 `NVIC_SystemReset()` 保留）：
  - `dfu-util -e`（DFU_DETACH）→ `platform_request_boot()`
  - GDB 侧 `monitor bootloader`

## 构建

```sh
cmake --preset ch32v30x_bmp-release
cmake --build --preset ch32v30x_bmp-release
```

产物：`build/ch32v30x_bmp-release/bmp-ch32v30x-port.{elf,hex,bin}`。

## 烧录 / 使用

1. 先烧配套的 DFU bootloader（它自带 32 KB 区，起始 `0x08000000`）。
2. 按住 BOOT 上电进入 DFU，烧写本应用：
   ```sh
   dfu-util -d 1a86 -s 0x08008000:leave -D build/ch32v30x_bmp-release/bmp-ch32v30x-port.bin
   ```
3. 之后用 GDB 连接：
   ```sh
   arm-none-eabi-gdb -ex 'target extended-remote /dev/ttyACM0'   # Linux
   # Windows: COMx / WinUSB，取决于你的 gdb 构建
   ```
4. 需要重新进入 bootloader 时：`dfu-util -e`（或 GDB 里 `monitor bootloader`）。

## 已知问题：GDB 长回复卡住（已规避，待专门修复）

### 症状

`att 1`（或任何会产生长回复的操作）失败：

```
$qXfer:features:read:target.xml:0,7fb
getpkt: Timed out.
Ignoring packet error, continuing...
...
Bad register packet; fetching a new packet
Truncated register 22 in remote 'g' packet
```

`mon jt`、`mon swd_scan` 这类短回复正常，所以表现为"目标能识别、但 attach 不上"。
把 GDB 超时放大（`set remotetimeout 20`）也没用：那份回复要等到**下一条主机命令**
之后才出现。

### 原因

1. **协议层**：attach 成功后 GDB 立即读 RISC-V 目标描述 XML（本目标约 4.5 KB），按
   stub 广播的 `PacketSize` 分块。包尺寸为 2048 时，第一块在线上正好 2048 字节。
2. **控制器层**：CH32V30x USBHS 的 IN 端点一次只装 1 个 max-packet（512 字节）
   （`usbd_ep_start_write()` 用 `MIN(len, ep_mps)`），其余必须由 USBHS 中断里的
   "续包"分支补发，完成回调也来自中断。
3. **端口层**：旧 `gdb_in_send()` 交包后死等，**250 ms 等不到就清状态**
   （`gdb_in_len = 0`）。于是硬件只发出第一个 512 字节，而驱动仍认为传输未完成；
   下一条回复再调 `usbd_ep_start_write()` 会重置驱动的 `xfer_buf`/`xfer_len`，两边
   状态不一致，剩下的包最终在中断被服务时才出去（即"下一条主机命令之后"）。GDB 把
   这份迟到的 XML 当成下一条 `$g` 的回复（`Bad register packet`），再拿内置寄存器表
   去量那个 33×4B 的寄存器包（`Truncated register 22`）。

**不是** 位操作（SWD/JTAG）的问题：XML 生成只做字符串格式化、不访问目标，而同一段
bitbang 路径上的短回复始终正常。

### 当前规避做法

- `bmp_port/CMakeLists.txt`：`GDB_PACKET_BUFFER_SIZE=496`，回复加帧格式 ≤500 字节
  < 512，**任何回复都不跨 USB 包**，不再需要驱动续包；`bmp_port/cdc_acm_dual.c` 里有
  编译期 `#error` 守住这个不变量。
- 发送路径改为"拷贝进专用在飞缓冲、由硬件自行发出"，最长 5 ms 的保护性等待**只**
  用于避免覆盖仍在被读出的缓冲；正确性不依赖完成回调，也不会中途清掉驱动的传输状态。
- **对客户端的要求**：单次 `m`（十六进制读内存）长度不能超过
  `GDB_PACKET_BUFFER_SIZE / 2`（当前 **248 字节**，判定在 `blackmagic/src/gdb_main.c`
  的 `len > GDB_PACKET_BUFFER_SIZE / 2U`），超了会直接回 `E02`。真正的 GDB 会按广播的
  `PacketSize` 自动分块，但自己实现协议的客户端必须自己收敛——例如浏览器刷写工具若
  回读固定 256 字节/块且不解析 `PacketSize`，在 496 的包尺寸下会出现大面积 `E02`。
  另外留意这类工具"读不回就用源数据补齐 CRC"的兜底：那样得到的 CRC 相等并不代表
  读回校验通过。

代价：`m`/`X` 每次约 230 字节（原来约 1020），`load`/`dump` 往返次数约 4 倍
（100 KB 量级多花 0.5 s 左右，基本无感）；RAM 反而省了约 8 KB。

### 后续正解（待办）

1. **驱动**（`third_party_components/CherryUSB/port/wch/ch32v30x/usb_dc_ch32v30x.c`）：
   `USBD_IRQHandler` 每次中断只处理 `INT_ST` 里的一个端点，然后清掉全局
   `USBHS_TRANSFER_FLAG`；端点一多（本设备 3 个 CDC + DFU，且 OUT 端点常驻 armed），
   IN 完成事件就可能被丢掉。改成循环处理所有 pending 端点
   （`while (USBHS_DEVICE->INT_FG & USBHS_TRANSFER_FLAG)`）。
   注意同一控制器上**多包 OUT 是好的**（DFU bootloader 能收 4 KB 整包传输），不可靠
   的只有 IN 方向。
2. **端口**：超时后不要重置驱动的传输状态（或干脆无限等）。
3. **定位手段**：在驱动里加计数（续包次数 / 完成回调次数 / 丢事件次数），用 `mon`
   命令或目标串口打印，先量化"丢的是哪次事件"，再改代码。
4. 修好后把 `GDB_PACKET_BUFFER_SIZE` 提回 2048（或更大），`load`/`dump` 往返更少。
5. 独立小改进（与本次无关）：`blackmagic` 的 `riscv_debug.c` 里 DMI `RV_DMI_TOO_SOON`
   重试是无限循环，链路边缘时表现为"卡死且无提示"，改成有上限并报错更好定位。

## 说明

- 子仓：`blackmagic`（`https://codeberg.org/mTOTm/blackmagic.git`，
  `dev/jiance.zhang/main_test`）与 `third_party_components/CherryUSB`
  （`ch32v30x-usbhs`，含 CH32V30x USBHS 设备控制器端口）。
- target 驱动清单在 `bmp_port/CMakeLists.txt` 里按需裁剪（应用分区 96 KB），
  需要更多芯片支持时往里加文件即可。
