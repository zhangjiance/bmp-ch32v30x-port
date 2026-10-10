# JTAG 时钟占空比：为什么约八分之一的时钟测得 86%

[English](jtag-clock-duty-cycle.md) | [简体中文](jtag-clock-duty-cycle.zh-CN.md)

[返回工程 README](../README.zh-CN.md)

开发笔记。这里记录在 bulk 移位循环已经配平到 50% 之后，仍然测得约 86% 占空比的那部分
JTAG 时钟来自哪里、怎么修的、代价是什么。时序模型本身摘要在 `bmp_port/timing_ch32.c`
里，推理过程放在这里。

## 症状

移位循环改写之后（见下文），示波器上大部分 TCK 时钟是 50%，但**约八分之一**测得 **86%**；
另有少数路径是反过来的 86%（低电平占 86%）。86% 这个数字很接近 `6 / 7` —— 这正是"某一
个半周期里完全没有工作"时时钟的样子。

## 根因

两条互不相关的单 bit 路径，歪的方向还相反。两者都不罕见，合起来覆盖了扫描中的相当大
一部分时钟。

### （a）每次 bulk 移位的"尾部位" —— 高电平约 86%

`jtagtap_tdi_tdo_seq()` 与 `jtagtap_tdi_seq()` 把传输拆成整字节加 `clock_cycles % 8`
个尾部位，而当时只改写了字节循环。尾部仍然沿用逐 bit 的辅助宏，而且背靠背：

```c
/* 修改前 */
PIN_JTAG_SHIFT_LOW((last >> bit) & 1U);   /* 下降沿 */
PIN_JTAG_CLK_HIGH();                      /* 上升沿，紧跟其后 */
if (PIN_TDO_IN())
    value |= mask;
```

两条 GPIO store 之间什么都没有：TCK 低 0~1 个周期、高 6~7 个周期。最后一拍（驱动
`final_tms` 的那一拍）和 `jtagtap_tdi_seq()` 的尾部是同样的形状。

按上层实际传入的 `clock_cycles`（`adiv5_jtag.c`、`riscv_jtag_dtm.c`）统计发生频率：

| 调用 | 尾部时钟数 | 占该次调用的比例 |
|---|---|---|
| 32 位（RISC-V DMI data、IDCODE） | 8 | 25% |
| 35 位（ADIv5 DP/AP） | 3 | 9% |
| 2 位（DMI status）、4-5 位（IR）、5-6 位（`abits`） | 全部 | **100%** |

本端口对接的是 RISC-V DTM，一次 DMI 访问是 `2 + 32 + abits` 个时钟 —— 也就是说**一次
DMI 访问里约 40% 的时钟是歪的**，而不是八分之一。示波器之所以看着像"偶尔"，是因为触发
在长扫描上，而长扫描里尾部占比很小。

### （b）单时钟路径 —— 低电平约 86%

`jtagtap_next()` 与 `jtagtap_cycle()` 是每个时钟调用一次的，所以它们时钟的**低电平半周
期属于调用方**：函数返回、调用方处理这一 bit、再调用进来，全都发生在 TCK 为低的时候。

```c
/* 修改前 */
PIN_SWCLK_TCK_SET();
const uint16_t result = (uint16_t)PIN_TDO_IN();   /* 高电平：2~3 条指令 */
PIN_SWCLK_TCK_CLR();
return result != 0;                               /* 低电平：12+ 周期，花在调用方 */
```

低约 12 个周期对高 2~3 个周期。这些路径只在枚举阶段使用（51 个时钟的
`jtagtap_cycle()` 复位、`jtag_read_irs()`），所以很容易漏掉。

## 改法

通用原则，适用于所有路径：**一个 JTAG 时钟要干的活是固定的，但由哪个半周期来干是可以
选的。** 目标器件在下降沿把 TDO 这一位打出来，并保持到下一个下降沿，所以"构建下一个下
降沿的 word"和"采样 TDO"两件事都可以放在 TCK 为低的时候做；把它们挪过去之后，上升沿
就只剩一条 store。时钟频率不变，变的只是波形形状。

```327:357:bmp_port/jtagtap.c
		do {
			/* Falling edge, TDI as built during the low half of this clock. */
			PIN_JTAG_SHIFT_LOW_STORE(word);
			PIN_STICK_HERE(in);
			/*
			 * Low half: build the word for the next falling edge.  ...
			 */
			in >>= 1U;
			word = PIN_JTAG_SHIFT_LOW_WORD(in);
```

在此之上还有三处具体改动：

1. **尾部不再是特殊情况。** 它套用与字节循环相同的结构，包括无分支的 TDO 累加和 32 位
   累加器（用 8 位累加器会让编译器每 bit 生成一次 `zext.b`，即每个时钟多一个周期）。
2. **`final_tms` 那一拍的 word 提到函数开头算**，在任何时钟之前，用新增的
   `PIN_JTAG_SHIFT_LOW_TMS_WORD()` 宏。它要 7 条指令；如果放在使用它的那一拍的低电平里，
   那一个低电平就会比其它所有低电平长 7 个周期。
3. **单时钟路径用 pad 配平。** `jtagtap_cycle_no_delay()` 两侧都加 pad（循环回跳落在低
   电平侧，所以两侧不等量）；`jtagtap_next_no_delay()` 给高电平加 pad，大致匹配调用方。

## 实测

用 `objdump -d` 数跨越每个上升沿两侧的指令数（144 MHz）：

| 路径 | 低 / 高 | 占空比 |
|---|---|---|
| `tdi_tdo_seq` 字节循环 | 7 / 7 | 50% |
| `tdi_tdo_seq` 尾部位 | 9 / 9 | 50% |
| `tdi_tdo_seq` 最后一拍（`final_tms`） | 8 / 8 | 50% |
| `tdi_seq` 字节循环 | 5 / 5 | 50% |
| `tdi_seq` 尾部位 | 5 / 5 | 50% |
| `tdi_seq` 最后一拍 | 6 / 6 | 50% |
| `tms_seq` | 7 / 7 | 50% |
| `jtagtap_cycle` | 8 / 8 | 50% |
| `jtagtap_next` | 调用方 / 约 13 | 约 40%（原先约 14%） |

每时钟开销从 13~14 条指令变成 16 条，这就是 `timing_ch32.c` 里 `USED_SWD_CYCLES` 改成
16 的原因。循环其实并没有变慢：原先用来选 TDI 电平的分支在大约一半的 bit 上预测失败，
现在那个分支没有了。

## 重新配平

`jtagtap.c` 顶部的 pad 用来吸收循环结构无法表达的 1~3 个周期残差，比一个
`PIN_CLK_DELAY_ITERS()` 步进（3 个周期）更细。改动循环或编译选项后，按下面的命令重新数：

```
/opt/Toolchain/RISC-V_Embedded_GCC12/bin/riscv-wch-elf-objdump -d \
  build/ch32v30x_ob-release/CMakeFiles/bmp-ch32v30x-port.elf.dir/bmp_port/jtagtap.c.obj
```

数"拉低 TCK 的 store"到"拉高 TCK 的 store"之间的指令数，以及从后者到下一个下降沿之间的
指令数，把差值补到较短的一侧。

## 残留风险

- **`jtagtap_next()` 是估算值。** 它的低电平属于调用方，而调用方在厂商自己的代码里，
  所以 `JTAG_NEXT_HIGH_PAD` 是按典型调用点估的，不是实测的。如果示波器上那几拍仍不准，
  调这一个常量即可。它只在枚举阶段运行。
- **`swdptap.c` 有同样的问题，本次未处理。** `PIN_SWD_SHIFT_LOW()` 仍然按数据 bit 分支，
  SWD 循环也有自己的尾部位和单时钟路径。
- **pad 与编译器相关。** 它们是在 `-DCMAKE_BUILD_TYPE=ch32v30x_ob-release` 下数出来的，
  换优化等级会变。
