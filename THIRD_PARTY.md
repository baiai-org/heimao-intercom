# 第三方组件与许可证

本项目原创代码按 [PolyForm Noncommercial 1.0.0](LICENSE) 发布；`flasher/` 按 GPL-3.0-or-later 发布。下表列出随仓库分发的第三方代码与运行时依赖。第三方组件始终按其原始许可证授权，本项目的商业授权不改变、也不额外限制这些权利。

## 固件（ESP-IDF 5.4.1，target esp32s3）

| 组件 | 位置 | 上游 | 许可证 | 修改说明 |
|---|---|---|---|---|
| ESP-IDF | 工具链（不随仓库分发） | espressif/esp-idf | Apache-2.0 | 未修改 |
| mDNS 响应器 | `firmware/components/mdns` | espressif/esp-idf `components/mdns` | Apache-2.0 | 本地覆盖：发送/接收/控制台拆分，见组件内 `README.md` |
| esp-mqtt | `firmware/components/mqtt` | espressif/esp-idf `components/mqtt` | Apache-2.0 | 本地覆盖：部分消息轮询、任务栈置于 PSRAM，见 `PATCH.md` |
| ES8311 驱动 | `firmware/components/es8311` | espressif/esp_codec_dev 系列 | Apache-2.0 | 本地裁剪，见 `PATCH.md` |
| 配网 DNS | `firmware/components/dns_server` | ESP-IDF 示例 | Unlicense OR CC0-1.0 | 本地集成 |
| minimp3 | `firmware/main/minimp3.h` | lieff/minimp3 | CC0-1.0 | 未修改 |
| Opus | `esphome/micro-opus`（Component Manager） | espressif/esphome | BSD-3-Clause | 未修改 |
| G.711 / G.722 | `firmware/main/g711.c`、`g722.c` | ITU 建议 / 公共领域实现 | 见文件头 | 未修改 |

## Android APP

| 组件 | 许可证 |
|---|---|
| AndroidX、Jetpack Compose、Material 3 | Apache-2.0 |
| Kotlin 标准库与协程 | Apache-2.0 |
| Gradle Wrapper | Apache-2.0 |

依赖版本见 `app/app/build.gradle.kts`、`app/gradle.properties` 与 `app/settings.gradle.kts`。

## 主机测试

| 组件 | 位置 | 许可证 |
|---|---|---|
| cJSON | `tools/vendor/cJSON` | MIT（Dave Gamble and cJSON contributors） |

cJSON 仅用于在 PC 上编译并执行固件的 HTTP 处理函数，不进入固件镜像。

## 刷机工具（GPL-3.0-or-later）

| 组件 | 许可证 | 说明 |
|---|---|---|
| arnaudlvq/ESP32-Flasher（`upstream/flasher_tk.py`） | GPL-3.0 | GUI 基础，本项目修改为中文界面、多串口、S3 合并 BIN 校验、两种刷机模式 |
| esptool 4.8.1 | GPL-2.0-or-later | 原样使用，随 EXE 分发；对应源码见 <https://github.com/espressif/esptool/tree/v4.8.1> |
| CPython / Tcl / Tk | PSF | 随 EXE 分发，许可证在 `flasher/licenses/` |
| PyInstaller 6.16.0 | GPL-2.0-or-later + bootloader exception | 仅构建期使用 |
| pyserial、cryptography 等 | BSD / Apache-2.0 / MIT | 见 `flasher/requirements-lock.txt` 与 `flasher/licenses/` |

## 品牌素材

`assets/brand/` 中的名称、Logo、图标不适用上述代码许可证，见 [TRADEMARK.md](TRADEMARK.md)。

## 合规提示

- 再分发固件或 APP 时，请保留本文件、`NOTICE` 与各组件的许可证文本。
- 再分发刷机工具时，必须同时提供 GPL-3.0 对应的完整源码（本仓库即对应源码）。
- 不要在再分发时移除第三方文件头中的版权与许可证声明。
