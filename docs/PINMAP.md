# OpenRadioLink-v0.3 板级引脚映射（由实测网表生成）

> 生成脚本 `tools/gen_pinmap.py`；数据源 `netlist.enet`（官方网表导出）。
> 引脚号是 **ESP32-S3-WROOM-1 模组焊盘号**，GPIO 号由厂方焊盘表换算。
> 已与设计报告的既有引脚表交叉校验通过（USB / I2S / SA818 / UART0 / BOOT）。

## 0. v0.4 增量（手工补充；本节的引脚**不来自**上面的 v0.3 网表）

第二版 PCB（2026-10-16）加了一块前置面板，并修正了 v0.3 的功率切换脚。
下面这些引脚是实板接线，`tools/gen_pinmap.py` 目前还不知道它们；重新生成
`board_pins.h` / 本文件之前，请先从 v0.4 网表导出，否则会丢掉全部面板控制。

| GPIO | 模组焊盘 | 方向 | 用途 | 备注 |
| --- | --- | --- | --- | --- |
| IO10 | 18 | 开漏输出 | SA818 U5 第 7 脚 H/L（高/低功率） | r8：接地低功率、释放高功率，上下拉关闭；接地描述只适用于早期 v0.3 原图 |
| IO12 | 20 | 输出 | 电源灯（心跳闪烁） | 即 v0.3 的 LED1/LED_STATUS，高电平点亮 |
| IO11 | 19 | 输出 | 全局 RX 状态灯 | 高电平点亮；收射频或收网络音频时亮 |
| IO42 | 35 | 输出 | 全局 TX 状态灯 | 高电平点亮；发射机被键控时亮 |
| IO41 | 34 | 输入 | 全局 PTT 按键 | 对地按键 + 内部上拉，低有效；按住发射 |
| IO38 | 31 | 输入 | VOL+ 按键 | 对地 + 内部上拉，低有效；默认音量 + |
| IO39 | 32 | 输入 | VOL- 按键 | 对地 + 内部上拉，低有效；默认音量 - |
| IO15 | 8 | 输入 | CH+ 按键 | 对地 + 内部上拉，低有效；默认切换并应用下一个已存频道 |
| IO16 | 9 | 输入 | CH- 按键 | 对地 + 内部上拉，低有效；默认上一个已存频道 |

- 四个功能键（VOL+/VOL-/CH+/CH-）**可改绑**：动作存 NVS（命名空间 `orl`，
  键 `fp_act`，4 字节 blob），可在面板"音频增益矩阵"卡片或
  `GET /api/set?k=fp_key0..3&v=<动作>` 修改；动作名见 `frontpanel.h` 的
  `fp_action_t`（off / ch± / vol± / sql± / power / monitor / mode / bridge）。
- 音量那一对按**实机行为**标注：v0.4 板上 IO39 的按键是"音量−"、IO38 是"音量+"。
  最初按 `IO39=音量+` 写进 `board_pins.h`，实测反了，已交换 `PIN_KEY_VOL_UP`/`_DN` 别名。
- PTT 键刻意不参与改绑：它是电平控制而非动作，误绑发射键不是可接受的代价。
- IO38/39/41/42 是模组的 JTAG 引脚，用作 GPIO 没有问题 —— S3 的
  USB-Serial/JTAG 走 IO19/IO20，不占用它们。
- IO15/IO16 在 v0.3 网表里是空网络（焊盘 8/9），v0.4 用作两个功能键。

## 1. U2 主控（ESP32-S3-WROOM-1-**N16R8**）

| 焊盘 | 模组丝印 | GPIO | 网络 | 用途 |
| --- | --- | --- | --- | --- |
| 1 | GND |  | GND |  |
| 2 | 3V3 |  | +3V3 |  |
| 3 | EN |  | EN | 复位 SW2 |
| 4 | IO4 | GPIO4 | I2S_MCLK | I2S 主时钟 |
| 5 | IO5 | GPIO5 | I2S_BCLK | I2S 位时钟 |
| 6 | IO6 | GPIO6 | I2S_LRCK | I2S 帧时钟 |
| 7 | IO7 | GPIO7 | I2S_DIN | I2S 数据 S3→ES8311 |
| 8 | IO15 | GPIO15 | — |  |
| 9 | IO16 | GPIO16 | — |  |
| 10 | IO17 | GPIO17 | SA818_TXD | SA818 模块 TX |
| 11 | IO18 | GPIO18 | SA818_RXD | SA818 模块 RX |
| 12 | IO8 | GPIO8 | I2S_DOUT | I2S 数据 ES8311→S3 |
| 13 | IO19 | GPIO19 | USB_DM | 原生 USB D- |
| 14 | IO20 | GPIO20 | USB_DP | 原生 USB D+ |
| 15 | IO3 | GPIO3 | CHG_STAT | 充电状态（J2.2） |
| 16 | IO46 | GPIO46 | — |  |
| 17 | IO9 | GPIO9 | CHG_EN | 充电使能（J2.3） |
| 18 | IO10 | GPIO10 | — |  |
| 19 | IO11 | GPIO11 | — |  |
| 20 | IO12 | GPIO12 | LED_STATUS | 状态灯（高=亮） |
| 21 | IO13 | GPIO13 | AMP_SHDN | 功放关断（LM4871，低=开启） |
| 22 | IO14 | GPIO14 | SA818_PD | SA818 使能 |
| 23 | IO21 | GPIO21 | SA818_PTT | SA818 PTT（低=发射） |
| 24 | IO47 | GPIO47 | I2C_SCL | ES8311 I2C SCL |
| 25 | IO48 | GPIO48 | I2C_SDA | ES8311 I2C SDA |
| 26 | IO45 | GPIO45 | — |  |
| 27 | IO0 | GPIO0 | BOOT_IO0 | BOOT 键 SW1 |
| 28 | IO35 | GPIO35 | — |  |
| 29 | IO36 | GPIO36 | — |  |
| 30 | IO37 | GPIO37 | — |  |
| 31 | IO38 | GPIO38 | — |  |
| 32 | IO39 | GPIO39 | — |  |
| 33 | IO40 | GPIO40 | — |  |
| 34 | IO41 | GPIO41 | — |  |
| 35 | IO42 | GPIO42 | — |  |
| 36 | RXD0/IO44 | GPIO44 | U0RXD | UART0 RX（H1 排针） |
| 37 | TXD0/IO43 | GPIO43 | U0TXD | UART0 TX（H1 排针） |
| 38 | IO2 | GPIO2 | VBAT_ADC | 电池电压采样 ADC1_CH1（J2.1） |
| 39 | IO1 | GPIO1 | SA818_AUDIO_IN | ADC1_CH0 采 AF_OUT |
| 40 | GND |  | GND |  |
| 41 | GND |  | GND |  |

## 2. 关键外设引脚

### U3 — ES8311 音频编解码器（ES8311）

| 脚 | 网络 |
| --- | --- |
| 1 | I2C_SCL |
| 2 | I2S_MCLK |
| 3 | AUDIO_3V3 |
| 4 | AUDIO_3V3 |
| 5 | GND |
| 6 | I2S_BCLK |
| 7 | I2S_DOUT |
| 8 | I2S_LRCK |
| 9 | I2S_DIN |
| 10 | GND |
| 11 | AUDIO_3V3 |
| 12 | C7_N12 |
| 13 | C7_N14 |
| 14 | C7_N18 |
| 15 | C7_N17 |
| 16 | VMID_BYP |
| 17 | C7_N10 |
| 18 | C7_N8 |
| 19 | I2C_SDA |
| 20 | GND |
| 21 | GND |

### U4 — LM4871 音频功放（LM4871MX/NOPB）

| 脚 | 网络 |
| --- | --- |
| 1 | AMP_SHDN |
| 2 | AMP_BYP |
| 3 | AMP_BYP |
| 4 | AMP_IN |
| 5 | SPK_AMP_P |
| 6 | VBUS_5V |
| 7 | GND |
| 8 | SPK_AMP_N |

> **`DAC_P` 是共用节点（做音量控制前必看）**：`DAC_P = C14.2(←U3.12 ES8311 OUTP) + C30.1 + R13.1`，
> 往下分成两路：
> - `R13.2 → AMP_IN → U4.4`，进 LM4871 推喇叭；
> - `C30.2 → MIC_AC → R16.2 → SA818_MIC(U5.18)`，就是**电台的发射音频输入**。
>
> 所以**动 ES8311 的 DAC 音量会同时改变我方发射电平**（麦克风、尾音都走这一路）。
> 固件里的音量键因此不碰该寄存器，而是只对"给喇叭的那些音频"做数字衰减，
> 见 `voice.h` 的 `voice_set_speaker_level()`（八档：0=关功放，40/45/50/55/60/65/70）。

### U5 — SA818S-U 射频模块（SA818S-U）

| 脚 | 网络 |
| --- | --- |
| 1 | +3V3 |
| 2 | — |
| 3 | SA818_AUDIO_IN |
| 4 | — |
| 5 | SA818_PTT |
| 6 | SA818_PD |
| 7 | GND |
| 8 | VBUS_5V |
| 9 | GND |
| 10 | GND |
| 11 | — |
| 12 | ANT_UHF |
| 13 | — |
| 14 | — |
| 15 | — |
| 16 | SA818_RXD |
| 17 | SA818_TXD |
| 18 | SA818_MIC |
| 19 | GND |
| 20 | GND |

### MK1 — MEMS 麦克风（MSM381AKT003）

| 脚 | 网络 |
| --- | --- |
| 1 | C17_N3 |
| 2 | GND |
| 3 | GND |
| 4 | C17_N1 |

### LED1 — 状态灯（KT-0805Y）

| 脚 | 网络 |
| --- | --- |
| 1 | LED1_N2 |
| 2 | GND |

## 3. 全部网络（56 张）

| 网络 | 脚数 | 连接 |
| --- | --- | --- |
| `+3V3` | 15 | C1.1 C3.1 C4.1 C6.2 FB3.1 H1.2 H2.4 L1.2 R14.2 R15.2 R4.1 R6.1 R7.1 U2.2 U5.1 |
| `AMP_BYP` | 3 | C18.1 U4.2 U4.3 |
| `AMP_IN` | 3 | R13.2 R18.1 U4.4 |
| `AMP_SHDN` | 3 | R14.1 U2.21 U4.1 |
| `ANT_UHF` | 2 | J5.1 U5.12 |
| `AUDIO_3V3` | 10 | C10.1 C15.1 C24.1 C25.1 C8.1 FB3.2 R9.1 U3.11 U3.3 U3.4 |
| `BOOT_IO0` | 4 | H2.3 R7.2 SW1.1 U2.27 |
| `C17_N1` | 2 | MK1.4 R10.1 |
| `C17_N3` | 3 | C17.1 MK1.1 R9.2 |
| `C1_N3` | 2 | L1.1 U1.3 |
| `C1_N5` | 3 | R3.1 R4.2 U1.5 |
| `C7_N10` | 2 | C11.1 U3.17 |
| `C7_N12` | 2 | C14.1 U3.12 |
| `C7_N14` | 2 | C13.1 U3.13 |
| `C7_N17` | 2 | C7.1 U3.15 |
| `C7_N18` | 2 | C9.1 U3.14 |
| `C7_N8` | 2 | C12.1 U3.18 |
| `CC1` | 2 | J1.A5 R1.1 |
| `CC2` | 2 | J1.B5 R2.1 |
| `CHG_EN` | 2 | J2.3 U2.17 |
| `CHG_STAT` | 2 | J2.2 U2.15 |
| `DAC_N` | 2 | C13.2 R12.1 |
| `DAC_P` | 3 | C14.2 C30.1 R13.1 |
| `EN` | 5 | C5.2 H2.2 R6.2 SW2.1 U2.3 |
| `GND` | 67 | C1.2 C10.2 C15.2 C16.2 C17.2 C18.2 C19.2 C2.2 C20.2 C21.2 C22.2 C23.2 C24.2 C25.2 C26.2 C27.2 C28.2 C29.2 C3.2 C4.2 C5.1 C6.1 C7.2 C8.2 C9.2 D1.2 D2.2 D3.2 H1.1 H2.1 J1.1 J1.2 J1.3 J1.4 J1.A1B12 J1.B1A12 J5.2 J5.3 J5.4 J5.5 LED1.2 MK1.2 MK1.3 R1.2 R11.1 R12.2 R17.2 R2.2 R3.2 R8.1 R8.2 SW1.2 SW2.2 U1.2 U2.1 U2.40 U2.41 U3.10 U3.20 U3.21 U3.5 U4.7 U5.10 U5.19 U5.20 U5.7 U5.9 |
| `I2C_SCL` | 2 | U2.24 U3.1 |
| `I2C_SDA` | 2 | U2.25 U3.19 |
| `I2S_BCLK` | 2 | U2.5 U3.6 |
| `I2S_DIN` | 2 | U2.7 U3.9 |
| `I2S_DOUT` | 2 | U2.12 U3.7 |
| `I2S_LRCK` | 2 | U2.6 U3.8 |
| `I2S_MCLK` | 2 | U2.4 U3.2 |
| `LED1_N2` | 2 | LED1.1 R5.2 |
| `LED_STATUS` | 2 | R5.1 U2.20 |
| `MIC_AC` | 2 | C30.2 R16.1 |
| `MIC_N` | 2 | C11.2 R11.2 |
| `MIC_P` | 2 | C12.2 R10.2 |
| `SA818_AUDIO_IN` | 2 | U2.39 U5.3 |
| `SA818_MIC` | 3 | R16.2 R17.1 U5.18 |
| `SA818_PD` | 2 | U2.22 U5.6 |
| `SA818_PTT` | 3 | R15.1 U2.23 U5.5 |
| `SA818_RXD` | 2 | U2.11 U5.16 |
| `SA818_TXD` | 2 | U2.10 U5.17 |
| `SPK_AMP_N` | 2 | FB1.1 U4.8 |
| `SPK_AMP_P` | 3 | FB2.1 R18.2 U4.5 |
| `SPK_OUT_N` | 3 | C20.1 FB1.2 J4.2 |
| `SPK_OUT_P` | 3 | C21.1 FB2.2 J4.1 |
| `U0RXD` | 2 | H1.4 U2.36 |
| `U0TXD` | 2 | H1.3 U2.37 |
| `USB_DM` | 4 | D1.1 J1.A7 J1.B7 U2.13 |
| `USB_DP` | 4 | D2.1 J1.A6 J1.B6 U2.14 |
| `USB_VBUS_RAW` | 3 | F1.1 J1.A4B9 J1.B4A9 |
| `VBAT_ADC` | 2 | J2.1 U2.38 |
| `VBUS_5V` | 14 | C19.1 C2.1 C22.1 C23.1 C26.1 C27.1 C28.1 C29.1 D3.1 F1.2 U1.1 U1.4 U4.6 U5.8 |
| `VMID_BYP` | 2 | C16.1 U3.16 |

