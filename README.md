# 黑猫互联 · Heimao Intercom

![黑猫互联](assets/brand/icon-512.png)

ESP32-S3 网络对讲手台：一块板子同时是 **SA818S 模拟电台**、**网络语音终端**（局域网直连 / P2P / MQTT 中继）、**6 段 EQ**、**99 槽信道库**，自带浏览器控制面板与 Android 对讲 APP。

> **许可声明（请先读）**
> 本仓库是**源码公开（source-available）**，不是 OSI 开源项目。
> 个人学习、研究、修改、非商业分发免费；**任何商业使用都需要书面授权**（[COMMERCIAL.md](COMMERCIAL.md)，mail@baiai.org）。
> 代码许可：[PolyForm Noncommercial 1.0.0](LICENSE)。`flasher/` 为 GPL-3.0 衍生作品，单独授权。

---

## 它是什么

| 部分 | 说明 |
|---|---|
| 手台固件 | ESP32-S3-N16R8（16 MB Flash / 8 MB PSRAM），驱动 SA818S 射频模块、ES8311 音频编解码、前面板按键与 LED |
| 网页面板 | 内嵌在固件里，浏览器打开即可配置频率/信道/音频/网络，支持 HTTPS（浏览器麦克风需要安全上下文） |
| Android APP | Kotlin/Compose，局域网发现、设备会话、PTT 发话、本机试听、参数设置 |
| 刷机工具 | Windows 单文件 EXE，保留配置 / 全新刷机两种模式，多串口并行 |

### 主要能力

- **模拟 + 网络双链路**：RF ↔ 网络桥接、网络 P2P 直连、MQTT 中继
- **信道库**：99 槽，频率、功率、亚音（CTCSS/DCS）、静噪、音量可配
- **音频**：PCM16 / Opus 链路，6 段 EQ，板载尾音（唯一来源）
- **并发写保护**：网页与 APP 可同时连接；写操作由板端串行锁保护，冲突返回 409（无登录、无控制租约）
- **升级**：OTA 双分区；保留配置刷机不擦 NVS/PHY/SPIFFS/另一 OTA 槽
- **配网**：开放热点 `黑猫智能手台-XXXXXX` + `http://192.168.4.1/`，支持 mDNS

> 这是个人/小团队自组网项目，不是商用对讲机替代品。实际通联距离、音质与稳定性取决于硬件、天线、环境与法规许可。

---

## 下载

从 [Releases](../../releases) 获取（只提供这三类文件 + 校验）。固件 BIN 的文件名带固件版本号（当前 `r11-83`），升级后文件名随之变化：

| 文件 | 用途 |
|---|---|
| `heimao-android-<版本>.apk` | Android APP（Android 7.0+） |
| `heimao-full-r11-83.bin` | 完整固件，USB 刷机 / 救砖 |
| `heimao-app-OTA-r11-83.bin` | OTA 升级镜像（网页面板或 APP 上传） |
| `heimao-flasher-<版本>.exe` | Windows 刷机工具（**不含内置固件**，需选择上面的 full BIN） |
| `SHA256SUMS.txt` | 校验 |

> 刷机工具是 GPL-3.0 程序、固件是另一套许可，所以公开版不内嵌固件。下载后先选版本化的 full BIN，再开始刷写。

---

## 快速开始

### 1. 刷机

1. 下载 `heimao-full-r11-83.bin` 与刷机工具；
2. 用 USB 数据线连接手台，打开工具，勾选出现的 COM 口；
3. 点击“选择 BIN”，选中 `heimao-full-r11-83.bin`（或当前版本）；
4. 选择模式：
   - **保留配置**（默认）：保留 Wi-Fi、信道、参数、自定义尾音；
   - **全新刷机**：擦除整片 Flash，刷完重新配网。
5. 点击开始，等待写入与校验完成。

> 没有 COM 口时：换数据线；按住 BOOT、点按 RST、松开 BOOT 后刷新。ESP32-S3 原生 USB 会枚举为 “USB-Serial/JTAG”。

### 2. 配网

1. 首次开机或长按配网键后，手台开启热点 `黑猫智能手台-XXXXXX`；
2. 手机/电脑连接该热点，打开 `http://192.168.4.1/`；
3. 选择家里的 Wi-Fi 并输入密码，保存；
4. 手台重启并加入局域网，面板地址为 `http://<手台IP>/`（或 `https://<手台IP>/`）。

### 3. 打开面板

- 浏览器访问 `http://<手台IP>/`；需要麦克风时用 `https://` 并接受自签证书告警；
- 面板可配置频率/功率/亚音/静噪、信道、音频增益、EQ、网络模式、尾音、OTA。

### 4. 安装 APP

安装 Release 中的 APK。APP 与网页共用控制权：一方发话时另一方等待或手动接管。

---

## 构建

| 部分 | 入口 | 文档 |
|---|---|---|
| 固件 | `cd firmware && idf.py build`（ESP-IDF 5.4.1） | [docs/build/firmware.md](docs/build/firmware.md) |
| APP | `cd app && ./gradlew :app:assembleDebug`（JDK 21 / SDK 37） | [docs/build/android.md](docs/build/android.md) |
| 刷机工具 | `flasher/build.ps1`（Python 3.11） | [docs/build/flasher.md](docs/build/flasher.md) |

生成的完整 BIN 可用 `flasher/make_firmware.py` 从 `firmware/build/` 合并。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/getting-started.md](docs/getting-started.md) | 从零上手：刷机、配网、面板、APP |
| [docs/hardware.md](docs/hardware.md) | 硬件组成、模块、接线要点 |
| [docs/PINMAP.md](docs/PINMAP.md) | ESP32-S3 引脚映射 |
| [docs/architecture.md](docs/architecture.md) | 固件分层、音频链、网络模式、控制权 |
| [docs/api/http-api.md](docs/api/http-api.md) | HTTP/HTTPS API |
| [docs/api/audio-ws.md](docs/api/audio-ws.md) | WebSocket 音频协议 |
| [docs/flashing.md](docs/flashing.md) | 刷机、OTA、回退、排错 |
| [docs/troubleshooting.md](docs/troubleshooting.md) | 常见故障 |
| [docs/release.md](docs/release.md) | 维护者发布流程 |

---

## 目录结构

```text
firmware/   ESP-IDF 固件（main/ 与本地覆盖组件 components/）
app/        Android 工程（Kotlin + Compose）
flasher/    Windows 刷机工具（GPL-3.0，含上游源码）
tools/      构建、主机回归、OTA、引脚生成
docs/       文档
assets/     品牌素材（商标保留，见 TRADEMARK.md）
```

---

## 许可、商用与贡献

- 代码：[PolyForm Noncommercial 1.0.0](LICENSE)（个人可自由二次开发；商用需授权）
- 商用授权：[COMMERCIAL.md](COMMERCIAL.md) · mail@baiai.org
- 贡献：[CONTRIBUTING.md](CONTRIBUTING.md)（PR 需签署 [CLA](CLA.md)）
- 第三方组件：[THIRD_PARTY.md](THIRD_PARTY.md) · [NOTICE](NOTICE)
- 品牌/商标：[TRADEMARK.md](TRADEMARK.md)
- 安全：[SECURITY.md](SECURITY.md)

## 免责声明

本项目用于业余无线电与自组网实验。使用者必须自行确认所在地区的频率、功率、执照与发射许可要求；因违规发射、改装或使用造成的后果由使用者承担。软件按“现状”提供，不附带任何担保。
