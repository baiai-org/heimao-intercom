# HTTP / HTTPS 接口

手台的网页面板、配网页、APP 与刷机脚本共用一套 HTTP 接口。本文按 `firmware/main/net_http.c` 的路由表与各 handler 逐条整理，所有字段以源码为准。

## 概览

| 事项 | 事实 | 源码 |
| --- | --- | --- |
| 传输 | 同一张路由表起两个监听：明文 HTTP 与 TLS HTTPS（`httpd_start` + `httpd_ssl_start`）。TLS 启动失败只告警，明文继续服务 | `net_http.c:2923-3019` |
| 端口 | 沿用 IDF 默认值（HTTP 80、HTTPS 443）；源码注释为 "HTTPS on 443" | `net_http.c:2942`、`net_http.c:2976` |
| 证书 | 自签，`CN=orl.local`、`SAN orl.local`；不随仓库提交，构建配置期由 `tools/gen_dev_cert.py` 生成 | `net_http.c:2806-2815`、`firmware/main/CMakeLists.txt:2-6` |
| 安全上下文 | 浏览器只在安全上下文暴露 `navigator.mediaDevices`，所以网页发话（SDR）必须走 HTTPS；页面本身接受一次告警即可 | `net_http.c:2806-2813` |
| 鉴权 | **没有登录、没有 token 校验**：固件不解析 `Authorization`（全仓库无该头处理）；`X-ORL-Control` 从未参与判定（读取它的 `request_token()` 无调用者） | `app_control.c:105-117` |
| 唯一的身份门槛 | `/api/wifi/setup_session` 要求请求落在 AP 地址且 `X-ORL-Device` 等于本机 `device_id`，否则 409 | `net_http.c:520-549` |
| 写操作串行化 | 除 `/`、`/api/device`、`/api/control` 外，所有路由经 `controlled_dispatch()` → `app_control_begin()`；100 ms 内取不到锁即 409 `device mutation in progress` | `net_http.c:2882-2895`、`app_control.c:119-128` |
| 租约现状 | 本仓库固件是**单操作者实现**，没有租约仲裁：`app_control.c` 注释写明 "no control lease"；`/api/control` 恒返回 `held:false`、`mine:true`、`expires_in_ms:0`，不下发 token | `app_control.c:119-128`、`app_control.c:139-179` |
| 根路径跳转 | 仅在 TLS 已就绪且已连网时，`/` 302 到 `https://`；`/setup` 与全部 `/api/*` 不跳转（配网表单要在证书被接受前可用） | `net_http.c:808-867` |
| 配网强制门户 | 配网 AP 模式下任何未注册 URL 都 302 到 `/setup`（iOS 需要响应体才认作 captive portal），否则 404 | `net_http.c:787-801` |
| 并发 | 明文最多 9 个 socket、TLS 5 个；均开启 keep-alive 与 LRU 清理；httpd 任务栈 10 KB | `net_http.c:2949-2966`、`net_http.c:2987-3001` |
| 响应编码 | JSON 响应带 `Cache-Control: no-store`；错误体统一 `Connection: close`（避免未读完的 POST 体被当成下一个请求） | `net_http.c:247-252`、`app_control.c:51-59` |

> 读法提醒：本文的“需要控制权”＝该路由是否经过上面那把串行锁，不是租约判定。旧文档里“租约（claim/takeover）”的说法在当前源码中只剩兼容接口，见文末“兼容性”。

## 端点表

| 路径 | 方法 | 用途 | 需要控制权 |
| --- | --- | --- | --- |
| `/` | GET | 控制面板（单文件 HTML，~115 KB，分块发送）；配网 AP 且未联网时直接返回 `/setup` 表单 | 否 |
| `/setup` | GET | 配网页 | 否 |
| `/brand.js`、`/app-control.js`、`/device-name.js` | GET | 面板脚本（品牌名、控制提示、设备名） | 否 |
| `/brand-icon.png`、`/favicon.png` | GET | 图标 | 否 |
| `/api/device` | GET | 设备身份、品牌、固件版本、`api_version`、能力表、音频格式、发现信息 | 否 |
| `/api/device/name` | POST | 写自定义设备名（带版本号做乐观并发） | 是 |
| `/api/control` | GET / POST | 控制权兼容接口（`claim`/`takeover`/`renew`/`release`），实际不改变任何权限 | 否（直接注册，不进 `controlled_dispatch`） |
| `/api/wifi` | GET | 已保存的 Wi-Fi 配置列表与当前链路状态 | 否 |
| `/api/wifi` | POST | `save` / `connect` / `forget` / `setup` 操作 | 是 |
| `/api/wifi/setup_session` | POST | 配网会话续期/释放（`renew`/`release`），仅限 AP 内且带本机 `X-ORL-Device` | 否（自带前置校验） |
| `/api/scan` | GET | 扫描结果；`force=1` 触发真实扫描 | `force=1` 时是 |
| `/api/provision` | POST | 一次性写入 SSID/密码（可带 host）并安排连接 | 是 |
| `/api/provision` | GET | 405，明确要求用 POST（凭据不许进 URL） | — |
| `/api/done` | GET | 由操作者结束配网，关闭 setup AP | 是 |
| `/api/status` | GET | 全量状态快照（约 4.6 KB 缓冲，字段最多） | 否 |
| `/api/live` | GET | 轻量实时量（表头、PTT、静噪灯），面板 1 Hz 轮询用 | 否 |
| `/api/radio` | GET | SA818 频段/型号/上下限/就绪状态/功率与 HL 引脚 | 否 |
| `/api/activity` | GET | 射频与网络收发活动快照（`type:"activity"`） | 否 |
| `/api/channels` | GET | 全部已存信道（最多 99 条） | 否 |
| `/api/channel` | GET | `save` / `load` / `edit` / `del` 单个信道（query 参数） | 是 |
| `/api/ctcss`、`/api/dcs` | GET | 模拟/数字亚音码表 | 否 |
| `/api/set` | GET / POST | 单键改参数（`k`/`v`），见下文键表 | 是 |
| `/api/talk` | GET | 网页发话：`confirm=1` 起发（`secs` 5..60），`on=0&tx=` 停止 | 是 |
| `/api/tail_upload` | POST | 上传尾音 WAV/MP3（原始字节，非 multipart），≤512 KB | 是 |
| `/api/ota` | POST | 上传应用固件（原始 `.bin`），成功即重启 | 是 |
| `/api/firmware.bin` | GET | 下载当前运行分区镜像（先 `esp_image_verify`） | 否 |
| `/api/coredump` | GET | 下载 coredump 分区中的 ELF（无崩溃则报 `no dump`） | 否 |
| `/api/sdr` | GET | 开/关网页收听会话：`on=1&client=<nonce>` 返回 `sid`；`on=0&sid=&force=` | 是 |
| `/api/sdr_state` | GET | 会话/PTT 状态快照（含 `activity` 可选） | 是 |
| `/api/sdr_report` | GET | 面板上报自身链路状态（原样存下并镜像到 `/api/status` 的 `sdr_rep`） | 是 |
| `/api/sdr_ptt` | GET | HTTP 兜底的 PTT：`sid&tx&on&id` | 是 |
| `/api/sdr_tx` | POST | HTTP 兜底的麦克风 PCM（≤8192 B，偶数长度） | 是 |
| `/api/sdr_rx` | GET | HTTP 兜底的接收 PCM（≤5120 采样）；已有 WS 监听时返回空 | 是 |
| `/api/sdr_test` | GET | 板上生成约 3 s 单音（需有效 `sid`） | 是 |
| `/ws/sdr` | GET（Upgrade） | 音频 WebSocket，协议见 [audio-ws.md](audio-ws.md) | 是 |
| `/api/adc` | GET | ADC 采集诊断（时钟、分频、溢出、写入耗时） | 否 |
| `/api/speaker_diag` | GET | 扬声器/编解码器诊断快照 | 否 |
| `/api/speaker_diag` | POST | 触发诊断动作：query 里 `op=` 与 `ms=250..5000` | 是 |
| `/api/speaker_hw` | GET | 读 ES8311 寄存器快照（要求 PTT 空闲、无播放） | 否 |
| `/api/speaker_pcm` | GET | 把最近一次采集导出成 WAV（`path=common\|local`） | 否 |
| `/debug/speaker` | GET | 扬声器调试页 | 否 |

`/api/device/name` 还依赖 `net_wifi_name_changed()` 刷新 mDNS 名；`/api/ota`、`/api/tail_upload` 会先拒绝非空闲状态（详见各节）。

源码位置：`firmware/main/net_http.c:2817-2866`（路由表）、`firmware/main/net_http.c:2870-2895`（控制权判定与分发）、`firmware/main/app_control.c:139-179`（`/api/control`）。

## 常用请求/响应字段

### `/api/device`

GET 无参数，返回设备身份与能力，是客户端连接后的第一个请求。

| 字段 | 含义 |
| --- | --- |
| `project`、`device_name`、`panel_name`、`app_name` | 品牌串（编译期常量） |
| `device_id` | 设备身份（≤64 字符，客户端用它核对“还是同一台”） |
| `custom_name`、`name_revision` | 用户自定义名与其版本号 |
| `model` | 固定 `ESP32-S3-N16R8` |
| `firmware` | 应用版本（`esp_app_get_description()->version`） |
| `api_version` | 当前为 `2` |
| `capabilities` | 见“兼容性” |
| `audio` | `{"format":"pcm_s16le","rate":16000,"channels":1,"websocket":"/ws/sdr"}` |
| `discovery` | `{"mdns":"_openradiolink._tcp","gateway_udp":54322}` |

### `/api/status`

一次性返回整机状态。缓冲区为 4608 B（PSRAM，按请求分配），字段极多，按组读：

| 组 | 关键键 |
| --- | --- |
| 身份 | `device_id`、`custom_name`、`name_revision` |
| 构建 | `build.version/date/time/sha256` |
| 复位 | `rst`（`poweron`/`sw`/`panic`/`brownout`/`task_wdt`…） |
| Wi-Fi | `wifi.connected/ip/ip6/ip6all/ssid/state/rssi/disc/disc_reason/disc_age_s/ap/ap_ip/ap_ssid/host` |
| 模式 | `mode`（`off` / `MONITOR (radio -> speaker)` / `TALK (...)`）、`ptt`（`IDLE`/`PREPARE`/`TX`/`TAIL`/`FAULT`/`TX(BRIDGE)`） |
| 电平 | `rssi`、`peak`、`sql_open`、`mod_sq` |
| 音频 | `eq_spk[6]`、`eq_mic[6]`、`clip`、`ovf`、`underruns`、`gain`、`txgain`、`ntxgain`、`bgtx`、`cvol`、`spk`、`micgain`、`mic_db`、`adcvol`、`sqgate`、`rxnetgain`、`txpre`、`audio_rate` |
| 资源 | `tx_count`、`uptime`、`heap`、`heapInt`、`heapIntMax`、`psram` |
| 电台 | `freq.tx/rx/bw/sq/vol/ctcss_tx/ctcss_rx/power/flt[3]` |
| 网络对讲 | `intercom.mode/room/state/tx/rx/peers/qdrop/broker/diag/user/pass` |
| 网络 PTT 诊断 | `ptt_net.*`（结束请求/送达/失败、时延、对端 PTT 与载波） |
| 模拟桥接 | `bridge.en/chan/chan_name/active/keyed/rx_open/rx_start*` |
| 尾音 | `tail.id`、`tail.custom_s`、`tail.modes` |
| 前面板 | `fp.led_rx/led_tx/ptt/chan/keys[4]/act[4]/led_policy/rx_source/tx_source/rf_rx/rf_tx/net_rx/net_tx/cos_*` |
| SDR | `sdr.*`（`on`/`ws`/`sid`/`txid`/`ptt`/`owner`/`reason`/`rel`/`tail`/计数器，见 [audio-ws.md](audio-ws.md)） |
| 日志 | `log`（最近日志尾部）、`logx`（只保留连接/错误相关行） |
| 面板上报 | `sdr_rep`（`/api/sdr_report` 原样回显） |
| 公网直连 | `netp2p.*`（由 `netp2pnet_status_json()` 追加） |

**注意**：这些附加段是“能放下才放”。缓冲不足时 `fp`、`sdr`、`log`、`logx`、`sdr_rep`、`netp2p` 会整段缺失，客户端必须容忍缺键，不能以键是否存在推断能力。

### `/api/radio`

| 字段 | 含义 |
| --- | --- |
| `band`、`model` | `UHF`/`VHF`/`UNSET`，型号串 `SA818S-U`/`SA818S-V` |
| `min`、`max` | 该频段上下限（MHz） |
| `ready` | 发射参数是否已下发给模块（`sa818_cfg_tx_ready()`） |
| `compatible`、`reason` | 当前配置是否可下发；不可用时给出原因（未确认时提示先提交频率） |
| `power_applied`、`power_pending`、`power_control`、`hl_open_drain/hl_pull_up/hl_pull_down/hl_output` | 高/低功率引脚状态与电气配置（`open_drain`） |

### `/api/control`

请求：`POST` + JSON，必须含字符串 `op`，取值 `claim`/`takeover`/`renew`/`release`；其他键（如 `client`、`label`、`device_id`、`hold`）会被解析 JSON 但**不参与判定**。

响应（GET 与 POST 同形）：

| 字段 | 当前取值/含义 |
| --- | --- |
| `ok` | 请求是否被理解（`op` 缺失或未知时为 `false` + 409） |
| `err` | `op required` / `unknown control operation` |
| `held` | 恒 `false`——没有租约 |
| `mine` | 恒 `true`——任何客户端都可以直接操作 |
| `device_id` | 本机身份 |
| `manual` | 恒 `false` |
| `generation` | 租约代号（不会因 claim 递增） |
| `expires_in_ms` | 恒 `0` |

### `/api/set`

三种写法等价，`k` < 24 字节、`v` < 96 字节：

- `GET /api/set?k=<key>&v=<value>`（value 会做 URL 解码）
- `POST` + JSON `{"k":"...","v":"..."}`（`Content-Type: application/json`）
- `POST` + 表单体 `k=<key>&v=<value>`

应答：成功 `{"ok":true}`；失败 `{"ok":false,"err":"..."}`。未知键返回 `unknown key`。

常用键（完整分支见 `h_set`）：

| 分类 | 键 |
| --- | --- |
| 发射 | `freq_tx`、`freq_rx`、`freq_pair`（`tx,rx` 或 `tx/rx`）、`bw`（0/1）、`sq`（0..8）、`vol`（1..8）、`ctcss_tx`、`ctcss_rx`、`power`（0/1）、`rf_band`（`UHF`/`VHF`） |
| 音频 | `gain`、`txgain`、`ntxgain`、`cvol`、`spk_vol`（0..7）、`micgain`（0..7）、`adcvol`、`sqgate`、`rxnetgain`（0..255）、`txpre`、`bgtx`、`safilt`、`dacmute`（0/1/2） |
| EQ | `eq_reset`、`eq_spk_<band>`、`eq_mic_<band>`，band ∈ `bass/warm/body/clear/pres/air`，取值 −12..+6 dB |
| 尾音 | `tail`（0..8，8=自定义）、`tail_modes`（1..7）、`tail_preview`（0..8）、`tail_custom_del` |
| 对讲 | `ix_mode`（0 本地 / 1 公网直连 / 2 中继）、`ix_room`（1000..9999）、`ix_broker`、`ix_user`、`ix_pass`、`ix_rf` |
| 公网直连 | `np2p_peer_add`（`name,ip:port`）、`np2p_peer_del`、`np2p_knock`、`np2p_hub`、`np2p_stun` |
| 桥接 | `bridge_en`、`bridge_chan`、`bridge_test` |
| 其它 | `monitor`、`auto`、`sqgpio`、`fp_key0..3`、`opussweep`、`opusbench`、`wifi_radio_off`、`factory_reset`、`factory_wipe` |

两个危险键的语义差别（两者都不要求二次确认，只受串行锁约束）：

- `factory_reset`：恢复运行参数默认值，**保留** Wi-Fi 凭据与已存信道；
- `factory_wipe`：延迟 700 ms 擦除整个 NVS 并重启，等于长按 SW1 10 s。

### `/api/wifi`

GET 返回（缓冲 8192 B，要求 ≥7800 B）：

| 字段 | 含义 |
| --- | --- |
| `state`、`ssid`、`host`、`connected` | 当前链路 |
| `active_id`、`pending_id` | 正在使用/正在切换的配置 id |
| `ap`、`setup_hold_ms` | 配网 AP 是否开启、会话剩余保持时间 |
| `offline_ms`、`scan_running`、`scan_age_ms`、`storage_error`、`limit` | 诊断 |
| `profiles[]` | `{id,ssid,has_password,auto_join,priority,rssi,failures,retry_in_ms}`（不含明文密码） |

POST 的 `op`：

| op | 必填 | 说明 |
| --- | --- | --- |
| `save` | `auto_join`（bool）、`priority`（−100..100）；`ssid`/`password`/`host` 可省略表示不改 | 无 `id` 时新建或按 SSID 合并 |
| `connect` | `id` | 先校验再应答，应答发出后延迟 ≥1500 ms 切换 |
| `forget` | `id` | 删除配置；若删的是当前活动项会请求断开 |
| `setup` | — | 开配网 AP（3 分钟后自动收） |

应答 `{"ok":true,"id":<id>,"scheduled":<bool>}`。

配套端点：

- `POST /api/provision`：JSON `{ssid, password, host?}`，保存为 `auto_join=true`、`priority=0`，回 `{"ok":true,"saved":true,"connected":false,"scheduled":true,"id":N}`；应答发出后才安排 1500 ms 后连接，给手机留出离开 AP 的时间。GET 返回 405。
- `GET /api/done`：AP 未开时直接 `ok:true`；未联网时 `ok:false,"not connected yet"`；否则关闭 setup AP。
- `POST /api/wifi/setup_session`：JSON `{op:"renew"|"release", session:"<owner>"}`，要求 AP 开启、请求落在 AP 地址、`X-ORL-Device` 等于本机；成功回 `{"ok":true,"timeout_ms":60000}`。owner 只允许字母数字与 `-`/`_`，≤64 字符。

### `/api/ota`

`POST` 原始应用镜像（`application/octet-stream`，非 JSON）：

| 约束 | 值 |
| --- | --- |
| 最小长度 | 0x10000（64 KB） |
| 最大长度 | 目标 OTA 分区大小 |
| 互斥 | `voice_mode()==TALK`、`ptt` 非 IDLE、`ptt_owner()!=NONE` 任一成立即拒绝 |
| 写前动作 | 网络引擎强制回本地模式（不恢复）、关闭 SDR 引擎 |
| 超时 | 15 s 无进展 / 总计 180 s → `upload timed out` 并 `esp_ota_abort` |
| 成功路径 | `esp_ota_end` 校验 → `set_boot_partition` → 先回 `{"ok":true}` → 延迟 1200 ms 重启 |

失败文案包括 `image too small`、`no OTA partition`、`image larger than the OTA slot`、`image rejected (wrong file?)`、`flash write failed`、`network tasks did not stop`、`SDR tasks did not stop`。

### `/api/sdr` 与 `/api/sdr_state`

`/api/sdr`：

| 请求 | 说明 |
| --- | --- |
| `on=1&client=<uint32>` | 开启（或接管）收听会话，返回 `sid`；`client` 必填，`0` 无效 |
| `on=0&sid=<uint32>&force=0\|1` | 关闭；按着 PTT 时普通关闭被拒，`force=1` 可中断发射 |

`/api/sdr_state?sid=` 只读查询，返回同一份状态 JSON（不消耗音频）。

两者共用 `sdr_state_json()` 的字段：

| 字段 | 含义 |
| --- | --- |
| `ok` | 该请求是否被接受 |
| `sid` | 当前会话号（每次开启递增） |
| `tx` | 板端已接受的发射号 |
| `id` | 请求里回显的关联号（HTTP `id=`，默认 0） |
| `ptt` | 是否仍持有发射（`s_ptt_hold && !releasing`） |
| `owner` | `none`/`talk`/`network`/`browser` |
| `reason` | 结束原因：`idle`/`transmitting`/`tail_pending`/`tail`/`released`/`pcm_timeout`/`hold_limit`/`owner_lost`/`ws_timeout`/`ws_replaced`/`disconnected`/`force_close`/`disabled`/`busy`/`network_not_ready`/`radio_not_ready`/`cancelled` |
| `on`、`rel`、`tail` | 引擎开启 / 正在释放 / 尾音已开始 |
| `activity` | 仅当请求带 `activity=1` 时追加（见 `/api/activity`） |

### `/api/activity`

无参数，返回 ≤512 B 的单一快照：

`{"type":"activity","device_id":...,"boot_id":"<8 hex>","sample_us":N,"rf_rx":b,"rf_tx":b,"net_rx":b,"net_tx":b,"tx_active":b,"rx_active":b,"ic_mode":0|1|2,"net_ready":b}`

`boot_id` 每次上电随机生成，`sample_us` 是采样时刻，用来判定快照新旧（同一次通话中迟到的 HTTP 应答不能覆盖更新的 WS 快照）。

## 错误与状态码

| 状态码 | 出现场景 | 响应体 |
| --- | --- | --- |
| 200 | 绝大多数请求，**包括业务失败** | `{"ok":false,"err":"..."}`，客户端必须同时看 `ok` |
| 302 | `/` 跳 HTTPS；配网 AP 下未知路径跳 `/setup` | 空体或提示文本 |
| 400 | JSON 体非法/长度不在 2..2048/重复键/含 `\u0000`；`custom_name` 参数非法；`/api/sdr_tx` PCM 长度为奇数或超限；`setup_session` 参数非法 | `{"ok":false,"err":...}` 或 httpd 默认错误页 |
| 405 | `GET /api/provision` | 提示改用 POST JSON |
| 409 | 串行锁忙（`device mutation in progress`）；`/api/device/name` 版本冲突；`/api/wifi/setup_session` 前置条件不满足；`/api/control` 的 `op` 缺失或未知 | `{"ok":false,"err":...}` |
| 415 | POST JSON 端点但 `Content-Type` 不是 `application/json` | `application/json required` |
| 500 | 运行分区读取/镜像校验失败、状态缓冲分配失败 | httpd 错误页 |
| 503 | `no memory`（JSON 体或应答缓冲分配失败） | `{"ok":false,"err":"no memory"}` |
| 404 | 未注册路径且不在配网 AP 模式 | `not found` |

JSON 请求体的通用校验（凡是用 `app_read_json()` 的端点）：

1. `Content-Type` 必须是 `application/json`（允许 `;charset=...`），否则 415；
2. `Content-Length` 必须在 2..2048 字节，否则 400；
3. 顶层必须是对象；重复键直接判非法；解码后含 NUL（含 `\u0000`）判非法；
4. 错误应答统一带 `Cache-Control: no-store` 与 `Connection: close`。

## 兼容性（api_version）

- `GET /api/device` 的 `api_version` 当前为 **2**。APP 在连接第一步就断言 `api_version == 2`，不等则提示“手台需要升级到配套 Wi-Fi App 固件”并停止（`app/.../BoardSession.kt:158-170`）。
- 能力位（同一响应）：

| 键 | 值 | 说明 |
| --- | --- | --- |
| `saved_wifi` | 1 | 支持多配置保存 |
| `recovery_ms` | 60000 | 配网会话保持窗口 |
| `control_lease` | true | **兼容值**：接口存在，但当前固件不强制租约 |
| `channels` | 99 | 信道上限 |
| `ota`、`files`、`audio` | 1 | OTA / 文件上传 / 音频 |
| `json_settings` | 1 | `/api/set` 接受 JSON |
| `audio_clients` | 1 | 同时只允许一个音频会话 |
| `radio_activity` | 1 | `/api/activity` 与 WS `activity=1` |
| `setup_session` | 1 | `/api/wifi/setup_session` |
| `custom_name` | 1 | `/api/device/name` |

- 字段增删策略是“追加 + 满了就丢整段”（`/api/status`），没有版本号随字段变化递增。客户端应：用 `capabilities` 判能力，用 `opt*` 读可选键，容忍整段缺失。
- `/api/control` 的四个 `op` 属于**兼容层**：源码明确写着旧客户端会发 `claim`/`takeover`/`renew`/`release`，固件只校验 `op` 是否合法，不做仲裁；`mine` 恒 `true`。APP 侧对应的 `BoardSession.claim()` 仍保留但已无调用者，写操作前只做 `ensureLease()`（空检查）。若要恢复真正的多客户端互斥，必须改固件，不能只改客户端。
- `Authorization` 头在固件中不存在；APP 的 `bearer`/`username` 字段（`BoardApi.authorization()`）只对自建反向代理有意义，对直连手台无影响。
- 两份传输（80/443）跑同一张路由表与同一份状态；差别只在 `/` 是否跳转与 TLS 开销。WEB 面板的 SDR 需 HTTPS，其它端点用 HTTP 即可。

源码位置：`firmware/main/net_http.c:494-509`（`/api/device` 与 `api_version`）、`firmware/main/app_control.c:139-179`（兼容层）、`app/app/src/main/java/net/openradiolink/app/BoardSession.kt:158-170`、`app/app/src/main/java/net/openradiolink/app/BoardSession.kt:247-255`（`ensureLease`）。
