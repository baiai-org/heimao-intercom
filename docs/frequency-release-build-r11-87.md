# r11-87 整套出包记录

## 当前整套交付（2026-11-11，r11-87 / APP 1.2.17，优先于下方历史）

用户要求把最新文件整理为整套交付，准备自行刷机并用综测测实际频率；最终明确只要 1、2.5、5 kHz，取消 6.25 kHz。
本轮交付为综测候选，不代表实板射频已验收；没有连接、刷写、键控任何设备，也没有推送远端。

- 稳定交付：`<delivery>`，根目录为新 r11-87 OTA/full、APP 1.2.17-heimao（29）、
  内置同版 full 的刷机工具 1.1.3，以及源码、ELF/map、说明、综测 CSV、许可、脱敏验证和 SHA/ZIP。
  完整 ZIP 为 `黑猫互联-完整交付包.zip`；最终 ZIP 摘要看同目录 sidecar，避免文档哈希循环。
- 源码主入口仍为 `<repository>`，分支 `fix/keys-channels-hub-20261111`。
  固件/工具与源码 ZIP 提交 `888580dc42ef535bf75726c0226488a8829d9ba6`；APP 编译提交 `4d6f226c583c084e260c5566e335a60e1e955f0f`，
  APP 源码与该 ZIP 的 app 树相同。后续文档提交不代表重编。
- 新 OTA SHA256 `ec88f3af93e3c9589ddbeeb9c5c6edcdab416b9b148b16230bec90e32b835d4f`；full `1c7b9467464c644c637999b01e456551b84fb618fa96f644ce2a790fb13b6493`；
  ELF `002ee18b015246c69040427424b6e3a2ff95bbce5602e5582dbc8ddbcedbcddf`；APK `ab2e89e90a7cb28be8f3d24cafec1554020fe6d026c8300abdd7433f35faccc7`；
  EXE `9283dd4e262b9a01222512ec4cb614dd281ec1b684b654a26a4012b294c02060`。运行状态 build.sha256 对应 ELF，不是 OTA 文件 SHA。
- APP 包名仍为 net.openradiolink.app，开发签名证书仍为 `5c928fded87d7aee27cc88b8b7c543a3376827c31a7c0a9b26eaa06eee3efcdf`，覆盖安装，不先卸载。
  固件 HTTPS 证书延续，未把证书私钥/签名密钥单独打包。
- 新增 1/2.5/5 kHz：API、网页、APP、信道兼容校验同步。500 Hz 是与原 12.5 kHz 组合的共同设置网格，
  不宣称硬件最小步进；AT 保持四位小数。6.25 kHz/100 Hz 均不开放。网页加减立即下发收发对，APP 加减先改草稿再保存。
  旧固件无 freq_grid_hz:500 能力时，新 APP/网页回退 12.5 kHz 校验；带宽选择与频率步长独立。
- 包含 r11-86 音量键（加 GPIO39 / 减 GPIO38）、统一切信道、Hub 转发身份/IP/角色修复；
  延续 r11-85 的 16 对端迁移、按需音频缓存、网络监听分流、固定时间接收保护。
- 实际验证：最终固件与 APP 构建退出 0（单 worker）；APP 87 项单测实际执行通过，Lint 0 错误 / 22 警告；
  主机 27 passed / 2 XFAIL / 0 failed（既有 test_tx_ownership.py、test_app_control_panel.js）；
  频率代码逐一扫过 VHF/UHF 合计 240002 个网格点的解析/float/格式化往返，真实 UART 序列化函数验证 1/2.5/5 kHz。
  full fixture 的刷机工具 14 项全通过，无跳过；实际冻结 EXE --diagnose 内置 full/app/ELF 一致，serial_opened=false/flash_requested=false。
  Chromium 模拟 HTTP 的桌面/手机尺寸频率、拒绝恢复、旧固件回退及信道/Hub/16 对端/接收保护通过。
  h_status 静态帧 4544 B、h_radio 960 B，均小于 6144 B。
- 出包时发现 VERSION 未自动触发重配，首轮增量构建仍标 r11-86，未交付；已在 CMake 加入 VERSION 依赖，
  重建后镜像头/manifest 均核对 r11-87。首轮无 fixture 的跳过结果不算通过，已用最终 full 重跑 14 项全通过。
- 本次未实测：实际载频、RX 中心、TX 小步进、UHF 参数实板、USB、断电、手机界面、三台 Hub 通联。
  状态值是已保存/ACK 确认的配置，不是载频测量或逐次模块 AT 读回。由用户安装后填写综测记录表。
- 最近实板观察仍为前序调试结束的 .50 / 示例设备V50 / VHF / r11-85（约 09:23）；
  已从临时诊断版回到原机 BIN 且零键控发射。其他设备本轮未联系，不推断其当前版本。
- 原 r11-85/r11-86 冻结测试目录不变。新包附原 r11-85 OTA/full 供回退；回退前先把当前频率恢复到 12.5 kHz 网格，
  小步进信道备份后按旧固件兼容情况处理，不自动取近似值。旧正式 r11-83/APP 1.2.13 整体归档到
  `<delivery>-历史版本\20261111-r11-83-app1.2.13-before-r11-87`。
- 永久证据：`<workspace>/diagnostics/20261111-frequency-release-r11-87`；仅复制明确选择的脱敏文件，
  前序探测 private、配置、NVS、凭据及诊断镜像禁止递归交付。一次性脚本保留追溯，不可原样重跑到下一版。

