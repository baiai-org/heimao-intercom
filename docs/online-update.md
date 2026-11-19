# 黑猫互联在线更新

手台和 Android APP 在启动联网后检查 GitHub 正式 Release，以后约每天再查一次。手台首次检查会等网络校时完成。APP「更多 → 关于与更新」和手台网页或 APP「诊断与维护 → 手台在线更新」提供手动检查。

发现匹配的新版后自动下载。普通更新可选「立即安装」或「稍后」。选「稍后」会保留已下载文件。APP 的新版通知由手机发出，取决于通知权限。强制固件更新在带回滚能力的手台上空闲安装并重启。Android 在允许自动安装时会在后台安装，若要求系统确认，APP 会保留安装包并提供「继续安装」。

## 发布普通更新或强制更新

1. 在 GitHub 创建 Release 草稿。准备已验收的 OTA 应用 BIN 或与当前安装证书相同的 APK。固件和 APP 可同发，也可单独发布。
2. 在本地生成签名清单。命令需要 Python 3.11 及 OpenSSL，`$updateSigningKey` 为本地签名私钥路径。给两个组件各选 `normal` 或 `force`。提供新文件时，省略模式默认普通。设置 `--previous` 会保留上一份清单中未改的组件及其资产和发布标签。

   ```powershell
   python tools/make_update_manifest.py --key $updateSigningKey --output update.json --firmware heimao-app-OTA-r11-108.bin r11-108 108 r11-108 --firmware-mode normal --app heimao-android-1.2.22-heimao.apk r11-108 34 1.2.22-heimao --app-mode normal
   ```

3. 把签名清单 `update.json` 与它所引用的新资产上传到对应标签的草稿。固件在线更新引用 OTA 应用 BIN，USB 刷机使用独立 full BIN。APK 上传前核对包名、版本号及安装证书。如果 CI 产物的证书不同，先用已验收的同证书 APK 替换草稿中的 APK，再更新校验文件。在 GitHub 草稿中选择发布为最新正式 Release。保留旧标签中仍被清单引用的资产。

只变更更新方式时可复用原资产。创建新的 Release，上传一份重新签名的清单，并发布为最新正式 Release。例如把已发布的固件改为强制。

```powershell
python tools/make_update_manifest.py --key $updateSigningKey --previous previous-update.json --output update.json --firmware-mode force
```

改回普通用 `--firmware-mode normal`。撤回某组件用 `--firmware-mode withdraw` 或 `--app-mode withdraw`。客户端在自动安装前还会读一次最新清单。已经提交给引导程序或 Android 的安装可能继续执行。策略在设备下一次成功检查时生效。

签名私钥存于仓库外，和 APK 安装证书对应的签名私钥分开保管。清单工具会检查签名是否匹配客户端内置的公钥。发布文件版号递增，同一版号对应同一份二进制。

## 测试顺序

1. 先用现有入口安装带在线更新的 APP 与固件。旧版引导程序可以准备 OTA，但会显示需要手动安装。验证无人值守强制 OTA 时，通过刷机工具与带回滚引导程序的 full BIN 在测试机建立基线。分区布局相同的手台可用「保留配置刷机」写入引导程序。回退到旧版 r11-107 时，使用对应的 full BIN 同时恢复旧引导程序。
2. 发布一个比当前安装版本新的普通版本。验证启动、定期和手动检查。普通版自动下载，选「稍后」仍运行旧版。点「立即安装」后检查版本和原有配置。
3. 用测试机检查强制策略。通联时观察版本保持，通联结束后核对安装结果。Android 分别核查免确认和要求系统确认的情形。手台验证断网、备用槽写入中断、首启失败回滚及同一坏镜像的处理。
