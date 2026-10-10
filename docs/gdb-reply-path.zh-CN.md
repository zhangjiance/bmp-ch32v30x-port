# GDB 回复路径：USBHS IN 传输问题与回复长度上限

[English](gdb-reply-path.md) | [简体中文](gdb-reply-path.zh-CN.md)

[返回工程 README](../README.zh-CN.md)

开发笔记。这里记录 CH32V30x USBHS 端口上长期存在的"GDB 长回复卡住"问题的症状、根因、
修复，以及最终消掉故障的回复长度上限。README 只保留工程简介，推理过程放在这里。

## 症状

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

## 根因

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

## 修复

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

## 回复长度上限：为什么 `GDB_PACKET_BUFFER_SIZE` 是 496

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

## 为什么必须一个包一个包发：这块控制器没有 TX burst

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

## 为什么"满包没有终止符"只在 Windows 上发作

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

## 残留风险

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
