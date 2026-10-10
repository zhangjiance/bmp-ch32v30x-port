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

## GDB 回复：IN 传输问题与回复长度上限

### 症状

在 GDB CDC 接口上观测到两种失败模式，表现都像"IN 传输卡住"。

**（a）`att 1`（或任何会产生长回复的操作）失败：**

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

**（b）flash 回读（`m` 读）偶发卡住。** 主机要么在整整一个超时窗口内**一个字节都收
不到**，要么收到的字节流过不了 GDB 组帧校验。它**与目标无关**：只要数据到了，其 CRC32
与源镜像完全一致。最初看着像"与速度相关"（把探针时钟设成固定值似乎就好了），那是误判，
原因见下面的"回复长度上限"。

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

**不是** 位操作（SWD/JTAG）的问题：XML 生成只做字符串格式化、不访问目标，同一段
bitbang 路径上的短回复始终正常，而且失败模式（b）是在一次普通 `m` 读上复现的——那条回复
与目标时序无关。SWD/JTAG 时钟及其占空比是单独测量过的，与本文无关。

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
  - `GDB_PACKET_BUFFER_SIZE` 现为 **496**，让每条回复都落在单个 USB 包内——见下面的
    "回复长度上限"。
  - **满包回复用 ZLP 收尾**（`c957249` `fix: terminate full-size GDB replies with a ZLP`）：
    bulk 传输只由**短包**终结，而 `usbd_ep_start_write()` 自己不会补零长包；当回复长度恰好是
    `USB_XFER_SIZE`（512 字节）的整数倍时，传输停在满包上，主机认为后面还有数据，整份回复要拖
    到**下一条主机命令**才被交上去。GDB 正好踩中：`qXfer:features:read:target.xml:0,7fb` 回
    复组帧后是 `$` + `m` + 2043 + `#` + 2 字节校验 = 2048 = 4 × 512，就是 attach 后卡住的
    直接原因。现在发送结束时若长度为 `USB_XFER_SIZE` 的整数倍，就再发一个**零长包**（ZLP）作为
    终止符并等它完成。
- **对客户端的要求**：单次 `m`（十六进制读内存）长度不能超过
  `GDB_PACKET_BUFFER_SIZE / 2`，现在是 **248 字节**（判定在
  `third_party_components/blackmagic/src/gdb_main.c` 的
  `len > GDB_PACKET_BUFFER_SIZE / 2U`），超了直接回 `E02`。按广播的 `PacketSize`
  自动分块的客户端（libgdb，以及 `static/blackmagic-utils/` 下的 webui）会自动适配；
  自己手写协议的客户端必须尊重它或解析 `PacketSize`。另外留意这类工具"读不回就用源数据
  补齐 CRC"的兜底：那样得到的 CRC 相等并不代表读回校验通过。

### 回复长度上限：为什么 `GDB_PACKET_BUFFER_SIZE` 是 496

真正消掉故障的是这一条。GDB 回复的组帧是 `'$' + payload + '#' + 2 字节校验`，所以

```
一条回复的线长  =  GDB_PACKET_BUFFER_SIZE + 4
落在一个 512 字节 USB 包内  <=>  GDB_PACKET_BUFFER_SIZE <= 508
顺便躲开 ZLP 路径          <=>  GDB_PACKET_BUFFER_SIZE <= 507
```

- **496**（496 + 4 = 500 < 512）让每条回复**既**在单包内、**又**不碰 ZLP 路径。
  `gdb_in_send()` 因此恒定退化为"**一次 `usbd_ep_start_write()` + 无 ZLP**"：
  分块续传那条路径不是"概率低"，而是**不可达**。
- 这不是在给一个坏控制器打补丁，而是这块控制器的**正规用法**，见下一节。
- 代价是读长度（单次 `m` 从 512/1024 降到 248 字节），全量转储会多几百次往返。

### 为什么必须一个包一个包发：这块控制器没有 TX burst

- CherryUSB 自己的文档把 USB IP 分成两类：**硬件分包**（IP 自带 DMA / descriptor DMA，
  自己把一次传输拆包）与**软件分包**（IP 只有 FIFO，驱动必须一个包一个包地交给它）。
  CH32V30x USBHS 设备控制器属于后者——这就是上面说的"没有多包 burst"，也是新版 IP
  （`port/wch/usbhs`）有 `UEP_TX_BURST` 而它没有的原因。
- WCH 官方文档把约定写得同样直白：MCU 备好数据和长度，主机下发 IN 令牌时硬件上传
  **一个包**，然后进入 IN 中断置 NAK、翻转 toggle。官方示例为此保留了一个 **busy** 标志，
  **在 IN 中断里清除**，并且明确的坑是：**凡是把端点置为 NAK 的地方都必须同时清 busy**。
  长回复被明确归为调用方的责任（"描述符较长时主机会分多个 IN 包获取，剩下的数据要在
  IN 中断里接着准备"），零长包同理。

与其他 BMP 端口对比：

| 端口 | `GDB_PACKET_BUFFER_SIZE` | 端点 MPS | 每条回复的包数 |
|---|---|---|---|
| 上游 BMP（STM32F103 native） | 1024 | 64（全速） | 最多 17 —— STM32 的 IP 会续发，没问题 |
| `bmp-hpm-port`（HPM5301） | 16384 | 512（高速） | 最多 33 —— 硬件分包，没问题 |
| `ch32v305_dap` DAPLink | 不适用（`DAP_PACKET_SIZE` = 512） | 512 | **恒为 1** |
| **本端口** | **496** | 512 | **恒为 1** |

同样用这块控制器的 DAPLink 是靠协议天然规避的：它的协议包就是 512 字节 = 端点 MPS，
所以那边也不会走续传分支。

### 为什么"满包没有终止符"只在 Windows 上发作

同一份回复在 Linux 上一向是好的，包括设备用 `usbipd` 挂进 WSL 的用法——因为决定"一次 IN
传输什么时候算结束"的是**主机侧的 USB 协议栈**，而 Linux 和 Windows 在这一点上的做法不同：

- **Linux**：`cdc_acm` 给 bulk IN 端点准备的每个 URB 缓冲区大小就是端点的最大包长
  （`acm->readsize = wMaxPacketSize`，本设备 512 字节），所以设备发来的一个满包就填满了整个
  URB，URB 立刻完成、数据当场交给用户态——它**从来不依赖那个零长包**。`usbfs` / libusb 只要
  按固定字节数读，效果也一样。
- **Windows**：`usbser.sys`（COM 口）和 WinUSB 都是按**自己选定的长度**提交一次 bulk IN
  请求，只有收满这个长度、或收到短包，才认为传输结束。回复长度正好是 512 的整数倍时，结尾没有
  短包，这次请求永远完不成，数据只能等下一条命令带来一个短包才被放行——所以"按整个 URB 读"的
  `usbser.sys` / WinUSB 最容易暴露这个问题。
- **WSL + usbip 为什么也没有事**：走 usbip 时 URBs 仍然由 Linux 侧产生
  （`vhci_hcd` + `cdc_acm`，还是 512 字节一个），只是被转发到 Windows 上执行，传输的终止语义
  依旧按 Linux 的来，问题不会出现。

请注意这一节只覆盖**终止符**这一类（回复长度正好是 512 的整数倍）。而**续传**失败是
与主机无关的：`GDB_PACKET_BUFFER_SIZE = 2048` 时一条 256 字节的 `m` 回复是
516 = 512 + 4 字节，回读卡顿在 Linux 上用 Chrome/Web Serial 客户端同样复现。把回复限制在
单包内可以同时避开这两类。

### 残留风险

- `gdb_in_send()` 在某个块的完成事件没有在 `GDB_IN_XFER_TIMEOUT_MS`（1000 ms）内到达时，
  仍然会**丢掉整条回复**（置 `gdb_in_len = 0`），于是这块控制器上的一次抖动会变成"客户端
  什么都没收到"，而不是"重试一次"。改成**有界地重发该块**即可自愈。496 下每条回复只有
  1 个块（原为 2..9 个），这就是它不再出现的直接原因。
- `usbd_event_handler()` 只处理了 `USBD_EVENT_RESET`，没有处理
  `USBD_EVENT_DISCONNECTED`。目前没有无限阻塞的等待，所以这条只是预防性建议。
- 若将来确实需要一条回复超过一个包，**请去修续传路径，而不是先调大
  `GDB_PACKET_BUFFER_SIZE`**。在这块控制器上可行的做法（见 `ch32v305_dap`）是：以**整条
  协议包**为粒度、由上层队列加 idle 标志来发送，而不是从 IN 中断里续发同一条长传输。
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
