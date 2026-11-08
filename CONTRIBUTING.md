# 参与贡献

感谢你愿意改进黑猫互联。提交前请先读 [README](README.md)、[COMMERCIAL.md](COMMERCIAL.md) 与 [LICENSE](LICENSE)。

## 可以贡献什么

- Bug 修复、测试、文档、翻译；
- 硬件适配（不同模块、不同按键/面板）；
- 性能与稳定性改进。

新功能请先开 Issue 讨论，避免做完才发现方向不一致。

## 贡献者许可协议

**所有代码 PR 必须先同意 [CLA](CLA.md)**：在 PR 中勾选复选框并评论
`I have read the CLA and agree to its terms.`。没有 CLA 的 PR 不会被合并。

原因：项目需要同时以非商业许可发布、并向商业客户授权；没有可再许可的权利，含你代码的版本就无法商业授权。

## 开发环境

| 部分 | 要求 |
|---|---|
| 固件 | ESP-IDF **5.4.1**，target `esp32s3`（16 MB Flash / 8 MB PSRAM） |
| APP | JDK 21、Android SDK 37、Gradle Wrapper |
| 刷机工具 | Python 3.11，依赖见 `flasher/requirements-lock.txt` |

固件构建：

```bash
cd firmware
idf.py set-target esp32s3   # 仅首次；本仓库 sdkconfig 手工维护，不要用 menuconfig 覆盖
idf.py build
```

> `firmware/sdkconfig` 是受控源码，不是构建产物。修改配置请直接改文件并在 PR 中说明原因。

APP 构建：

```bash
cd app
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

刷机工具测试：

```bash
python flasher/test_flasher.py
# 需要一份完整合并 BIN 作为夹具：
#   ORL_FLASHER_FIXTURE=/path/to/heimao-full.bin
#   HM_FLASHER_APP=/path/to/openradiolink.bin
```

## 测试要求

- 修 Bug 请附带能复现的回归测试；
- 涉及 HTTP/控制权/音频协议时，跑 `python tools/run_host_tests.py`（需要 gcc 与 node）；
- 涉及 APP 时跑单测 + Lint；
- 涉及刷机工具时跑 `python flasher/test_flasher.py`；
- CI 必须全绿（已知的 4 个历史 XFAIL 除外）。

## 代码风格

- C：跟随现有文件风格，4 空格缩进，不要引入新的全局缓冲；
- Kotlin：4 空格，Compose 组件保持无状态优先；
- Python：PEP 8，中文注释可以保留；
- 提交信息用 `type(scope): summary`，例如 `fix(sdr): release owner token on rejected ON`。

## 许可证与文件头

- 原创代码：`SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0`
- `flasher/` 下的代码：`SPDX-License-Identifier: GPL-3.0-or-later`
- 不要把 GPL 代码复制进 `firmware/` 或 `app/`；也不要把第三方代码去掉版权头。

## 不要提交

- 私钥、证书（`serverkey.pem` 由 `tools/gen_dev_cert.py` 生成）、keystore；
- 真实设备 IP、`device_id`、Wi-Fi/MQTT 密码、串口日志中的个人信息；
- 编译产物（`firmware/build/`、`app/**/build/`）、APK/BIN/EXE/ZIP。

## 行为准则

参与本项目即表示同意 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

## 联系

- 一般问题：GitHub Issue
- 商用授权：mail@baiai.org
- 安全问题：见 [SECURITY.md](SECURITY.md)
