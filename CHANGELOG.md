# 更新记录

本项目从 v1.0.0 起对外发布。更早的内部迭代（r1–r11）保留在私有档案中，不随仓库发布。

## v1.0.0 — 2026-11-08

首个公开版本，源码公开（source-available），个人可自由二次开发，商业使用需授权。

### 固件 `r11-83`

- 默认硬件 COS（载波检测）改为 `IO40`；NVS 中 `sqgpio=0` 表示显式关闭，`-1`/缺键保留默认；
- 短按（<300 ms）尾音：释放只等发送环清空，不再等注入环预充；
- 快速连按不再锁死发射：新 ON 失败不推进 `s_tx_id`，释放回退真实 owner token；
- 释放上限按所选尾音长度 + 600 ms（绝对上限 6 s）；
- RF→网络默认增益 32 → 16（0 dB）；
- `/api/status.sdr` 暴露释放/尾音/所有权计数，便于验证；
- 板载尾音是唯一来源，APP 不再自播 880/660 Hz。

### APP `1.2.13`（versionCode 25）

- `heldTx`：上一轮未收尾不并发新 ON；
- busy / owner_lost 时按板端 `txid` 重发 OFF，必要时 force；
- 停止收听 5 秒状态机、PTT-OFF 2.5 秒重试、800 ms force 兜底；
- 上传自定义尾音前先确认板端停止收听。

### 刷机工具 `1.1.3`

- 修复 `COM1`–`COM9` 作为工作目录名导致的 Win10“目录名无效”；
- onefile 自解压回到当前目录（`--runtime-tmpdir .`）；
- **公开构建不再内嵌固件**：工具为 GPL-3.0 衍生作品，固件按 PolyForm Noncommercial 分发，二者不混装；用户在工具内选择 `full.bin`。

### 仓库

- 单仓库结构：`firmware/`、`app/`、`flasher/`、`tools/`、`docs/`；
- 移除真实 HTTPS 私钥，改为构建时生成；
- 移除内部设备 IP、`device_id`、内部交接文档；
- 新增 LICENSE、COMMERCIAL、CLA、TRADEMARK、NOTICE、THIRD_PARTY、SECURITY、CONTRIBUTING；
- 固件 BIN 文件名带固件版本号：`heimao-full-r11-83.bin`、`heimao-app-OTA-r11-83.bin`；
- 新增 GitHub Actions：固件构建、主机回归、APP 构建、刷机工具测试、secret scan、license check、release。

### 已知问题

- 源码重建的版本串由 `firmware/VERSION` 固定为 `r11-83`，与已交付 BIN 的 `r11-83-gc9d20b4` 不同；不承诺字节级复现。
- 4 个历史主机回归（`test_tx_ownership.py`、`test_radio_profile.py`、`test_bridge_edges.py`、`test_app_control_panel.js`）在旧仓库即失败，本版先以显式 skip/xfail 标记，待修复。
- 真实 USB 刷写、断电名称持久化、P2P/中继对端尾音听感仍需实板验收。

### 升级说明

- 官方 APK 使用开发签名；自行构建的 APK 签名不同，**不能覆盖安装**官方版本。
- 从旧版升级请保留配置刷机；跨大版本降级前先确认 NVS 兼容性。
