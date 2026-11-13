# 发布与维护（维护者）

本文件面向仓库维护者，说明如何从源码发布一个版本。普通用户请看 [快速上手](getting-started.md)。

## 一次性准备

1. 在 GitHub 创建空仓库 `heimao-intercom`（不要勾选自动生成 README/LICENSE）；
2. 本地推送本仓库的 `main`：

```bash
git remote add origin git@github.com:<you>/heimao-intercom.git
git push -u origin main
```

3. 仓库设置：
   - 默认分支 `main`；
   - 开启 Secret scanning / Dependabot（如可用）；
   - 保护 `main`：禁止 force push，要求 PR 与 CI 通过；
   - About 写明：`ESP32-S3 网络电台固件 + Android 对讲 APP（源码公开，商用需授权）`。

## 日常发布

1. 更新版本：
   - 固件：修改 `firmware/VERSION`（如 `r11-93`），并把 `flasher/brand.json` 的 `default_bin` 同步为 `heimao-full-r11-93.bin`；
   - APP：修改 `app/app/build.gradle.kts` 的 `versionCode` / `versionName`；
   - 刷机工具：修改 `flasher/brand.json` 的 `version` 与 `flasher/version_info.txt`；
   - 更新 `CHANGELOG.md`。
2. 本地验证：

```bash
python tools/run_host_tests.py
python flasher/test_flasher.py        # 可选：设置 ORL_FLASHER_FIXTURE
cd firmware && idf.py build
cd app && ./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

3. 提交并打标签：

```bash
git add -A
git commit -m "release: v1.x.0"
git tag v1.x.0
git push origin main --tags
```

4. GitHub Actions 会：
   - 构建固件并生成 `heimao-full-<版本>.bin` 与 `heimao-app-OTA-<版本>.bin`；
   - 构建 APK；
   - 在 Windows 上构建不含内置固件的刷机工具；
   - 生成 `SHA256SUMS.txt` 并创建 Release。

> **v1.0.0 首次发布**建议手工上传已验证的交付二进制（`3ede0b50…` full.bin、`f34ed6cc…` OTA、`6e498669…` APK），它们与源码提交 `c9d20b4` / `63ab6d3` 对应；Actions 重建的产物哈希不同（编译器、时间戳、证书与签名环境差异），不要混用。后续版本再以 Actions 产物为准。

5. 发布前人工核对 Release：
   - 四个二进制（APK、full、OTA、EXE）与 `SHA256SUMS.txt` 都在；
   - APK 的 versionCode 与 CHANGELOG 一致；
   - 刷机工具 `--diagnose` 报告 `embedded_firmware=false`、`bundled_firmware_files=[]`、`serial_opened=false`；
   - 固件版本串是 `firmware/VERSION` 的值。

工具构建入口已移除内嵌选项；必须使用隔离资源目录并检查最终EXE。正式交付包包含本地交接资料时，不要直接把整包当成GitHub资产：使用单独整理的GitHub发布文件及源码ZIP。

## 安全与合规检查

- `git ls-files | grep -Ei 'pem|key|jks|keystore'` 必须为空；
- 不要提交 `firmware/main/servercert.pem` / `serverkey.pem`（`.gitignore` 已覆盖，由构建生成）；
- 不要把内部交接文档、设备 IP、`device_id`、诊断快照带进仓库；
- `flasher/` 之外的代码不得出现 GPL 头（CI `licence-check` 会拦）；
- 任何新增第三方代码都要更新 `THIRD_PARTY.md` / `NOTICE`。

## 旧仓库

旧的私有仓库保留在本地作档案，**不要**把它的历史、分支或二进制推送到公开仓库：

- 旧历史包含真实 HTTPS 私钥与内部设备信息；
- 公开仓库从 v1.0.0 的初始提交开始，保持线性、可审计。

## 商用授权

- 收到申请：核对主体、场景、数量、期限，签署授权书/合同；
- 授权书编号与订单/设备序列号关联；
- 随整机出货的授权覆盖该批硬件，终端用户无需再签；
- 不提供“口头授权”，全部走邮件/合同留档（mail@baiai.org）。
