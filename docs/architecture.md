# 架构

黑猫互联是一套"模拟对讲机 + 网络中继"的完整实现：手台固件跑在 ESP32-S3 上，
网页面板由手台自己提供，Android APP 通过局域网访问同一套 HTTP/WS 接口，
Windows 刷机工具负责把固件写进板子。

本文只描述**代码里确实存在**的结构与数值。凡是需要留待实测确认的地方，
都写成"以源码为准（路径）"，不代替实测结论。

---

## 1 系统组成

| 部分 | 目录 | 形态 | 位置 |
|---|---|---|---|
| 手台固件 | `firmware/` | ESP-IDF（C），交叉编译后烧进 Flash | ESP32-S3-WROOM-1 N16R8 |
| 网页面板 | `firmware/main/panel_index.html`、`setup.html` | 编译期 `EMBED_TXTFILES` 进固件，由手台 HTTP/HTTPS 直接提供 | 浏览器 |
| Android APP | `app/` | Kotlin，单一 `app` 模块 | 手机（仅局域网） |
| 刷机工具 | `flasher/` | Python 3 + Tkinter + esptool 4.8.1，PyInstaller 单文件 EXE | Windows 10/11 x64 |
| 辅助脚本 | `tools/` | PowerShell / Python | 开发机 |

固件、网页面板、APP 共用同一套设备侧接口（刷机工具走 USB，不参与）：

- **控制面**：`GET/POST /api/*`（JSON），用于状态、无线参数、信道、尾音、升级；
- **音频面**：`/ws/sdr`（WebSocket，双向 PCM），以及作为兼容通道的 `/api/sdr_tx`、`/api/sdr_rx`；
- **发现**：mDNS 服务 `_openradiolink._tcp`，以及手台每 10 s 向网关 UDP `54322` 发一条 JSON 通告。

APP 侧没有蓝牙权限、也没有任何 Bluetooth 代码，连接方式只有 Wi-Fi：
`app/app/src/main/AndroidManifest.xml` 只申请 `INTERNET` / `NEARBY_WIFI_DEVICES` /
`ACCESS_LOCAL_NETWORK` / `RECORD_AUDIO` 等网络与录音权限。

源码位置：`firmware/main/CMakeLists.txt`（EMBED_TXTFILES）、`firmware/main/net_http.c`
（路由表 `s_routes`）、`firmware/main/net_wifi.c`（`announce_gateway`）、
`app/app/src/main/java/net/openradiolink/app/DeviceDiscovery.kt`。

### 1.1 网页面板

`panel_index.html` 是一份自带 CSS/JS 的单文件页面（约 150 KB），没有前端构建步骤；
它被编译进固件，与 API 同源。配套还有 `setup.html`（AP 配网表单）、
`app_control.js`、`device_name.js`、`speaker_debug.html`。

面板同时跑在 **80（HTTP）** 和 **443（HTTPS）** 上：只有 SDR 语音（浏览器麦克风）
必须走 HTTPS，因为浏览器只在安全上下文里给 `navigator.mediaDevices`。
HTTPS 证书是构建期生成的自签证书（ECDSA P-256，SAN 只有 DNS 名），
`firmware/main/CMakeLists.txt` 在证书缺失时自动调用 `tools/gen_dev_cert.py`。

### 1.2 Android APP

- 设备档案（`DeviceStore`）用 AES-GCM 加密后写 `AtomicFile`，密钥在 AndroidKeyStore 里，
  不落地明文；
- 每个档案可有多个接入点（`AccessPoint`），支持 HTTP/HTTPS、Basic/Bearer 凭据、
  证书指纹绑定（首次连接记住服务端证书 SHA-256，之后指纹变化即拒绝）。
  注意：固件本身不校验 `Authorization`，这两个字段是给"板子前面挂了反向代理/网关"
  的场景准备的；直连时它们不起作用。
- 连接后读 `/api/status`、`/api/live`、`/api/radio`、`/api/wifi`、`/api/control`、`/api/channels`，
  音频走 `/ws/sdr`；
- 发话是"按住说话"：先 `ptt1:sid:tx:id`，再送 PCM，松手后由**板端**播尾音。

源码位置：`app/app/src/main/java/net/openradiolink/app/BoardSession.kt`、`BoardApi.kt`、
`PcmAudio.kt`、`DeviceStore.kt`、`MicrophoneInput.kt`、`AudioProtocol.kt`。

### 1.3 刷机工具

`flasher/` 是 GPL-3.0-or-later 的 Windows 单文件工具，底层直接调 esptool，
把"完整合并 BIN"拆成四个区段写入：`0x0` bootloader、`0x8000` 分区表、
`0xF000` otadata、`0x20000` 应用。两种模式（保留配置 / 全新刷机）见
[flashing.md](flashing.md)。

源码位置：`flasher/firmware.py`（镜像校验与区段计划）、`flasher/runner.py`
（多 COM 并行）、`flasher/worker.py`。

---

## 2 固件分层与主要任务

### 2.1 分层

| 层 | 模块 | 职责 |
|---|---|---|
| 入口 | `main.c` | 启动顺序、串口控制台、心跳/状态任务 |
| 板级 | `board_hw.c`、`board_pins.h`、`sa818*.c`、`audio_i2s.c`、`audio_codec.c`、`adc_audio_clock.c` | GPIO/I2C/UART/I2S/ADC、SA818 模块、ES8311 寄存器 |
| 音频 | `voice.c`、`voice_tail.c`、`eq6.c`、`g711.c`、`g722.c`、`opus_link.c` | 采集/播放、EQ、尾音、编解码 |
| 网络 | `net_wifi.c`、`wifi_policy.c`、`netcom.c`、`netp2pnet.c`、`netrelay.c`、`bridge.c` | 联网、模式管理、P2P、MQTT 中继、模数桥 |
| 服务 | `net_http.c`、`sdr.c`、`frontpanel.c`、`app_control.c`、`radio_activity.c` | HTTP/HTTPS/WS、浏览器音频、前面板、并发锁、活动快照 |

`netcom.c` 只做模式管理与引擎热切换：两种网络引擎都实现同一张函数表
（`start/stop/tx_sample/tx_end/rx_sample/stats/diag/test_tone`），
音频前端（麦克风→EQ）和后端（解码→EQ→功放）由所有模式共用。

### 2.2 启动顺序（`app_main`）

1. `board_safe_state_init()`：先把 PTT 与功放钉在安全状态，失败则直接停住；
2. `ptt_init()`：之后只有 `ptt_request_tx()` 能键控发射；
3. NVS、`sa818_cfg_init()`、PTT 任务、控制台任务；
4. `bringup_run()` 上电自检，然后心跳、状态任务；
5. 自动进入 `VOICE_MONITOR`（只听，不能发射）；
6. `net_wifi_start()` → `net_http_start()`（HTTP+HTTPS）；
7. `netrelay_init()`、`netp2pnet_init()`、`netcom_init()`、`bridge_init()`；
8. `voice_tail_init()`；
9. `frontpanel_init()`（最后，因为它能通过 `voice_start()` 触发发射）。

源码位置：`firmware/main/main.c`（`app_main`）。

### 2.3 任务与栈

| 任务 | 栈 | 优先级 | 核心 | 栈所在 | 职责 |
|---|---|---|---|---|---|
| `ptt` | 4096 | 6 | 任意 | 内部 RAM | 10 ms 周期跑 `ptt_tick()`，兼打音频电平行、500 ms 轮询 RSSI |
| `console` | 8192 | 5 | 任意 | 内部 RAM | 串口命令 |
| `mon` | 6144 | 12 | 1 | 内部 RAM | 射频接收→喇叭；尾音预览与模数桥通过 `voice_monitor_borrow()` 借用它 |
| `talk` | 6144 | 12 | 1 | `.bss` 静态 | 麦克风→SA818 或网络 |
| `hb` | 2560 | 4 | 任意 | 内部 RAM | 电源灯心跳、SW1 手势（3 s 开配网 AP / 10 s 全清） |
| `status` | 3072 | 3 | 任意 | 内部 RAM | 15 s 一条状态日志 |
| `panel_led` | 2304 | 4 | 任意 | 内部 RAM | 前面板 LED 独立节拍 |
| `frontpanel` | 3584 | 4 | 任意 | 内部 RAM | 20 ms 轮询 5 个按键，60 ms 去抖 |
| `httpd`（HTTP） | 10240 | 默认 | 任意 | 内部 RAM | 面板与 API |
| `httpd`（HTTPS） | 10240 | 默认 | 任意 | 内部 RAM | 同上，TLS |
| `wifi_mgr` | 6144 | 3 | 任意 | PSRAM | Wi-Fi 状态机 |
| `sdr_tx` | 4096 | 6 | 任意 | PSRAM | 浏览器音频泵 |
| `np2p_eng` | 6144 | 8 | 1 | PSRAM | P2P 收包/状态机 |
| `np2p_play` | 16384 | 8 | 1 | PSRAM | P2P 播放 |
| `np_tx` | 16384 | 5 | 任意 | PSRAM | P2P 发送（含 Opus 编码） |
| `np2p_stun` | 3072 | 3 | 0 | PSRAM | STUN/DNS |
| `netrel` | 16384 | 8 | 1 | PSRAM | MQTT 中继播放 |
| `rel_tx` | 16384 | 5 | 任意 | PSRAM | MQTT 中继发送 |

两条音频任务（`mon`、`talk`）都在核心 1、优先级 12，高于网络引擎的 8：
音频是硬实时消费者，网络任务任何阻塞都不能让 ADC 缓冲池溢出。
`talk` 的栈放在 `.bss` 而不是堆上——原因见 [troubleshooting.md](troubleshooting.md) 4.2。

源码位置：`firmware/main/main.c`、`voice.c`、`sdr.c`、`net_wifi.c`、`netp2pnet.c`、
`netrelay.c`、`frontpanel.c`、`app_control.c`；栈数值即各 `xTaskCreate*` 调用的实参。

### 2.4 HTTP/HTTPS 服务

| 配置 | HTTP :80 | HTTPS :443 |
|---|---|---|
| 任务栈 | 10240 | 10240 |
| 最大 socket | 9 | 5 |
| keep-alive | 开（idle 10 s / 2 s / 3 次） | 同 |
| LRU 回收 | 开 | 开 |
| 路由表 | 同一份 `s_routes` | 同 |

keep-alive 是必需项而不是优化：固件注释给出的板端实测是"一次 TLS 握手约 470 ms、
已建连接上的一个请求约 22 ms"，关掉 keep-alive 面板的轮询根本跟不上。
HTTPS 的 socket 上限压到 5，是因为每个 TLS 会话都吃内部 RAM，而 PTT 任务也要用。

`net_http.c` 另有两条针对栈的硬约束（见第 5 节）：
编译选项 `-Werror=frame-larger-than=6144`，以及 `-fstack-usage` 生成 `.su` 报告。

源码位置：`firmware/main/net_http.c`（`net_http_start`、`s_routes`）、
`firmware/main/CMakeLists.txt`（`set_source_files_properties`）。

### 2.5 音频采集/播放

`voice.c` 只有两条真实音频路径，都按 16 kHz / 单声道 / PCM16 处理：

- **MONITOR**：SA818 的 `AF_OUT`（GPIO1 / ADC1_CH0）→ 连续 ADC（DMA）→ 抽取到 16 kHz
  → 扬声器 EQ → ES8311 DAC → LM4871；
- **TALK**：麦克风（ES8311 ADC，经 I2S 读）→ 麦克风 EQ → 预加重 → TX 增益
  → 软限幅 → ES8311 DAC → SA818 `MIC_IN`，同时键控发射。

两条路径不能同时持有 DAC，因此有一个"单 DAC 所有者"约定：`talk` 独占时功放保持关闭
（否则自己的声音会被麦克风再收一遍）；尾音预览和模数桥通过
`voice_monitor_borrow("preview"/"bridge")` 借走 `mon` 任务，最后一个借用者结束时才还回去。

`talk` 的结束不是"松手就断"：`voice_request_talk_end()` 只置标志，`talk` 任务跳出主循环、
**仍保持键控**播完尾音、再用 `ptt_extend()` 覆盖整段尾音时长，最后才 `ptt_release()`。

源码位置：`firmware/main/voice.h`、`voice.c`（`talk_task`、`monitor_task`、
`voice_monitor_borrow`）、`audio_i2s.h`。

### 2.6 SDR（浏览器/APP 音频）

`/ws/sdr` 一次只允许**一个**监听者：射频音频只有一个环形缓冲，第二个客户端只会拿到
半句话。旧的 fd 静默超过 `SDR_WS_STALE_MS`（6 s）后才允许接管，
避免一个崩溃的标签页把流锁死到重启。

- 会话用 `sid` 标识，每次按下用一个严格递增的 `tx`；释放和 PCM 都带同一个 `tx`；
- WS 二进制帧 = `uint32 sid | uint32 tx | PCM`（最多 2040 字节）；
- 文本控制帧 = `ptt1:sid:tx:id` / `ptt0:sid:tx:id`，应答用同一个 `id` 关联；
- 所有发送都发生在 httpd 自己的任务里，没有跨任务写 socket。

关键超时（`sdr.c` 宏）：`SDR_PTT_HOLD_MAX_MS` 30 s（单次按住上限）、
`SDR_TX_STALL_MS` 2 s（收到第一块 PCM 后又静音）、`SDR_NO_PCM_GRACE_MS` 3 s、
`SDR_RELEASE_MAX_US` 6 s（释放+尾音的安全上限）、`SDR_RELEASE_NOTONE_US` 600 ms
（无尾音时的排空上限）、`SDR_NO_CLIENT_STALE_MS` 30 s（无任何客户端即视为孤儿会话）。

源码位置：`firmware/main/sdr.h`、`sdr.c`、`net_http.c`（`h_ws_sdr`、`h_sdr_ptt`、`h_sdr_tx`）。

### 2.7 网络 P2P / 中继

见第 4 节。

### 2.8 桥接（RF ↔ 网络）

`bridge.c` 是一个状态机，满足全部条件才"激活"：操作者启用（NVS 持久化）、
选中某个预置信道、当前是 P2P 或中继模式、该模式的引擎已启动、SA818 已可发射。

- **RF → 网络**：借用 `mon` 任务，把已经过静噪门控的接收音频按块送进当前引擎，
  静噪状态作为线上的 PTT 标志；
- **网络 → RF**：由引擎的播放任务直接调 `ptt_bridge_key()` 与 `voice_bridge_inject()`，
  用房间音频的包络驱动键控与功放；
- **半双工**：桥自己键控期间，RF→网络方向被静音，否则模块会在自己发射时开静噪，
  形成自激回环；
- 桥的键控有 30 s 看门狗（`PTT_BRIDGE_MAX_MS`），播放任务每包刷新一次，
  任务死掉就自动松开。

源码位置：`firmware/main/bridge.h`、`bridge.c`、`ptt.c`（`PTT_BRIDGE_MAX_MS`）。

### 2.9 前面板

三个 LED（电源/收发）加五个按键：PTT 键 + 四个可改绑功能键。
按键 20 ms 轮询、连续 3 次相同才认（60 ms 去抖）；PTT 键**不可改绑**。
LED 策略是 `rf_steady_net_5hz`：射频收发常亮，真实网络语音按 5 Hz 闪。

动作枚举：`off / ch± / vol± / sql± / power / monitor / mode / bridge`。

源码位置：`firmware/main/frontpanel.h`、`frontpanel.c`、`radio_led.c`、`radio_activity.c`。

---

## 3 音频链路

### 3.1 采样与格式

| 项 | 值 | 出处 |
|---|---|---|
| 采样率 | 16000 Hz | `audio_i2s.h` `AUDIO_SAMPLE_RATE_HZ` |
| 位深/声道 | PCM16，单声道 | 同上 |
| 网络包 | 20 ms = 320 样本 | `AUDIO_PKT_MS` / `AUDIO_PKT_SAMPLES` |
| Opus | 16 kHz、20 ms、32 kbit/s CBR | `opus_link.h` |
| Eq 系数 | 按 `AUDIO_SAMPLE_RATE_HZ` 计算 | `eq6.c` `EQ6_SR` |

`opus_link.h` 的文件头注释里还写着 12 kHz / 40 ms / 24 kbit/s，那是历史值；
以该文件里的宏（`OPUS_LINK_RATE_HZ 16000`、`OPUS_LINK_FRAME_MS 20`、
`OPUS_LINK_BITRATE 32000`）为准。

### 3.2 硬件与信号

| 信号 | 连接 |
|---|---|
| I2S MCLK/BCLK/LRCK | GPIO4 / 5 / 6 |
| I2S DIN（codec→ESP32）、DOUT（ESP32→codec） | GPIO7 / 8 |
| ES8311 控制 | I2C（0x18） |
| 射频接收音频 | `AF_OUT` → GPIO1 = ADC1_CH0（**不经过 ES8311**） |
| 电池电压（多为空贴） | GPIO2 = ADC1_CH1 |

接收音频走 ESP32-S3 的连续 ADC：`adc_audio_clock.c` 用链接期 `--wrap` 把
ADC 分频改成 80 MHz / 25 / (2×50) = 恰好 32000 Hz，再 2:1 抽取到 16 kHz。
IDF 默认的 78 分频会得到 32051.28 Hz，大约每 10 s 丢一帧，所以这里必须精确。

源码位置：`firmware/main/board_hw.h`、`audio_i2s.h`、`adc_audio_clock.c`、
`firmware/CMakeLists.txt`（`--wrap=adc_hal_digi_controller_config`）。

### 3.3 一条接收音频的完整去向

```
SA818 AF_OUT ─ ADC1 连续 DMA(32 kHz) ─ 2:1 ─ 16 kHz PCM16
      ├─ EQ(扬声器 6 段) ─ ES8311 DAC ─ LM4871 ─ 喇叭     （MONITOR）
      ├─ sdr_rx_push()  ─ /ws/sdr ─ 浏览器 / APP          （SDR）
      └─ bridge_rx_tap() ─ netcom_tx_sample() ─ 网络引擎  （桥接）
```

发射方向反过来：麦克风 / 浏览器 PCM / 网络注入 → 同一套麦克风 EQ → 预加重 → 增益
→ 限幅 → 要么进 SA818 `MIC_IN`，要么进网络引擎，要么两者同时（`netcom_rf_tx`）。

### 3.4 尾音

尾音是"唯一来源在板端"的礼貌音：松键后发射机继续保持键控一小段时间，
把配置好的尾音真正发出去（本地走出 SA818 上天，网络模式当普通音频帧发出）。
手机不再自播 880/660 Hz，只负责停止采集并让发送队列排空。

内置音在启动时合成到一块 PSRAM 池（`POOL_SAMPLES` = 20000 样本，所有内置音共用；
池满时排在前面的音优先，后面的会被截断，严重时日志报 `tone pool overflow`）：
单音 880 Hz/150 ms、摩斯 K（K-CHOO）、Baofeng ROGER、Motorola MDC-1200、
Icom `*`/`#`、UV-K5 双响。

自定义尾音支持 WAV（16-bit PCM，单/双声道，4.8–96 kHz）与 MP3（MPEG-1/2 Layer III，
8–48 kHz），板端线性重采样到当前流采样率（`AUDIO_SAMPLE_RATE_HZ` = 16 kHz）、
峰值归一化到 -2.5 dBFS、μ-law 编码后存进 1 MB 的 `storage` 分区。
输出样本数上限是 `TAIL_CUSTOM_MAX_SAMPLES`（40000），按 16 kHz 算约 2.5 s；
头文件注释里的"5 s / 8 kHz"是 8 kHz 时代的值，以宏和 `voice_tail.c` 的重采样循环为准。
上传体积上限 `TAIL_UPLOAD_MAX` = 512 KB。

尾音按模式掩码生效（`TAIL_MODE_LOCAL/NETP2P/RELAY`，默认全开），
`TAIL_OFF` 才是全局关闭。

源码位置：`firmware/main/voice_tail.h`、`voice_tail.c`、`voice.c`
（`talk_task` 尾部、`voice_sdr_tail_start`）、`sdr.c`（释放路径）。

### 3.5 EQ

六段固定频点：150 Hz 低架、400 / 900 / 2k / 3.2k 峰值、3.8 kHz 高架；
范围 ±12 dB，0 dB 时每个 section 都是严格的直通（unity + 线性相位）。
两个实例：扬声器路径、麦克风路径，全部存 NVS、全局生效。
系数用 Audio EQ Cookbook 公式，按 16 kHz 计算。

源码位置：`firmware/main/eq6.h`、`eq6.c`、`voice.c`（`voice_eq_set`）。

---

## 4 网络模式

`netcom.c` 管三种模式（NVS 持久化，热切换，不重启）：

| 值 | 名称 | 传输 | 分组 | 说明 |
|---|---|---|---|---|
| 0 | `local` | 无 | — | 纯模拟，SA818 收发；默认模式 |
| 1 | `netp2p` | UDP 直连（STUN 打洞） | 房间号 1000–9999 | 公网/局域网点对点，可开 hub 转发 |
| 2 | `relay` | MQTT（EMQX） | broker 即范围 | FMO 式中继：broker 就是中继台 |

模式编号在 2026-10-16 变更过（旧的 ESP-NOW 模式被删除），NVS 里带版本号
`ncmodev`，旧值会被翻译；没有版本戳的旧值一律按旧语义迁移，
避免"存着 1"的手台被静默送上公网。

### 4.1 局域网直连（控制面）

"局域网直连"指的是**访问方式**，不是第四种互通模式：手台加入操作者的 Wi-Fi 后，
浏览器/APP 直接用 `http://<ip>/`、`https://<ip>/` 或 `http://<hostname>.local/` 访问。
没有云、没有账号；面板与 API 都在板子上。

- 默认 mDNS 主机名来自 NVS（`heimao`，旧板保留 `orl`），可改；
- 多台手台在同一网段时必须各用各的主机名，否则 `.local` 会撞名；
- 未存任何 Wi-Fi 时自动开配网 AP：开放热点 `黑猫智能手台-XXXXXX`（MAC 后 3 字节），
  地址 `192.168.4.1`，`dns_server` 组件做强制门户式跳转；
- AP 与已关联的 STA 不会共存（APSTA 只用于未关联时的扫描）。

源码位置：`firmware/main/net_wifi.c`（`start_mdns`、`net_wifi_setup_ap_start`、
`announce_gateway`）、`net_wifi.h`。

### 4.2 NET-P2P（模式 1）

- 主 UDP socket 固定 `54321`，控制帧与音频共用；`54322` 只用于探测 NAT 是否保留端口；
- 帧头 14 字节：magic `0x4F52`、房间号、seq、发送方 MAC、flags（bit0=PTT）、type；
  其后是 `HMAC-SHA256(PSK, room‖mac‖"ORL-NP2P")[:8]`；
- type：`0` 音频、`1` KNOCK、`2` KNOCK_ACK、`3` PING、`4` PONG、`5` BYE；
- 状态机：`KNOCK → ACK → LINKED`，静默 90 s 变 `EXPIRED`；
- 音频负载是固定长度的 Opus 帧，接收端按对端各一个 PSRAM 抖动缓冲（128 槽）；
- PSK 是 13 字节的 `netcom` PSK（NVS `ncpsk`），未设置时用内置默认值；
  房间号或校验不对的帧直接丢弃；
- hub 模式可让一台手台把成员音频原样转发给其他已连接成员。

源码位置：`firmware/main/netp2pnet.c`（文件头协议说明）、`netp2pnet.h`、`netcom.h`。

### 4.3 MQTT 中继（模式 2）

- 发布 `orl/tx/<device_id>`，订阅 `orl/tx/+`；`device_id` 是 MAC 后三字节；
- 普通 MQTT 3.1.1、QoS 0，URI 固定拼成 `mqtt://<host>:<port>`（默认端口 1883），没有 TLS；
  支持用户名/口令（EMQX 认证或任何 broker 的 password file），清空用户名即回到匿名；
- 自己发的消息会经通配订阅回到自己，靠帧头里的 MAC 过滤掉；
- 中继没有房间号：连到同一个 broker 且订阅 `orl/tx/+` 的人都能听到，这就是该模式的定义；
- 接收侧与 P2P 共用同一套带 seq 的抖动缓冲与播放任务。

源码位置：`firmware/main/netrelay.c`（文件头）、`netcom.h`（`NETCOM_USER_LEN` 等）。

### 4.4 模式对比

| | local | netp2p | relay |
|---|---|---|---|
| 需要基础设施 | 无 | STUN（内置默认列表） | 一个 MQTT broker |
| 分组 | 无 | 房间号 1000–9999 + PSK | broker 范围 |
| 音频编码 | 无（模拟） | Opus 32 kbit/s | Opus 32 kbit/s |
| 是否可桥接射频 | — | 是 | 是 |
| 加密 | — | 帧级 HMAC 校验（非加密） | 取决于 broker（默认明文） |

### 4.5 RF ↔ 网络桥接

见 2.8。注意两个容易误解的点：

1. 桥接只在 P2P/中继模式且引擎已启动时激活，local 模式没有可桥的对象；
2. 桥接不改写操作者当前的射频参数，只有"显式选择预置信道"才会重新调谐 SA818。

---

## 5 控制权与并发

### 5.1 控制租约：已移除，只剩兼容层

早期版本有一个 15 s（自动）/ 60 s（手动）的控制租约，`control_lease.c` 里还留着
完整的实现（token 64 字节、generation 递增、`CONTROL_LEASE_MS` / `CONTROL_MANUAL_MS`），
但**当前固件不再使用它**：`app_control.c` 的 `app_control_begin()` 只取一把递归互斥锁，
`/api/control` 对 `claim/takeover/renew/release` 一律接受，并固定返回
`"held":false, "mine":true`。也就是说这是"单操作者设备"，
并发写由锁串行化，而不是由租约仲裁。

APP 侧与此对齐：`BoardSession.ensureLease()` 已是空实现，
`applyControl()` 固定把 `mine` 当作 true，不再静默抢控制权。

源码位置：`firmware/main/app_control.c`、`control_lease.c`、`control_lease.h`；
`app/app/src/main/java/net/openradiolink/app/BoardSession.kt`（`applyControl`、`ensureLease`）。

### 5.2 变更串行化

走 HTTP 的写操作与音频会话请求（`h_set`、`h_channel`、`h_tail_upload`、`h_ota`、
`h_talk`、`h_provision`、`h_wifi` POST、`h_sdr*`、`h_ws_sdr` 等，由
`route_needs_control()` 判定）都经过同一把递归互斥锁
（`app_control_begin` / `app_control_end`，100 ms 超时，取不到返回 `409 Conflict`）。
这保证上传（OTA、尾音）期间不会有另一个 HTTP 写操作插进来。

串口控制台与前面板按键**不走这把锁**，它们各自有自己的约束（PTT 状态机、
音频任务的生命周期锁），所以"并发"在这两条路径上仍然可能发生，
只是不会破坏 PTT 安全规则。

### 5.3 sid / tx

`sid` 标识一次收听会话，`tx` 标识一次按下。规则：

- `tx` 严格递增；正常释放要带同一个 `tx`（HTTP 兼容通道的 `id=0` 表示不需要应答关联）；
- 板端只认"当前 sid + 当前 tx"的 ON；OFF 有一个刻意的例外：
  `tx >= 当前 tx` 的 OFF 一律接受，因为快速连按时 OFF 可能先于 ON 到达板端；
- 板端已经接受过 ON 却收到不匹配 `tx` 的 ON，会拒绝（`busy`），
  并且**不推进** `s_tx_id`——否则之后用一个板端从未接受的 token 去释放，
  真正的持有者会一直被锁住。

拒绝原因（`/api/sdr_ptt` 与 `/api/sdr_state` 的 `reason` 字段）：
`busy`（上一个持有者还在尾音/释放中）、`owner_lost`（PTT 已不属于这个 SDR token）、
`hold_limit`（超过 30 s 按住）、`pcm_timeout`、`network_not_ready`、`radio_not_ready`。

源码位置：`firmware/main/sdr.c`（`sdr_ptt_request`）、`net_http.c`、`sdr.h`。

### 5.4 PTT 所有权

PTT 模块只有一个键控入口（`pad_apply`），电平是两路键控域的"或"：

| 键控域 | 持有者 | 说明 |
|---|---|---|
| 操作者状态机 | `PTT_OWNER_TALK` | PREPARE→TX→TAIL，带绝对天花板 |
| 网络/浏览器 | `PTT_OWNER_NETWORK` / `PTT_OWNER_SDR` | 桥接键、SDR 发话 |

安全网：每次发射有硬截止（默认 30 s，语音会话按时续期，绝对上限 180 s）；
`ptt_tick()` 必须至少每 250 ms 跑一次，否则判 `PTT_FAULT` 并释放；
桥接键另有 30 s 看门狗。`ptt.h` 的注释明确写了：
"这是固件里唯一允许键控发射机的代码"。

> 发射需要持照操作者与匹配天线/假负载，固件无法检查这一点。

源码位置：`firmware/main/ptt.h`、`ptt.c`。

### 5.5 单会话约束

| 资源 | 约束 | 表现 |
|---|---|---|
| `/ws/sdr` 监听者 | 同时 1 个（旧连接静默 6 s 后才可接管） | 第二个客户端被拒绝 |
| SDR 会话 | 同时 1 个 `sid` | `/api/sdr?on=1` 返回 "audio session busy or TALK active" |
| DAC | 同时 1 个所有者 | TALK 期间功放关闭、预览借用 `mon` |
| PTT | 同时 1 个 owner | 其他来源被拒 |
| 网络引擎 | 每个模式 1 个 | 切模式先停旧引擎再起新引擎 |

---

## 6 资源约束

### 6.1 芯片与存储

| 项 | 值 | 出处 |
|---|---|---|
| 目标 | `esp32s3`（模组 N16R8：16 MB Flash，8 MB PSRAM） | `sdkconfig`、[hardware.md](hardware.md) |
| PSRAM | Octal，80 MHz，`CONFIG_SPIRAM_USE_MALLOC` | `sdkconfig` |
| PSRAM 内部保留 | `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` | `sdkconfig` |
| 小于 2 KB 的分配 | 默认走内部 RAM（`MALLOC_CAP_ALWAYSINTERNAL=2048`） | `sdkconfig` |
| 栈可放 PSRAM | `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | `sdkconfig` |

### 6.2 内部 RAM 是稀缺资源

任务栈、lwIP pbuf、httpd 会话、Wi-Fi 驱动都从内部 RAM 出。`/api/status` 同时给出
四个数：`heap`（总空闲）、`heapInt`（`MALLOC_CAP_INTERNAL` 空闲）、
`heapIntMax`（内部 RAM 最大连续块）、`psram`（PSRAM 空闲）。
固件自己的注释写明：内部 RAM 紧张时先出现的是
"wifi: mem fail"或丢控制帧，而不是 panic。

因此凡是"大而长命"的缓冲都放 PSRAM：

- 网络引擎的任务栈（`MALLOC_CAP_SPIRAM`）、抖动缓冲、TX 队列；
- SDR 收发环形缓冲、`sdr_tx`/`sdr_tone` 任务栈；
- `/api/status` 每次请求单独分配 12288 字节响应缓冲，并为完整 16 个 P2P 对端预留 5120 字节。

### 6.3 两个栈相关的硬约束

1. **HTTP 任务栈 10 KB**：`net_http_start()` 把 HTTP 与 HTTPS 两个 server 的
   `stack_size` 都设为 10240，`max_uri_handlers` 按路由表长度 +2。
2. **单帧上限 6144 字节**：`CMakeLists.txt` 给 `net_http.c` 单独加
   `-Werror=frame-larger-than=6144` 和 `-fstack-usage`。
   这不是风格要求，而是一次真实崩溃的补丁——见 [troubleshooting.md](troubleshooting.md) 4.1。

同一条约束在代码里还有几处落地：`/api/scan` 的 AP 列表（32 条约 3.5 KB）
在堆上分配而不是放处理函数栈；`h_status` 的大 JSON 也在 PSRAM。

### 6.4 音频缓冲

| 缓冲 | 大小 | 位置 |
|---|---|---|
| SDR RX 环 | `AUDIO_SAMPLE_RATE_HZ * 3`（3 s） | PSRAM |
| SDR TX 环 | 8192 样本 | PSRAM |
| P2P 每对端抖动缓冲 | 128 槽 ×（Opus 帧 + 序号 + 有效位），首次收音时分配 | PSRAM |
| P2P 对端数 | 16 个持久 + 1 个自测槽 | — |
| 中继抖动缓冲 | 128 槽 | PSRAM |

源码位置：`firmware/sdkconfig`、`firmware/main/CMakeLists.txt`、
`firmware/main/net_http.c`、`sdr.c`、`netp2pnet.c`、`netrelay.c`、`voice.c`。

---

## 7 存储与升级

### 7.1 分区表

`firmware/partitions.csv`（16 MB，OTA 可用）：

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| bootloader | — | `0x0` | ~20 KB |
| 分区表 | — | `0x8000` | 4 KB |
| `nvs` | data/nvs | `0x9000` | `0x6000`（24 KB） |
| `otadata` | data/ota | `0xF000` | `0x2000` |
| `phy_init` | data/phy | `0x11000` | `0x1000` |
| `ota_0` | app/ota_0 | `0x20000` | `0x400000`（4 MB） |
| `ota_1` | app/ota_1 | `0x420000` | `0x400000`（4 MB） |
| `storage` | data/spiffs | `0x820000` | `0x100000`（1 MB） |
| `coredump` | data/coredump | `0x920000` | `0x40000`（256 KB） |

coredump 从 64 KB 提到 256 KB 是必须的：这套固件有 20 多个任务栈
（两个 10 KB 的 httpd、8 KB 控制台），64 KB 存不下时 IDF 会直接放弃写 dump，
panic 现场一点证据都不留。

### 7.2 NVS 里存了什么

| 命名空间 | 内容 |
|---|---|
| `orl` | 射频参数、信道表、EQ、增益、尾音选择与模式掩码、桥接开关/信道、`netcom` 配置（模式/房间/PSK/broker/账号）、前面板键位映射、自定义名称、`sqgp` 等 |
| `orl`（`NVS_NS_NET` 也是 `"orl"`） | Wi-Fi 档案表 `wprofiles1`（多 SSID、优先级、自动加入、主机名）、旧键 `whost`/`wssid`/`wpass` |
| `nvs.net80211` | Wi-Fi 驱动自己的凭据副本（"忘记全部"时一并清除） |

Wi-Fi 档案是"版本化 blob + 旧键保留"的结构：迁移时不删旧键，只有显式
"忘记全部"才清 `wssid`/`wpass`，这样回刷旧固件仍能联网。

### 7.3 两种 BIN

| 产物 | 组成 | 用途 | 谁校验 |
|---|---|---|---|
| 完整合并 BIN（full） | bootloader@0x0 + 分区表@0x8000 + otadata@0xF000 + 应用@0x20000 | USB 刷机（保留配置 / 全新） | 刷机工具（结构、分区表 MD5、段校验、SHA256、项目名、布局、填充区必须全 `0xFF`） |
| 应用 BIN（app） | 只有应用镜像 | 面板/APP 的 OTA 上传 | 固件 `esp_ota_end()` |

"保留配置"只写这四个区段，不把中间的 `0xFF` 填充区写进 NVS/PHY；
同时重新写 otadata（全 `0xFF`），让启动选择回到 `ota_0`。
把整片 full BIN 从 `0x0` 连续写入会覆盖 NVS/PHY 的填充区，等于清配置。

### 7.4 OTA 流程

`POST /api/ota`（body 就是应用镜像）：

1. 拒绝小于 64 KB 的上传（`image too small`）；
2. 选 `esp_ota_get_next_update_partition()` 的**非活动**槽，超过槽大小直接拒绝；
3. 拒绝在通话 / PTT 非 IDLE / PTT 被占用时升级；
4. **先把网络引擎和 SDR 停掉**（模式切到 local），再开始写 Flash——
   Flash cache 在写入期间关闭，而引擎跑在 PSRAM 栈上并持续访问 PSRAM，
   这是"中继/P2P 运行时 OTA 偶发失败"的根因；
5. 分块写，15 s 无进展或 180 s 总超时即 `esp_ota_abort()`；
6. `esp_ota_end()` 校验镜像 → `esp_ota_set_boot_partition()` → 回 200 → 1.2 s 后重启。

升级后板子回到 `local` 模式（不自动恢复原模式），这是刻意的安全状态。

**没有自动回滚**：`sdkconfig` 里 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 未开启。
坏镜像只要通过 `esp_ota_end()` 就会被启动；恢复手段是串口重刷
（保留配置模式会重写 otadata，使启动回到 `ota_0`）。

源码位置：`firmware/partitions.csv`、`firmware/sdkconfig`、`firmware/main/net_http.c`
（`h_ota`、`h_firmware_bin`）、`firmware/main/net_wifi.c`（档案迁移）、
`flasher/firmware.py`（`Firmware.parse`、`flash_steps`）、`flasher/make_firmware.py`。

### 7.5 面板上的固件回读

`GET /api/firmware.bin` 返回的是**当前运行分区的应用镜像**（先用
`esp_image_verify` 验长度，再按 1 KB 分块读），文件名是 `heimao-firmware.bin`。
它可以直接用于 OTA，但**不能**当完整合并 BIN 去刷机。
