# /ws/sdr 音频通道协议

网页与 APP 的实时语音走一条 WebSocket：下行（手台 → 客户端）是裸 PCM，上行（麦克风 → 手台）是带 8 字节头的 PCM，文本帧只承载 PTT 指令与状态。音频格式固定 **PCM s16le、16 kHz、单声道**，一块 20 ms（320 采样）。

设计要点（源码注释原文意思）：PTT 显式、与音频幅度无关；用一个会话号 `sid` 和一个严格递增的发射号 `tx` 把迟到的 HTTP/WS 报文隔离在外；所有下行帧只由持有该 socket 的 httpd 任务发送，绝不跨任务写同一个 socket。

## 1. 会话建立与握手

音频会话是“先开引擎，再挂 socket”两步，顺序不能反：

| 步骤 | 请求 | 结果 |
| --- | --- | --- |
| 1 | `GET /api/sdr?on=1&client=<uint32 nonce>` | 引擎开启，返回 `{"ok":true,"sid":N,...}`；`client` 必填且非 0 |
| 2 | `GET /ws/sdr?sid=<N>`（Upgrade） | 握手 101；`sdr_ws_attach()` 通过后才建立会话上下文 |
| 3 | 客户端发心跳/音频帧 | 每收到一帧就回一帧下行音频（有数据才发） |
| 4 | `GET /api/sdr?on=0&sid=<N>[&force=1]` | 关引擎；正常关闭在“仍按着 PTT”时被拒 |

握手 query 参数：

| 参数 | 必填 | 含义 |
| --- | --- | --- |
| `sid` | 是 | `/api/sdr?on=1` 返回的会话号；不匹配则 `sdr_ws_attach()` 失败，握手不成立 |
| `activity` | 否 | `=1` 时在每条状态应答里附带 `activity` 快照，并在下行 tick 上推送活动变化 |

请求头：APP 的 `BoardApi.builder()` 会带 `X-ORL-Device`（本机设备号），但本仓库固件不校验身份类头——`app_control.c` 里的 `request_token()` 没有任何调用者。WebSocket 路由和其它写路由一样要过串行锁（`route_needs_control()` 含 `h_ws_sdr`）。

握手成功即清空接收环（`s_rx_tail = s_rx_head`），避免把挂接前的旧音频当成新会话开头。

源码位置：`firmware/main/net_http.c:2817-2866`、`firmware/main/net_http.c:2574-2586`、`firmware/main/sdr.c:672-691`、`firmware/main/app_control.c:105-117`。

## 2. 二进制帧格式

### 下行（手台 → 客户端）

| 帧 | 长度 | 内容 |
| --- | --- | --- |
| 音频 | 2..3840 B，偶数 | N 个 int16 LE 采样，N ≤ 1920（6×320） |
| 空帧 | — | 没有可发音频时**不发空帧**，客户端要持续 tick |

- 每个 tick 最多聚合 6 个 20 ms 包（1920 采样 = 120 ms）成一帧，减少 TLS 记录数与往返；
- 聚合后整帧 ≤3840 B；`s_sdr_pcm` 是 HTTP 兜底与 WS 共用的一块缓冲，由互斥量保护，取不到锁就跳过本次 tick；
- 无时间戳、无序号：音频顺序完全依赖 TCP/WS 有序，丢帧由板端计数器（`sdr.werr`、`sdr.drop`）体现。

### 上行（客户端 → 手台）

| 偏移 | 长度 | 含义 |
| --- | --- | --- |
| 0..3 | 4 | `sid`，LE uint32，必须等于会话号 |
| 4..7 | 4 | `tx`，LE uint32，必须等于板端已接受的发射号 |
| 8.. | 2..2040 | PCM s16le 采样（≤1020 采样） |

| 规则 | 值 | 违反后果 |
| --- | --- | --- |
| 整个二进制帧上限 | 2048 B（含 8 B 头） | 超限按 `ESP_ERR_INVALID_SIZE` 处理并断开 |
| 多字节帧最小长度 | ≥10 且为偶数 | 断开该 WS 会话 |
| 单字节帧 | 任意 1 字节 | **只当心跳**，不解析头，仅触发下行取数 |
| `sid` 不匹配 | — | 帧被丢弃，处理返回失败 |

上行 PCM 进入 TX 环（8192 采样，约 512 ms）；写环前还要同时满足：会话有效、`ptt_hold`、`tx == s_tx_id`、`ptt_owned(PTT_OWNER_SDR, tx)`。数字化静音也算“收到 PCM”，固件不做电平门限。

源码位置：`firmware/main/net_http.c:2592-2703`、`firmware/main/sdr.c:428-452`、`firmware/main/sdr.h:1-7`。

## 3. 控制消息

### 文本帧：PTT

语法固定，整串严格匹配（`sscanf` 后要求 `%n` 消费到串尾，`on` 只能是 0/1）：

```
ptt1:<sid>:<tx>:<request>     # 按下
ptt0:<sid>:<tx>:<request>     # 松开
```

- `<sid>` 必须等于本 socket 的会话号，否则 `ok:false`；
- `<tx>` 每次按下严格递增；`tx` 相等且仍按着时重发是幂等的，不会延长 30 s 上限；
- `<request>` 只用于关联应答，板端原样回显为 `id`。

### 文本帧：应答/状态

每次文本帧都回一帧 JSON（`sdr_state_json()`）：

| 字段 | 含义 |
| --- | --- |
| `ok` | 本条 PTT 是否被接受 |
| `sid`、`tx`、`id` | 会话号、板端发射号、请求关联号（客户端必须三者都对上才认这次应答） |
| `ptt` | 是否仍持有发射 |
| `owner` | `none`/`talk`/`network`/`browser` |
| `reason` | `transmitting`/`tail_pending`/`tail`/`released`/`pcm_timeout`/`hold_limit`/`owner_lost`/`ws_timeout`/`ws_replaced`/`disconnected`/`force_close`/`disabled`/`busy`/`network_not_ready`/`radio_not_ready`/`cancelled`/`idle` |
| `on`、`rel`、`tail` | 引擎开、正在释放（尾音窗口）、尾音已开始 |
| `activity` | 仅握手带 `activity=1` 时追加（见下） |

`reason` 是客户端做提示的唯一依据：`busy`＝上一轮尾音/释放未结束；`owner_lost`＝发射占用已失效；`hold_limit`＝到 30 s 上限；`network_not_ready`/`radio_not_ready`＝路由未就绪。

### 其它帧

| 帧 | 处理 |
| --- | --- |
| PING | 回 PONG |
| CLOSE | detach；若仍按着 PTT 走 `release_locked("disconnected")` 立即掉载波 |
| 其它类型 | 忽略 |

### 活动快照（`activity=1`）

握手带 `activity=1` 时，同一 socket 上还会收到文本帧：

```json
{"type":"activity","device_id":"...","boot_id":"...","sample_us":N,
 "rf_rx":true,"rf_tx":false,"net_rx":false,"net_tx":false,
 "tx_active":false,"rx_active":true,"ic_mode":0,"net_ready":true}
```

只在状态变化或距上次 ≥1 s 时发送（含静默接收），因此可当作低频心跳用。

### 关闭、查询与上报

| 目的 | 请求 | 要点 |
| --- | --- | --- |
| 关会话 | `GET /api/sdr?on=0&sid=<N>&force=0\|1` | 按着 PTT 时普通关闭被拒（`ok:false,"audio session transmitting"`）；`force=1` 可中断发射；尾音进行中关闭会取消尾音并立即掉载波 |
| 查状态 | `GET /api/sdr_state?sid=<N>` | 只读，不消耗音频；`sid` 失效时 `ok:false` |
| 上报面板侧状态 | `GET /api/sdr_report?<任意 query>` | 原样存下（≤319 B）并镜像到 `/api/status` 的 `sdr_rep`；空 query 存为 `(empty)` |
| 板上自测音 | `GET /api/sdr_test?sid=<N>&f=<Hz>` | 生成约 3 s 单音，走一次完整 PTT（需有效会话） |

### HTTP 兜底通道

iOS/Safari 在自签证书下会拒绝 `wss://`，网页面板会自动降级为普通请求；APP 的 `PcmAudio` 在 WS 建连失败时同样降级（`transport="HTTP 兼容通道"`）：

| 端点 | 说明 |
| --- | --- |
| `GET /api/sdr_ptt?sid&tx&on&id` | 与文本帧等价的 PTT，回同一份状态 JSON |
| `POST /api/sdr_tx?sid&tx` + 原始 PCM | 单次 ≤8192 B、偶数长度；分片读取时保持采样对齐 |
| `GET /api/sdr_rx?sid` | 一次最多 5120 采样（10240 B）的 `application/octet-stream`；WS 监听存在时返回空体，取不到共享缓冲也返回空体 |

源码位置：`firmware/main/net_http.c:2610-2622`（文本帧）、`firmware/main/net_http.c:2707-2802`（兜底）、`firmware/main/sdr.c:843-854`（状态 JSON）、`firmware/main/sdr.c:791-828`（关闭）。

## 4. 时序：ON → 音频 → OFF → 尾音

| # | 动作 | 板端行为 | 关键超时 |
| --- | --- | --- | --- |
| 1 | `on=1&client=` | 借用 monitor 任务、清空收发环、`++session`、启动 5 ms 周期的发射任务 | 与 TALK 互斥：`voice_mode()==TALK` 时拒绝 |
| 2 | `/ws/sdr?sid=` | attach，登记 fd，清空接收环，开始按 tick 供数 | 旧 fd 静默 ≥6 s 才允许被顶替 |
| 3 | 心跳/上行帧 | 每帧回一帧下行音频（有数据时） | 6 s 无帧 → 视为死连接并释放 |
| 4 | `ptt1:sid:tx:id` | 校验会话与 `tx`，`voice_sdr_begin(tx)`，按模式决定网络/射频路由，清空 TX 环，`ptt_hold=true` | 首包宽限 3 s |
| 5 | 上行 PCM | 每 5 ms 取 320 采样，过 MIC EQ 与 `ntxgain`，再送网络引擎和/或本机注入 | 连续 2 s 无 PCM → 结束发话 |
| 6 | `ptt0:sid:tx:id` | 不立即掉载波：进入释放窗口，先把 TX 环里剩余 PCM 发完 | 尾音窗口 = 尾音时长+600 ms；无尾音 600 ms；绝对上限 6 s |
| 7 | 环空 | `voice_sdr_tail_start(tx)` 播板载尾音（尾音由手台产生，APP 不播） | 尾音完成（`voice_sdr_tail_done()==tx`）→ `release_locked("released")` 掉 PTT |
| 8 | 会话结束 | `on=0` 释放引擎；无客户端 30 s 时自毁 | 见“限制” |

安全性释放（不等客户端指令）：`pcm_timeout`（3 s 无首包 / 2 s 断流）走同一尾音路径；`hold_limit`（30 s）、`owner_lost`、`ws_timeout`、`ws_replaced`、`disconnected`、`force_close`、`disabled` 立即释放；`ws_dead`（30 s 无任何客户端痕迹）释放后自动关引擎。

源码位置：`firmware/main/sdr.c:349-426`（PTT）、`firmware/main/sdr.c:500-591`（每步状态机）、`firmware/main/sdr.c:769-789`（开会话）、`firmware/main/net_http.c:2452-2467`（`/api/sdr`）。

## 5. 限制

| 项 | 值 | 说明 |
| --- | --- | --- |
| 采样格式 | s16le / 16 kHz / 单声道 | 由 `AUDIO_SAMPLE_RATE_HZ`、`AUDIO_PKT_SAMPLES` 决定 |
| 块大小 | 20 ms = 320 采样 | 网络包与注入单元 |
| 上行单帧 | ≤2048 B（头 8 + PCM 2040） | 超过即断开 |
| 下行单帧 | ≤3840 B（1920 采样） | 每 tick 聚合 6 包 |
| HTTP 上行 | ≤8192 B / 次 | 更长请分片 |
| HTTP 下行 | ≤5120 采样 / 次 | WS 在用时为空 |
| 接收环 | 3 s（48000 采样） | 满了丢最旧，`sdr.drop` 计数 |
| 发送环 | 8192 采样（约 512 ms） | 满了同样丢最旧 |
| WS 活性 | 6 s | 无帧即判死；也用于判断旧 fd 能否被顶替 |
| 无客户端 | 30 s | 无 WS、无轮询、无 PTT、无控制流量 → 自毁并关引擎 |
| 单次发话 | 30 s | `SDR_PTT_HOLD_MAX_MS`，重发 ON 不延长 |
| 首包宽限 / 断流 | 3 s / 2 s | 触发尾音后释放 |
| 释放窗口 | 600 ms 或 尾音+600 ms | 绝对上限 6 s |
| 单会话 | 同时 1 个 | 第二个 `on=1` 在旧客户端 6 s 内活跃时被拒；同一会话只允许 1 个 WS fd |

并发注意：`/api/sdr_rx` 也在串行锁内（`route_needs_control()` 包含它），与另一次写操作撞车时可能收到 `409`（JSON 体）而不是音频；HTTP 兜底的客户端要能区分。

源码位置：`firmware/main/sdr.c:28-45`（常量）、`firmware/main/sdr.c:574-589`（活性与自毁）、`firmware/main/net_http.c:2508-2531`（帧上限与共享缓冲）、`firmware/main/net_http.c:2761-2802`（HTTP 下行）。

## 6. 与 APP 的对应实现

| 协议要素 | APP 实现 |
| --- | --- |
| 开会话 `on=1&client=` | `BoardSession.startListening()`：`api.json("api/sdr?on=1&client=<elapsedRealtime 取模后的 nonce>")`，读 `sid`/`tx`；必须先开收听才能发话 |
| 建 WS | `BoardApi.audioSocket(sid, listener, activity)` → `/ws/sdr?sid=..&activity=1`；HTTPS 入口自然得到 `wss://`，也可单独配置音频地址 |
| 心跳 | `PcmAudio.open()`：未录音时每 50 ms 发 1 字节帧；发送失败即上报故障 |
| 上行打包 | `AudioProtocol.microphonePacket()`：8 字节头（sid、tx，LE）+ PCM，采样数 1..1020；`PcmAudio` 读 320 采样一块 |
| 下行播放 | `PcmAudio.onMessage(bytes)`：长度必须为偶数 → 送入 `AudioTrack`（16 kHz 单声道 16-bit） |
| PTT 文本 | `PcmAudio.sendPtt()` 发 `ptt1:$sid:$tx:$id` / `ptt0:...`；`awaitPtt()` 3 s 内等 `id` 匹配的应答 |
| 应答校验 | `RadioStateRules.validatePtt()`：`sid`/`tx`/`id` 全对才认，`ok:false` 时按 `reason` 给出提示 |
| 松键确认 | `BoardSession.stopPtt()`：先试 WS 的 `ptt0`（800 ms），再以 HTTP `/api/sdr_ptt` 为准，最多 2.5 s；仍在按着才 `force=1` |
| 关会话 | `BoardSession.stopListening()`：`api/sdr?on=0&sid=`，最多重试 5 s；第一次失败后即带 `force=1`；以 `/api/status` 的 `sdr.on` 为最终依据 |
| HTTP 兜底 | WS 失败时 `PcmAudio` 切 `transport="HTTP 兼容通道"`：轮询 `/api/sdr_rx`、用 `/api/sdr_tx` 发送（按 ≤8192 B 聚合） |
| 活动快照 | `PcmAudio` 收到 `type=="activity"` 的文本帧 → `RadioActivitySample.parse()` 更新指示灯；缺实时通道时退回 `/api/activity` 轮询 |
| 控制权 | `BoardSession.ensureLease()` 已不申请租约（注释：板端不再有租约）；`api.token` 已无人写入，`X-ORL-Control` 头实际不会发出 |

源码位置：`app/app/src/main/java/net/openradiolink/app/PcmAudio.kt:64-152`（open/心跳/兜底）、`app/.../PcmAudio.kt:154-197`（PTT 与应答）、`app/.../PcmAudio.kt:200-258`（采集与打包）、`app/.../AudioProtocol.kt:6-14`（帧格式）、`app/.../BoardApi.kt:136-139,178-193`（PTT 与 WS 建连）、`app/.../BoardSession.kt:298-371`（开会话/关会话）、`app/.../BoardSession.kt:470-528`（松键确认）、`app/.../RadioStateRules.kt:32-47`（应答与原因映射）。
