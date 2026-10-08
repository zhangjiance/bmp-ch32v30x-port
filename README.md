# bmp-ch32v30x-port

Black Magic Probe（BMP）在 CH32V30x（USBHS，高速）上的端口工程，作为
`ch32_dfu_boot` 的**应用程序**运行。

结构参考 `bmp-hpm-port`，构建约定沿用 `ch32_dfu_boot` / `ch32_hello_world`：

```
CMakeLists.txt            应用工程
CMakePresets.json         release / debug 预设
cmake/wch_riscv.cmake     工具链（riscv-wch-elf-）
cmake/blackmagic.cmake    blackmagic 源清单与 include
blackmagic/               子仓：BMP 核心与 target 驱动
third_party_components/CherryUSB/  子仓：USB 协议栈 + CH32V30x USBHS 端口
SDK/                      WCH ch32v30x 外设库
port/ch32v30x/            端口层
boards/ch32v30x_bmp/      板级 BSP
src/                      main + WCH 系统/中断
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

nTRST / nSRST 未接线（空实现）。引脚定义在 `port/ch32v30x/jtag_port.h`。

## USB 设备（复合设备，1d50:6018）

| 接口 | 功能 |
|---|---|
| 0 / 1 | GDB server（CDC ACM） |
| 2 / 3 | 目标串口 UART（CDC ACM，USART3） |
| 4 | DFU runtime（无端点） |

- 带 MS OS 1.0（WCID）Compatible ID，Windows 会**自动装 WinUSB**（接口 0/2/4），不需要 Zadig。
- **不含 RTT**：本端口只提供 GDB 口和目标串口，不额外加 RTT 虚拟串口。

## 与 bootloader 配合

- 应用链接在 `0x00008000`（`linkfile/flash_dfu.ld`，96 KB），与
  `shared/boot_protocol.h` 里的 `BOOT_APP_OFFSET` 一致。
- 回到 bootloader 有两条路径，都走 `port/ch32v30x/boot_trigger_ch32v30x.c` 的
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
   dfu-util -d 1a50 -s 0x08008000:leave -D build/ch32v30x_bmp-release/bmp-ch32v30x-port.bin
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
- target 驱动清单在 `cmake/blackmagic.cmake` 里按需裁剪（应用分区 96 KB），
  需要更多芯片支持时往里加文件即可。
