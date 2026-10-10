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
docs/                     开发笔记（问题记录、设计笔记）
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
2. 烧写本应用。因为本应用自带 **DFU runtime 接口**（接口 4），下面这**同一条命令在两种
   状态下都可用**，不需要先判断设备当前跑的是哪一份固件：

   ```sh
   dfu-util -d 1a86 -s 0x08008000:leave -D build/ch32v30x_ob-release/bmp-ch32v30x-port.bin
   ```

   - **设备已经在 bootloader 的 DFU 模式**（按住 BOOT 上电进入）→ 直接下载。
   - **设备正在运行本应用**（`1a86:6018`）→ dfu-util 在做下载前会先发 DFU_DETACH，
     runtime 接口把它交给 `platform_request_boot()`，写 BKP 握手后复位，bootloader 以
     DFU 模式启动，命令接着把镜像烧进去。因此**不必再按住 BOOT 上电**。
   - `-d 1a86` 只按 VID 匹配（`dfu-util` 的 run-time ID 也会匹配之后的 DFU 模式设备），
     正好同时覆盖应用（`1a86:6018`）和 bootloader（`1a86:df11`），切换模式后不会被丢掉。
   - 只想让正在运行的应用回到 bootloader、不立即烧录时，单独用 `dfu-util -e`。

3. 之后用 GDB 连接：
   ```sh
   arm-none-eabi-gdb -ex 'target extended-remote /dev/ttyACM0'   # Linux
   # Windows: COMx / WinUSB，取决于你的 gdb 构建
   ```
4. 需要重新进入 bootloader 时：`dfu-util -e`（或 GDB 里 `monitor bootloader`）。

## 开发笔记

问题记录与设计笔记都在 `docs/` 下，中英文各一份。本 README 只作为工程简介，推理过程记录在
笔记里：

- [`docs/gdb-reply-path.zh-CN.md`](docs/gdb-reply-path.zh-CN.md) —— CH32V30x USBHS 的
  IN 传输问题（GDB 长回复卡住的根因）、把每条回复限制在单个 USB 包内的回复长度上限
  （`GDB_PACKET_BUFFER_SIZE = 496`），以及残留风险。

## 说明

- 子仓（都在 `third_party_components/` 下）：`blackmagic`
  （`https://codeberg.org/mTOTm/blackmagic.git`，`dev/jiance.zhang/main_test`）与
  `CherryUSB`（`ch32v30x-usbhs`，含 CH32V30x USBHS 设备控制器端口）。
- target 驱动清单在 `bmp_port/CMakeLists.txt` 里按需裁剪（应用分区 96 KB），
  需要更多芯片支持时往里加文件即可。
