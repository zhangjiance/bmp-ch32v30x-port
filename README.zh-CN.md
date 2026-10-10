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
third_party_components/
  blackmagic/             子仓：BMP 核心与 target 驱动
  CherryUSB/              子仓：USB 协议栈 + CH32V30x USBHS 端口
SDK/                      WCH ch32v30x 外设库
boards/ch32v30x_ob/      板级 BSP
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
| 状态 LED（低有效） | PA5 |
| BOOT 按键（接 GND，低有效） | PA6 |
| 目标串口 TX (USART3) | PB10 |
| 目标串口 RX (USART3) | PB11 |

nTRST / nSRST 未接线（空实现）。SWD/JTAG 引脚定义在 `bmp_port/jtag_port.h`；状态 LED 与
BOOT 按键属于板级引脚（`boards/ch32v30x_ob/board_config.h` 里的 `BOARD_LED_*` /
`BOARD_BOOT_*`），通过板级原语 `board_led_write()` / `board_read_boot_pin()` 访问。

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
- `bcdUSB` = 2.0；`bcdDevice` = `0x0104`。`bcdDevice` 属于 Windows 硬件 ID 与 usbflags
  缓存键，每次改描述符/WCID 数据就 +1，用来强制 Windows 重新枚举并重装。
- **不含 RTT**：本端口只提供 GDB 口和目标串口。

## 目标串口（USB2UART）

第二路 CDC（接口 2/3）对接 USART3（PB10/PB11）。串口**硬件由板级描述**而不是端口：实例、引脚
和时钟是 `boards/ch32v30x_ob/board_config.h` 里的 `BOARD_APP_UART*` 宏，由
`board_init_app_uart()` 完成初始化；端口只负责线路格式和线上层。
整条通路是 **DMA + 中断驱动**，不依赖主循环：

- **target → host**：USART3 RX 由 DMA1 通道 3 循环写入 256 字节缓冲；串口**空闲中断**
  与 DMA 半满/全满中断把已到达的数据经 USB sink 直接写进 IN 端点，端点完成回调
  （`aux_serial_usb_ready()`）接着搬剩下的部分。
- **host → target**：USB OUT 回调把数据排入 1 KB 环形缓冲，DMA1 通道 2 一次发一段连续
  区域，传输完成中断接着发下一段。
- **不做 DTR 门控**：只要求主机在读数据，兼容不置 DTR 的终端。
- 由此 GDB 正在执行长命令时串口也不会停顿——旧实现是靠 `gdb_if_getchar()` 的空闲
  循环轮询转发的，命令期间会卡住。

## 与 bootloader 配合

- 应用链接在 `0x00008000`（`linkfile/flash_dfu.ld`，96 KB），与
  `shared/boot_protocol.h` 里的 `BOOT_APP_OFFSET` 一致。
- 回到 bootloader 有三条路径，都走 `bmp_port/boot_trigger_ch32v30x.c` 的
  BKP 握手（跨 `NVIC_SystemReset()` 保留）：
  - `dfu-util -e`（DFU_DETACH）→ `platform_request_boot()`
  - GDB 侧 `monitor bootloader`
  - 按住 BOOT 按键（PA6）：探测端通过 `board_timer_create()`（`bmp_port/platform.c`）
    注册 100 ms 周期回调，由板级的 TIM3 中断调用，因此在主循环阻塞于
    `gdb_if_getchar()` 等 GDB 命令时依然有效

## 构建

```sh
cmake --preset ch32v30x_ob-release
cmake --build --preset ch32v30x_ob-release
```

产物：`build/ch32v30x_ob-release/bmp-ch32v30x-port.{elf,hex,bin}`。

## 烧录 / 使用

1. 先烧配套的 DFU bootloader（它自带 32 KB 区，起始 `0x08000000`）。
2. 按住 BOOT 上电进入 DFU，烧写本应用：
   ```sh
   dfu-util -d 1a86 -s 0x08008000:leave -D build/ch32v30x_ob-release/bmp-ch32v30x-port.bin
   ```
3. 之后用 GDB 连接：
   ```sh
   arm-none-eabi-gdb -ex 'target extended-remote /dev/ttyACM0'   # Linux
   # Windows: COMx / WinUSB，取决于你的 gdb 构建
   ```
4. 需要重新进入 bootloader 时：`dfu-util -e`（或 GDB 里 `monitor bootloader`）。

## 已修复：GDB 长回复卡住（根因与修复）

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

### 根因

是 **CherryUSB CH32V30x USBHS 设备控制器端口**的问题
（`third_party_components/CherryUSB/port/wch/ch32v30x/usb_dc_ch32v30x.c`），而且只
发生在 IN 方向；端口层的写法则一直在触发它。

1. **控制器**：CH32V30x USBHS 没有多包 burst（新版 IP 的 `port/wch/usbhs` 有
   `UEP_TX_BURST`）。`usbd_ep_start_write()` 只装 `MIN(len, ep_mps)` = 512 字节，
   剩下的必须由 `USBD_IRQHandler()` 续发，因此一次传输要跨多个中断。
2. **驱动：没有"在飞"保护**。上一个传输还没发完时，`usbd_ep_start_write()` 照样接受
   新的传输：直接覆盖 `xfer_buf`/`xfer_len`/`actual_xfer_len`，重新装
   `UEPn_TX_LEN`/`TX_DMA`/`TX_CTRL`，等于把主机还在读的那次传输的 data toggle 重来一遍。
   旧传输的尾巴就被搁死在端点上，只能等下一个 USB 事件才出去——正是观测到的"回复在下
   一条主机命令之后才出现"。上游 `port/wch/usbhs` 对这种情况返回 `-4` 拒收，CH32V30x
   端口原来什么都不检查。
3. **驱动：每次中断只处理一个事件**。`USBD_IRQHandler()` 取一次 `INT_FG` 快照，只服务
   一个端点，然后清掉全局 `USBHS_TRANSFER_FLAG`；这期间锁存的其它完成事件就被丢了。
   本设备是复合设备（2 个 CDC + DFU，两个 bulk OUT 端点常驻 armed），丢掉 IN 完成事件
   就会让一次传输永远结束不了。
4. **端口：超时后清自己的状态**。旧 `gdb_in_send()` 交包后死等，**250 ms** 等不到就清
   `gdb_in_len`（中间版本只等 **5 ms**），然后重新装端点——正好喂给第 2 条。
5. **端口：在中断里装 IN 端点**。旧完成回调在 `USBD_IRQHandler()` 里调 `gdb_in_kick()`，
   即回复的第 2..n 块是从中断上下文排队的，这条路径在本控制器上不稳定。多包 **OUT 是
   好的**（DFU bootloader 能收 4 KB 整包传输），只有 IN 方向有问题。

**不是** 位操作（SWD/JTAG）的问题：XML 生成只做字符串格式化、不访问目标，而同一段
bitbang 路径上的短回复始终正常。

### 修复

- **驱动**（`usb_dc_ch32v30x.c`）
  - `usbd_ep_start_write()` 在端点仍持有传输时返回 `-4`，运行中的传输再也不会被覆盖。
    该标志在事务完成时释放（在回调之前，因为回调允许排队下一次传输）、stall 时释放、
    收到新 SETUP 时释放。
  - `USBD_IRQHandler()` 改成循环排空所有锁存事件（transfer / SETUP / 总线复位），不再
    一次中断只处理一个；`INT_ST` 只锁存一次——原来 `TOG_OK` 是在清掉 `INT_FG` **之后**
    才读的。
  - 新增 `g_ch32_usbhs_stats` 事件计数（装包次数 / 忙拒次数 / IN 事务数 / 续包次数 /
    完成回调数 / OUT / SETUP 与复位 / 循环溢出）。调试构建里
    `extern struct ch32_usbhs_stats g_ch32_usbhs_stats;` 就能打印，下次再卡可以先量化
    "丢的是哪次事件"。
- **端口**（`bmp_port/cdc_acm_dual.c`）
  - `gdb_in_send()` 把回复切成 `USB_XFER_SIZE`（512 字节）的块，**只在线程上下文**一次
    装一块并等它完成。回复不再依赖驱动的中断续包，完成回调只负责清 busy 标志。
  - `GDB_PACKET_BUFFER_SIZE` 已改回 **2048**。
- **对客户端的要求**：单次 `m`（十六进制读内存）长度仍然不能超过
  `GDB_PACKET_BUFFER_SIZE / 2`，现在是 **1024 字节**（判定在
  `third_party_components/blackmagic/src/gdb_main.c` 的
  `len > GDB_PACKET_BUFFER_SIZE / 2U`），超了直接回 `E02`。真正的 GDB 会按广播的 `PacketSize` 自动分块，自己实现协议的客户端必须尊重它
  或解析 `PacketSize`。另外留意这类工具"读不回就用源数据补齐 CRC"的兜底：那样得到的
  CRC 相等并不代表读回校验通过。

### 仍未处理

- `blackmagic` 子仓的 `riscv_debug.c` 里 DMI `RV_DMI_TOO_SOON` 重试是无限循环，链路边缘时
  表现为"卡死且无提示"，改成有上限并报错更好定位。
- 本驱动的 `usbd_get_port_speed()` 硬编码返回 `USB_SPEED_HIGH`，没有读
  `USBHS_DEVICE->SPEED_TYPE`；万一协商回全速，仍会按高速描述符上报非法的 512 字节
  bulk MPS。

## 说明

- 子仓（都在 `third_party_components/` 下）：`blackmagic`
  （`https://codeberg.org/mTOTm/blackmagic.git`，`dev/jiance.zhang/main_test`）与
  `CherryUSB`（`ch32v30x-usbhs`，含 CH32V30x USBHS 设备控制器端口）。
- target 驱动清单在 `bmp_port/CMakeLists.txt` 里按需裁剪（应用分区 96 KB），
  需要更多芯片支持时往里加文件即可。
