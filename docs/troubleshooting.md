# 故障排查

按"现象 → 先看哪里 → 原因 → 处置"的顺序写。所有结论都对应到源码；
查不到的行为写"以源码为准（路径）"，不猜。

历史问题一律按 **已修复 / 触发条件 / 正确行为** 记录，不代表当前固件仍有该故障。

---

## 0 先做三件事

| 动作 | 怎么看 | 说明 |
|---|---|---|
| 看串口日志 | `tools/monitor.py` 或任意串口工具，115200 | 上电横幅里有芯片、Flash、PSRAM、MAC、复位原因 |
| 看 `/api/status` | 浏览器打开 `http://<ip>/api/status` | 一份 JSON 里有 WiFi、PTT、音频、内存、网络、桥接、前面板全部状态 |
| 看复位原因 | 状态里的 `rst` 字段 / 串口横幅 | `panic`、`task_wdt`、`int_wdt`、`brownout`、`sw`、`poweron`、`usb`、`cpu_lockup` 指向完全不同的方向 |

`/api/status` 里几个最常用的字段：

| 字段 | 含义 |
|---|---|
| `ptt` | PTT 状态机：`IDLE / PREPARE / TX / TAIL / FAULT` |
| `mode` | 语音路径：`off`、`MONITOR (radio -> speaker)`、`TALK (microphone -> ...)` |
| `sdr.on` / `sdr.ws` / `sdr.ptt` / `sdr.owner` / `sdr.reason` | 浏览器/APP 音频会话 |
| `sdr.rel` / `sdr.tail` | 正在释放 / 正在播尾音 |
| `sdr.rel_notail` / `sdr.rel_tail_timeout` / `sdr.off_reject` / `sdr.on_reject` / `sdr.begin_busy` | 松键与连按的诊断计数 |
| `heap` / `heapInt` / `heapIntMax` / `psram` | 总空闲 / 内部 RAM / 内部最大连续块 / PSRAM |
| `intercom.mode` / `intercom.state` | 网络模式（0/1/2）与引擎状态 |
| `bridge.active` / `bridge.keyed` / `bridge.rx_open` | 模数桥状态 |
| `tail.id` / `tail.modes` | 当前尾音与生效模式掩码 |

"能不能真发出去"不在 `/api/status` 里，要看 `/api/activity` 的 `net_ready`
（同一份快照还有 `rf_rx/rf_tx/net_rx/net_tx/tx_active/rx_active/ic_mode`）。

源码位置：`firmware/main/net_http.c`（`h_status`）、`firmware/main/sdr.c`（`sdr_status_json`）、
`firmware/main/main.c`（`board_print_banner`、`status_task`）。

---

## 1 刷机与串口

### 1.1 快速对照

| 现象 | 多半是 | 处置 |
|---|---|---|
| 列表里没有 COM 口 | 充电线 / 驱动 / 板子没进下载模式 | 换数据线；按住 BOOT 点 RST，松开 BOOT 后刷新 |
| 提示"这是 OTA 应用 BIN" | 选错文件 | 换完整合并 BIN（文件名通常带 `full`） |
| 提示"固件分区布局与此手台不匹配" | 固件与板子不是同一布局 | 停止操作，核对固件来源；确需重装时用全新刷机 |
| 提示"合并固件包含额外配置数据" | 拿整片 Flash 备份当固件 | 用发布包里的完整合并 BIN |
| 刷到一半停止/拔线 | 擦写已开始 | 重新刷机，不能只重试写一半 |
| 刷完起不来 | bootloader/分区表写坏 | 进下载模式重新刷（保留配置模式即可） |

### 1.2 COM1–COM9 是 Windows 保留设备名

**已修复**：某些 Windows 10 机器上，刷机工具为每个串口建的工作目录用裸 `COM3`
这类名字，目录能被创建，但随后在该目录里打开/写文件会失败，
报 `WinError 267`（目录名无效），表现为"刷机刚开始就失败"。

- 触发条件：Windows 把 `COM1`–`COM9` 当保留设备名；裸名恰好是保留名时才会踩到。
- 根因：`path\COM3` 在部分 Windows 版本上被接受，但内部按设备名解析。
- 正确行为：每个串口的工作目录加上前缀，变成 `port-COM3`，是普通文件路径；
  刷机日志与事件文件都在里面。
- 源码位置：`flasher/runner.py`（`Batch.port_directory`）。

自己写脚本时同理：**不要**用裸 `COM1`–`COM9` 当目录名，加任何前缀即可。

### 1.3 OTA 只能传应用 BIN

| 上传目标 | 接受什么 | 拒绝什么 |
|---|---|---|
| 面板/APP 的 OTA（`POST /api/ota`） | 仅应用镜像（app BIN） | 小于 64 KB；大于 4 MB 的 OTA 槽；通话中/PTT 非 IDLE |
| 刷机工具（USB） | 完整合并 BIN（full） | OTA 应用 BIN（`0x20` 处是应用描述魔数）、错误芯片、错误布局、截断、含配置数据的备份 |

两边是**互斥**的：full BIN 里有 bootloader，不能塞进应用槽；app BIN 没有
bootloader 和分区表，不能从 `0x0` 刷。

- 从面板 `/api/firmware.bin` 下载到的 `heimao-firmware.bin` 是**应用镜像**，
  只能拿去做 OTA，不能当 full BIN。
- OTA 收到错误文件时是 `esp_ota_end()` 校验失败后 abort，**正在运行的镜像不受影响**；
  最坏情况是"这次升级被拒绝"，不是变砖。

源码位置：`firmware/main/net_http.c`（`h_ota`、`h_firmware_bin`）、
`flasher/firmware.py`（`Firmware.parse`）、`flasher/README.md`。

### 1.4 保留配置 vs 全新刷机

| | 保留配置（默认） | 全新刷机 |
|---|---|---|
| 写入区段 | `0x0` bootloader、`0x8000` 分区表、`0xF000` otadata、`0x20000` 应用 | 先 `erase_flash`，再写全部区段 |
| NVS（Wi-Fi/信道/参数） | 保留 | 清空 |
| `storage`（自定义尾音） | 保留 | 清空 |
| 另一个 OTA 槽 | 保留 | 清空 |
| coredump | 保留 | 清空 |
| 前置检查 | 先读回目标板分区表并逐项比对 | 先核对芯片型号与 16 MB 容量 |

保留模式的关键点：**不写 `0xFF` 填充区**。把 full BIN 从 `0x0` 连续写入，
会把 bootloader 与分区表之间、分区表与 NVS 之间的填充区一并写进去，
等于清掉 NVS/PHY——所以"没执行 erase_flash"不等于"保住了配置"。
保留模式同时把 otadata 重写为全 `0xFF`，让启动选择回到 `ota_0`。

源码位置：`flasher/firmware.py`（`flash_steps`、`Firmware.parse` 的填充区检查）、
`docs/flashing.md`。

### 1.5 断电与回退

| 中断时机 | 后果 | 恢复 |
|---|---|---|
| OTA 上传中断 | 非活动槽写了一半，启动分区没变 | 重新 OTA；旧镜像照常启动 |
| OTA 写完但重启前断电 | otadata 已指向新槽，新镜像完整 | 正常启动新镜像 |
| USB 刷机在擦除/写入中断 | bootloader 或分区表可能不完整，**无法启动** | 进下载模式（BOOT+RST）重新刷；保留配置模式会顺带把启动选择重置到 `ota_0` |
| 刷完新固件不启动 | 新镜像通过校验但运行即崩 | 串口重刷回上一版（无板端自动回滚） |

**没有自动回滚**：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 未启用，
所以"新镜像能通过 `esp_ota_end()`、但一跑就重启"这种情况板子不会自己退回旧版。
唯一的回退路径是串口重刷；保留配置模式只重置 OTA 选择，不动 NVS，
因此频点、信道、Wi-Fi、尾音都还在。

源码位置：`firmware/sdkconfig`（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set`）、
`firmware/main/net_http.c`（`h_ota`）、`firmware/partitions.csv`。

### 1.6 刷机工具的运行方式

- 单文件 EXE，`--onefile` 且 `--runtime-tmpdir .`：运行时把依赖解到**当前目录**，
  退出后清理；所以请放在可写目录里运行，不要放在只读位置或系统保护目录。
- 只在你点"开始"之后才打开所选 COM，插入板子不会自动刷机；
- 多端口是真正并行（每口一个子进程），一个口失败不影响其他口；
- 公开构建默认**不内嵌固件**（工具是 GPL-3.0，固件单独授权），
  需要自行选择 full BIN；内嵌版本要用 `-WithFirmware` / `-FirmwareBin` 显式构建。

源码位置：`flasher/build.ps1`、`flasher/app.py`（`home_directory`）、`flasher/runner.py`。

---

## 2 网络与访问

### 2.1 自签 HTTPS 证书告警

手台的 HTTPS 用的是**构建期生成的自签证书**（ECDSA P-256，默认 CN/SAN 为
`orl.local`，有效期 10 年）。因此：

| 现象 | 原因 | 处置 |
|---|---|---|
| 浏览器整页红色警告 | 自签证书不被系统信任 | 手动确认继续；或在系统/浏览器里信任该证书 |
| 警告"名称不匹配" | 证书 SAN 里只有 DNS 名，没有 IP | 用主机名访问（`https://<hostname>.local/`）；直接访问 IP 必然名称不匹配 |
| 换了固件后 APP 报证书变化 | 证书随构建生成、不进仓库，重建一次就可能换一张 | APP 会要求重新确认指纹，不会静默接受 |
| iOS 上麦克风按钮不可用 | 非安全上下文不给 `navigator.mediaDevices` | 必须走 HTTPS（或在 localhost 场景下用受信证书） |

要点：**面板可以走 HTTP，语音必须走 HTTPS**。HTTP 页面上"按住说话"能点，
但浏览器不会交出麦克风。

APP 侧是 TOFU 指纹绑定：首次连接记住服务端证书的 SHA-256，之后指纹不一致直接抛
`CertificateFailure` 并交给用户确认，而不是退回系统信任链。

源码位置：`tools/gen_dev_cert.py`、`firmware/main/CMakeLists.txt`、
`firmware/main/net_http.c`（`httpd_ssl_start`）、
`app/app/src/main/java/net/openradiolink/app/BoardApi.kt`（`createClient`）。

### 2.2 mDNS / 主机名

| 项 | 行为 |
|---|---|
| 服务类型 | `_openradiolink._tcp`（附带 `device_id`、`api=2`、`https=443` 等 TXT），另有 `_http._tcp` |
| 主机名 | 存在 NVS，默认 `heimao`；从很旧的固件升级上来的板子保留 `orl` |
| 冲突 | 同一网段多台手台必须各用各的主机名，否则 `.local` 指向不明 |
| 改名 | 通过 `/api/wifi` 写主机名，立即 `mdns_hostname_set`，不用重启 |

排查顺序：

1. `ping <hostname>.local` 通不通；不通先用 IP；
2. Android 12+ 需要"附近设备/局域网"权限，否则 mDNS 发现为空（APP 会提示
   "请先授予局域网或附近 Wi-Fi 权限"）；
3. 路由器开了"AP 隔离/客户端隔离"时，手机与手台互相不可见；
4. mDNS 走多播，部分企业 Wi-Fi 会过滤多播——此时用手动输入 IP。

APP 还有第二条发现通道：手台每 10 s 向**网关地址**的 UDP `54322` 发一条 JSON 通告。
它只到网关，所以手机做热点时最有效（APP 里显示为"手机热点通告"）；
手台接在路由器后面时，网关是路由器而不是手机，这条通道通常收不到——
用 mDNS 或手动地址。

源码位置：`firmware/main/net_wifi.c`（`start_mdns`、`announce_gateway`）、
`app/app/src/main/java/net/openradiolink/app/DeviceDiscovery.kt`。

### 2.3 AP 配网

- 触发：**没有可用 Wi-Fi 凭据**时自动开配网 AP。SSID 是
  `黑猫智能手台-` + MAC 后 3 字节，地址是固件固定的 AP 网关（`net_wifi_ap_ip()`），
  由 `dns_server` 组件做跳转；
- 这个热点是**开放**的（无密码）——配完网尽快离开配网状态，别长期停在 AP 模式；
- AP 与已关联的 STA 不会共存，所以配网时手台上不了网是正常的；
- 配网会话是"持有式"的：`/api/wifi/setup_session` 每 15 s 续一次，
  60 s 不续就释放，避免一个关掉的页面把手台一直按在配网态；
- 页面上的操作序列是扫描（`/api/scan`）→ 提交（`/api/provision`）→ 完成（`/api/done`）。

拿不回网络时不用串口，用 SW1（BOOT）手势：

| 按住时长 | 动作 |
|---|---|
| 3 s 后松开 | 打开配网 AP，**已保存的网络保留**（灯会边按边闪，表示计数生效） |
| 6 s | 只提示"继续按住将全部擦除"，不执行 |
| 10 s（不用松手） | `nvs_flash_erase()` 后立即重启：Wi-Fi、信道、参数、尾音全部清空 |

注意 SW1 是 GPIO0，复位时按住会进 ROM 下载模式，所以这个手势只能在固件
已经跑起来之后用。

源码位置：`firmware/main/net_wifi.c`（`net_wifi_setup_ap_start`、
`net_wifi_setup_session`）、`firmware/main/main.c`（`heartbeat_task`）、
`firmware/main/net_http.c`（`h_provision`、`h_scan`、`h_done`）。

### 2.4 常见网络现象

| 现象 | 检查 | 说明 |
|---|---|---|
| 面板打得开，语音不出声 | 是否 HTTPS；`sdr.on` 是否为 true | HTTP 下浏览器不给麦克风；`/ws/sdr` 同时只允许一个监听者 |
| 面板打开很慢 | 是否 HTTPS、连接是否复用 | 一次 TLS 握手约 470 ms，已建连接上的请求约 22 ms；keep-alive 已默认开启 |
| 面板"卡住不响应" | `sdr` / socket 占用 | 服务端开了 LRU 回收，正常情况下不会因 socket 用尽而挂死 |
| 只能一台手机连 | 设计如此 | 单操作者设备，没有控制租约；第二个入口会互相覆盖，抢写时返回 `409` |

`409` 现在的含义是"设备正在处理另一个写操作"（锁 100 ms 超时），
**不是**"别人拿着控制权"。APP 里"控制权由…持有"的提示是旧租约时代的遗留分支，
当前固件不会因此返回 409/423。

源码位置：`firmware/main/net_http.c`（`net_http_start` 的 keep-alive 与 socket 上限）、
`firmware/main/app_control.c`（`app_control_begin`）。

---

## 3 音频与 PTT

### 3.1 松手后还在发射（尾音）

这是**正常行为**：松键不等于立刻断载波。`voice_request_talk_end()` 只置标志，
`talk` 任务跳出循环后仍然保持键控，把配置的尾音发完，再 `ptt_release()`。

| 观察 | 判断 |
|---|---|
| 松手后 0.1–0.3 s 结束 | 短尾音（单音/双响） |
| 松手后 0.2–3 s 结束 | 自定义尾音或 K-CHOO/MDC 这类长音，正常 |
| 超过 6 s 还在发 | 异常：`/api/sdr_state` 看 `rel` / `tail` / `owner` |

上限：SDR 释放路径的硬上限是 6 s（`SDR_RELEASE_MAX_US`），
`talk` 路径的续期受 `PTT_TX_ABSOLUTE_MAX_MS`（180 s）约束。
尾音在**本地喇叭上不播放**（设计如此：发射时功放保持关闭，避免自己的声音被麦克风再收一遍），
所以"自己听不到尾音、对端听得到"是正确的。

源码位置：`firmware/main/voice.c`（`talk_task` 尾部）、`voice.h`、
`sdr.c`（`SDR_RELEASE_MAX_US`）。

### 3.2 短按没有尾音

**已修复**：很短的一次发射（0.1–0.3 s）松手后没有尾音。

- 触发条件：按下时间很短，且取第一个尾音数据块时发生阻塞（Flash 访问、I2S 忙、
  别的任务占着 CPU）。
- 根因：`talk` 路径只按每个音频块续期约 1 s，取第一块尾音可能超过这个窗口，
  窗口一过 PTT 就被松开，尾音写到已经不再发射的 DAC 上，等于没发。
- 正确行为：播尾音前按**整段尾音时长 + 1.5 s** 一次性授权
  （`ptt_extend(total_ms + 1500)`，仍受 180 s 绝对上限约束），
  取数据要多久都不影响尾音发完。
- SDR 路径的同类问题也修了：释放上限从固定的 1.2 s 提到 6 s，
  较长的自定义尾音不会再被截断成 `tail_timeout`。

如果现在仍然没尾音，按顺序查：

1. `tail_get()` 是否为 `TAIL_OFF`（面板里选的是"关"）；
2. 尾音是否为空——自定义尾音被删掉后日志会出现
   `tail '...' selected but empty -- nothing sent`；
3. 模式掩码是否包含当前模式（`tail_modes_get()`，默认三种全开）；
4. 日志是否出现 `tail '...' skipped: PTT is already ...`——
   说明 PTT 在播尾音前就已经不是 TX（超时、FAULT、外部强制停止）。

源码位置：`firmware/main/voice.c`、`voice_tail.c`、`voice_tail.h`、`sdr.c`。

### 3.3 快速连按

**已修复**：快速连按时载波被锁在发射状态，直到 30 s 按住上限才松开。

- 触发条件：上一次松手还没走完（尾音/释放中）就再次按下，ON 和 OFF 带着不同的 `tx`。
- 根因（板端）：ON 与 OFF 都因为 `tx` 不匹配被拒绝，PTT 一直没人释放。
- 根因（APP 端）：上一次 OFF 还在飞的时候推进 `tx` 并发出新的 OFF，
  板端从未接受过那个 `tx`，于是"释放"落空。
- 正确行为：
  - 板端：`tx >= 当前 tx` 的 OFF **一律接受**（OFF 权威，不要求严格匹配），
    并计入 `off_stale`；不匹配的 ON 仍然拒绝（`busy`），且**不推进** `tx`；
  - APP 端：上一轮没结束（`heldTx != 0` 或 `pttMayBeActive`）时直接忽略第二次按下，
    保证"一次只发一轮"。

诊断：`/api/status` 的 `sdr.on_reject` / `sdr.off_reject` / `sdr.off_stale` /
`sdr.begin_busy`。健康的快速连按只会让 `off_stale` 偶尔加一。

源码位置：`firmware/main/sdr.c`（`sdr_ptt_request`）、
`app/app/src/main/java/net/openradiolink/app/BoardSession.kt`（`beginPress`）。

### 3.4 `busy` 与 `owner_lost`

| reason | 含义 | 正确处置 |
|---|---|---|
| `busy` | 上一个持有者还在释放/播尾音（或 TALK 占着 DAC），新的 ON 被拒 | 等尾音结束再按；APP 会先用板端 `txid` 补发一次 OFF 清理，再报错让操作者重按，**不要**立刻强关 |
| `owner_lost` | PTT 已经不属于这个 `sid`+`tx`（被强制关闭、超时、或换了会话） | 用板端报告的 `txid` 发一次 OFF；只有在 owner 仍非 `none` 且不处于 `rel`/`tail` 时才 `force=1` |
| `hold_limit` | 单次按住超过 30 s | 松开重按 |
| `pcm_timeout` | 键控后 3 s 内没收到任何 PCM，或收到后又静音 2 s | 检查手机麦克风权限/录音是否被别的应用抢占；这类结束仍会播尾音，不是强关 |
| `network_not_ready` | 网络模式未就绪 | 见 3.5 |
| `radio_not_ready` | SA818 未配置好（频段/参数没提交） | 先在面板提交频率参数 |

关键设计：**不要一看到失败就强关**。释放请求在尾音期间是被接受的（`rel`/`tail` 为真），
此时 `force=1` 会把礼貌音切断，正好制造出"有载波没尾音"的假象。
APP 的策略是：先用 WS 发 OFF → HTTP OFF 带同一 `tx` → 轮询 `sdr_state`；
连续 800 ms 没确认、且板端仍报真实按住（`ptt=true` 且不在 `rel`/`tail`）才强关。

源码位置：`firmware/main/sdr.c`、`app/app/src/main/java/net/openradiolink/app/BoardSession.kt`
（`beginPress` 的 catch、`stopPtt`）。

### 3.5 听不到对端

按链路从下往上查：

| # | 检查 | 判据 |
|---|---|---|
| 1 | 模式对不对 | `/api/status` 的 `intercom.mode`：`0=local`、`1=netp2p`、`2=relay`。local 模式没有网络音频 |
| 2 | 引擎起来没有 | `intercom.state`：`stopped`/`no-engine`（没起）、`connecting`（中继等 broker）、`knocking`（P2P 在打洞）、`idle`/`tx`/`rx`。引擎起了也**不代表能发出去** |
| 3 | 能不能真发出去 | `/api/activity` 的 `net_ready`（对应 `netcom_tx_ready()`）；中继要 broker 已连接，P2P 要至少一个对端 `LINKED` |
| 4 | 房间/PSK | P2P 房间号 1000–9999 必须一致；PSK 不一致的帧会被静默丢弃 |
| 5 | 桥接半双工 | 桥自己键控期间 RF→网络被静音（防自激），此时听不到射频侧是正常的 |
| 6 | 静噪/功放 | 模块 COS 关着就没有声音。默认用 IO40 读硬件 COS；`/api/set?k=sqgpio&v=0` 会关掉硬件 COS、改为按电平推断——没做该改线的板子必须设 0，否则引脚被上拉读高、静噪永远不开 |
| 7 | 自己发自己 | 中继会把你的消息经通配订阅送回，靠帧头 MAC 过滤；过滤失效会听到自己 |
| 8 | 只有一个听众 | `/ws/sdr` 同时只允许一个客户端，第二个会被拒 |

未就绪时面板显示的原因就来自 `netcom_tx_ready_hint()`：

- 中继：`网络中继未就绪：EMQX 未连接（检查 broker 地址与用户名密码）`
- P2P：`公网直连未就绪：还没有对端连通（在面板添加对端并等待打洞完成）`

P2P 打洞失败的典型原因是**两端都在对称 NAT** 后面：这时要选
"端口保留（port-preserving）"的一侧作为 target，面板会显示 NAT 类型判断
（用 `54322` 探测端口是否保留）。这一步依赖具体网络，以源码为准
（`firmware/main/netp2pnet.c`）。

源码位置：`firmware/main/netcom.c`（`netcom_tx_ready`、`netcom_engine_running`、
`netcom_tx_ready_hint`）、`netp2pnet.c`、`netrelay.c`、`bridge.c`。

### 3.6 界面上会看到的固定提示

| 提示 | 触发 | 含义 |
|---|---|---|
| `请先开启收听` | 没开收听就按 PTT | 音频会话必须先建立（`sid`） |
| `手机通话仍在进行：请先结束通话再升级` | OTA 时还在发话 | 升级会先停引擎，不允许在发话中写 Flash |
| `发话网络积压，已停止麦克风` | WS 发送队列超过 32 KB | 链路太慢，继续发只会堆积 |
| `麦克风输入设备已切换，请核对音量后重新发话` | 录音中途换了输入设备 | 换设备后增益与路由都变了，必须重来 |
| `未能确认发射已停止` | 释放 2.5 s 内没确认 | 见 3.4，按 `sdr_state` 判断是否真卡住 |

源码位置：`app/app/src/main/java/net/openradiolink/app/PcmAudio.kt`、`BoardSession.kt`、
`firmware/main/net_http.c`（`h_ota`）。

---

## 4 状态与内存

### 4.1 HTTP 状态接口栈溢出（已修复）

- **症状**：升级到某个版本后，设备身份与活动接口都正常，但请求 `/api/status`
  会让手台立即重启。
- **触发条件**：状态 JSON 与处理函数的局部变量增加（新增自定义名称、更多诊断字段），
  `h_status` 的 Xtensa 函数入口帧涨到 8096 字节。
- **根因**：HTTP 与 HTTPS 任务总栈只有 10240 字节，8096 字节的帧加上路由与
  newlib 格式化调用，余量不足；主机上的逻辑测试无法验证 Xtensa 真实栈预算。
- **正确行为**：4608 字节的状态 JSON 响应改为**每次请求单独在 PSRAM 分配**，
  发送结束（含失败路径）即释放；分配失败返回 HTTP 500，而不是退回栈上。
  容量检查保留，HTTP 与 HTTPS 不共用缓冲。
- **防复发**：`net_http.c` 加 `-Werror=frame-larger-than=6144`，
  并生成 `.su` 栈用量报告；任何处理函数再把大响应放回栈上会直接编译失败。
- 源码位置：`firmware/main/net_http.c`（`h_status`）、`firmware/main/CMakeLists.txt`。

同一类问题的其他落点（当前代码已按此处理）：

| 位置 | 做法 |
|---|---|
| `/api/scan` 的 AP 列表（32 条约 3.5 KB） | 堆上分配，不放处理函数栈 |
| 日志尾环形缓冲 | `portMUX` 保护，两个 CPU 并发写入也不会撕裂 |
| WebSocket 帧缓冲 | 每个会话独立上下文，不在 handler 栈上 |

### 4.2 HTTP 任务内存

HTTP/HTTPS 两个 server 各占 10 KB 栈，再加上 lwIP pbuf、socket、TLS 会话，
都从内部 RAM 出。HTTPS 的 `max_open_sockets` 特意压到 5。

**已修复**：开面板时按 PTT 失败（`ESP_ERR_NO_MEM`）。

- 触发条件：面板走 HTTPS，浏览器保持 3–5 条 TLS 连接。
- 根因：内部堆最大连续块只剩 8 KB，而 `talk` 任务当时用 `xTaskCreate()`
  申请 8 KB 栈，正好要不到。
- 正确行为：`talk` 的栈改成 `.bss` 里的 6144 字节静态数组 + 静态 TCB，
  链接期一次性分配，碎片再多也不会拒绝一次按键；每次释放都会打印栈高水位，
  余量低于约 1 KB 就该把尺寸调回去。
- 源码位置：`firmware/main/voice.c`（`TALK_STACK_BYTES`、`talk_task`）。

**已修复**：panic 之后没有 coredump。

- 触发条件：任务栈溢出等 panic。
- 根因：coredump 分区只有 64 KB，而这套固件的任务栈总量（20 多个任务、
  两个 10 KB 的 httpd、8 KB 控制台）根本放不下，IDF 报
  `Not enough space to save core dump!` 后什么也不写。
- 正确行为：coredump 分区改为 256 KB（`0x920000`，`0x40000`），
  后面到 16 MB 都是空闲空间，不占别的分区。

### 4.3 PSRAM 缓冲

大而长命的东西都放 PSRAM，内部 RAM 留给栈与网络：

| 用途 | 位置 | 说明 |
|---|---|---|
| P2P 引擎/播放/发送/STUN 任务栈 | PSRAM | 引擎重启时不会再因为要不到那块内部栈而 `ESP_ERR_NO_MEM` |
| P2P 每对端抖动缓冲 | PSRAM | 128 槽压缩帧；4 个持久对端 + 1 个自测槽 |
| 中继抖动缓冲 | PSRAM | 128 槽，公网链路动辄停顿数秒 |
| SDR 收发环 | PSRAM | RX 环 3 s，TX 环 8192 样本 |
| 状态 JSON | PSRAM | 每请求 4608 字节，用完即放 |

SDR 发送侧还有一个"发不出去就丢"的计数：`sdr.werr`（WS 发送失败）与 `sdr.drop`
（环溢出）分开统计，用来区分"链路慢"和"板子自己丢了"。

源码位置：`firmware/sdkconfig`（PSRAM 相关）、`firmware/main/netp2pnet.c`、
`netrelay.c`、`sdr.c`、`net_http.c`。

### 4.4 内存告警怎么读

| 观察 | 含义 | 动作 |
|---|---|---|
| `heapIntMax` 持续下降 | 内部 RAM 碎片化 | 重启可解；复现则记下触发的操作序列 |
| 日志出现 `wifi: mem fail` | 内部 RAM 不够 Wi-Fi 驱动用 | 同上，通常发生在引擎反复切换后 |
| `psram` 缓慢下降 | 抖动缓冲/任务栈泄漏 | 切一次模式再切回来，看是否回收 |
| `rst=panic` + 有 coredump | 真崩溃 | 通过 `/api/coredump` 取回分析 |

---

## 5 已知限制

按"代码里就是这样"列出，不是待办清单：

1. **没有 OTA 自动回滚**。`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 未启用，
   坏镜像只能串口重刷。保留配置模式会重置 OTA 选择到 `ota_0`。
2. **没有控制租约**。`/api/control` 是兼容层，固定返回 `held:false, mine:true`；
   多入口同时操作靠一把互斥锁串行化，不是互斥访问。`ControlRules` 里的
   抢占逻辑在当前固件上不会真正生效。
3. **同时只有一个音频监听者**。`/ws/sdr` 第二个客户端会被拒；
   APP 与网页不能同时听。
4. **单次按住上限 30 s**（SDR），语音会话的绝对上限 180 s。
5. **中继只连明文 `mqtt://`、QoS 0、无房间号**。同一 broker 上任何订阅
   `orl/tx/+` 的人都能听到；固件不做音频加密（P2P 只有帧级 HMAC 校验，不是加密），
   TLS 需要在 broker 前面加隧道/VPN。
6. **P2P 依赖 UDP 打洞**。两端都是对称 NAT 时可能连不上；
   STUN 服务器列表可改，默认列表内置。
7. **音频编码固定**：Opus 16 kHz / 20 ms / 32 kbit/s CBR，无 FEC、无 DTX；
   丢包靠抖动缓冲与解码器 PLC。
8. **自定义尾音上限**：上传 ≤ 512 KB；转换后最多 `TAIL_CUSTOM_MAX_SAMPLES`
   （40000 样本，按 16 kHz 算约 2.5 s），μ-law 存进 1 MB 的 `storage` 分区。
   内置音共用一块 20000 样本的 PSRAM 池，池满后后合成的音会被截断。
9. **需要 HTTPS 才有麦克风**。HTTP 面板能看能控，但浏览器不给麦克风。
   证书是自签的、SAN 只有 DNS 名、随构建生成（换固件就可能换指纹）。
10. **配网 AP 是开放热点**，且 AP 与 STA 不共存；配网完成后应离开该状态。
11. **APP 只走 Wi-Fi**，没有蓝牙；Android 12+ 需要局域网/附近设备权限。
12. **刷机工具只支持 Windows x64**；不提供改 eFuse、关闭保护、任意地址写入；
    公开构建不内嵌固件。
13. **面板 `/api/firmware.bin` 给的是应用镜像**，不能当完整合并 BIN 使用。
14. **前面板 PTT 键不可改绑**（安全考虑），四个功能键可以。
15. **尾音不在本地喇叭播放**，只发到空中/网络——这是设计，不是故障。

---

## 附：一句话定位表

| 症状 | 先看 |
|---|---|
| 起不来 | 串口横幅 `rst=`、coredump |
| 面板打不开 | `ping <hostname>.local`、AP 是否开着、`/api/status` |
| 没声音 | `sdr.on`、`ptt`、`intercom.state`、`net_ready`、`sqgpio` |
| 松手不断 | `sdr.rel` / `sdr.tail` / `sdr.last_reason` / 6 s 上限 |
| 按了没反应 | `sdr.owner` / `sdr.reason` / `off_reject` / `begin_busy` |
| 升级失败 | `/api/ota` 的 `err`（small / too large / rejected / interrupted） |
| 内存问题 | `heapIntMax`、`wifi: mem fail` |
