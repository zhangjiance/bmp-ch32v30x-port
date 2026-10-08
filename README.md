# bmp-ch32v30x-port

Black Magic Probe（BMP）在 CH32V30x（USBHS，高速）上的端口工程，作为
`ch32_dfu_boot` 的**应用程序**运行。

结构对齐 `bmp-hpm-port`：端口层集中在 `bmp_port/`，`main.c` 在顶层，板级 BSP 在
`boards/<board>/`，构建约定沿用 `ch32_dfu_boot` / `ch32_hello_world`：

```
CMakeLists.txt            顶层：工具链、板级选择、链接、objcopy
main.c                    应用入口
CMakePresets.json         release / debug 预设
bmp_port/                 端口层：USB、GPIO 位操作 SWD/JTAG、目标串口、定时、boot 握手、MCU glue
bmp_port/CMakeLists.txt   端口源清单 + blackmagic 源清单与 include（对齐 hpm 的组织方式）
cmake/wch_riscv.cmake     工具链（riscv-wch-elf-）
blackmagic/               子仓：BMP 核心与 target 驱动
third_party_components/CherryUSB/  子仓：USB 协议栈 + CH32V30x USBHS 端口
SDK/                      WCH ch32v30x 外设库
boards/ch32v30x_bmp/      板级 BSP
linkfile/flash_dfu.ld     应用链接脚本（0x00008000，96K）
shared/boot_protocol.h    boot 与 app 的约定（分区、BKP 触发）
```

## 引脚（与 `ch32v305_bmp` 一致，直接用同一根调试排线）

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

SWD/JTAG 只做 **GPIO 位操作**（`bmp_port/jtag_port.h` 提供 `PIN_*` 宏，
`swdptap.c` / `jtagtap.c` 与 `bmp-hpm-port` 逐字共用），不搬 hpm 的 SPI 加速，
这样时序完全确定。

## USB 设备（复合设备，1a86:6018）

| 接口 | 功能 |
|---|---|
| 0 / 1 | GDB server（CDC ACM） |
| 2 / 3 | 目标串口 UART（CDC ACM，USART3） |
| 4 | DFU runtime（无端点） |

- **DFU runtime（接口 4）用 Microsoft OS 1.0（WCID）**：0xEE 字符串 + `bRequest=0x20`，
  Compatible ID = `WINUSB`，并带 `DeviceInterfaceGUIDs` 扩展属性（必须，CherryUSB 对
  `wIndex=5` 会直接解引用 `comp_id_property`）。Windows **自动装 WinUSB**，`dfu-util -e`
  开箱可用，不需要 Zadig；做法与 `ch32_hello_world` / `bmp-hpm-port` / `ch32_dfu_boot` 一致。
- Windows 把 WCID 结果按 `VID/PID/bcdDevice` 缓存在
  `HKLM\SYSTEM\CurrentControlSet\Control\usbflags`，所以**改 `bcdDevice`（或 VID/PID）
  才会让它重新询问**。
- GDB / 目标串口两个 CDC 功能**故意不列入 WCID**，Windows 继续用 inbox usbser.sys，
  因此它们仍是 COM 口（与 `bmp-hpm-port` / `ch32_hello_world` 的用法一致）。
- DFU 接口的 `iInterface` 指向字符串 6 = `"Black Magic Firmware Upgrade"`，所以
  `dfu-util -l` 里 `name=` 不再显示 `UNKNOWN`。
- VID 用 `1a86`（WCH，与 `ch32_dfu_boot` 的 `1a86:df11`、`ch32_hello_world` 的
  `1a86:df12` 同源），PID 保留 Black Magic 的 `0x6018`。
- `bcdUSB` = 2.0；`bcdDevice` = `0x0103`（属于 Windows 硬件 ID 与 usbflags 缓存键，
  每次改描述符/WCID 数据就 +1，用来强制 Windows 重新询问并重装）。
- **不含 RTT**：本端口只提供 GDB 口和目标串口，不额外加 RTT 虚拟串口。

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

1. 先烧 `ch32_dfu_boot`（bootloader，占前 32 KB）。
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

## 说明

- 子仓：`blackmagic`（`https://codeberg.org/mTOTm/blackmagic.git`，
  `dev/jiance.zhang/main_test`）与 `third_party_components/CherryUSB`
  （`ch32v30x-usbhs`，含 CH32V30x USBHS 设备控制器端口）。
- target 驱动清单在 `bmp_port/CMakeLists.txt` 里按需裁剪（应用分区 96 KB），
  需要更多芯片支持时往里加文件即可。
