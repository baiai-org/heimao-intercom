# 硬件

本仓库只包含固件与软件，**不包含原理图、PCB 与 BOM**。下面的引脚与模块信息足以理解固件行为、排查接线与做二次开发。

## 板卡

| 项 | 值 |
|---|---|
| 模组 | ESP32-S3-WROOM-1 **N16R8** |
| Flash | 16 MB |
| PSRAM | 8 MB（内嵌，Octal） |
| USB | ESP32-S3 原生 USB（GPIO19/20）直连 Type-C，无 USB-UART 芯片 |
| 下载 | 枚举为 `USB-Serial/JTAG`（VID 303A），通常无需按 BOOT |

原生 USB 的好处是：一根数据线即可刷机、看串口；备用通路是 `U0TXD(GPIO43)/U0RXD(GPIO44)` + `EN`/`BOOT_IO0` 接外部 USB-TTL。

## 射频

| 功能 | 连接 |
|---|---|
| SA818S 模块 | UART：`TXD=GPIO17`、`RXD=GPIO18`（交叉接线） |
| PTT | `GPIO21` |
| 模块 PD（省电/使能） | `GPIO14` |
| 高/低功率 | `GPIO10` 开漏输出（低=低功率，释放=高功率） |
| 音频 | 模块模拟音频经 ES8311 编解码器进出 |
| 静噪/COS | 默认 `GPIO40`，可用 NVS `sqgpio` 覆盖；`0` 表示显式关闭 |

UHF/VHF 共用同一套程序，实际频段由所装 SA818S 模块决定；频率、功率、亚音、静噪都由用户在面板/APP 显式设置，固件不会自动改写。

> 发射前必须接天线或假负载。无天线发射可能损坏模块。

## 音频

| 信号 | GPIO |
|---|---|
| I2S MCLK | 4 |
| I2S BCLK | 5 |
| I2S LRCK | 6 |
| I2S DIN（codec → ESP32） | 7 |
| I2S DOUT（ESP32 → codec） | 8 |
| I2C SCL / SDA（ES8311） | 47 / 48 |

- 编解码器：ES8311（I2C 地址与寄存器见 `firmware/components/es8311/`）；
- 扬声器功放：LM4871 类模拟功放，带使能脚；
- 采样率：射频/网络链路按 16 kHz 单声道 PCM16 处理，具体路径见 `voice.c`、`sdr.c`。

## 前面板（v0.4）

| GPIO | 用途 |
|---|---|
| IO12 | 电源灯（心跳） |
| IO11 | RX 状态灯（射频或网络接收） |
| IO42 | TX 状态灯（发射键控） |
| IO41 | PTT 按键（低有效） |
| IO38 / IO39 | 音量 + / −（低有效） |
| IO15 / IO16 | 信道 + / −（低有效） |

四个功能键可改绑（NVS `orl/fp_act`），动作包括 off / ch± / vol± / sql± / power / monitor / mode / bridge，见 `frontpanel.h`。

## 引脚表

完整引脚映射（模组焊盘号 ↔ GPIO）见 [PINMAP.md](PINMAP.md)；代码中的权威定义是 `firmware/main/board_pins.h`。更换 PCB 或改线后，先更新网表并用 `tools/gen_pinmap.py` 重新生成，不要只改一处。

## 供电与结构

- 整机 5 V 供电（USB 或外部 5 V），SA818S 发射瞬间电流较大，电源要能提供足够余量并加去耦；
- 射频与数字地分开走，天线远离 DC-DC 与 USB；
- 外壳、按键、天线接口不在本仓库范围内。

## 合规

频率、功率、执照要求因国家/地区而异。使用前请确认当地法规；本项目不提供任何频段合法性保证。
