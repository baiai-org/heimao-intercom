# 构建 Android APP

## 环境

| 项 | 要求 |
|---|---|
| JDK | 21 |
| Android SDK | Platform 37、Build-Tools 37.x |
| Gradle | 使用仓库自带 Wrapper（`app/gradlew`） |
| minSdk / targetSdk | 24 / 37 |
| 包名 | `net.openradiolink.app` |

## 配置 SDK 路径

在 `app/local.properties` 写入（该文件已被忽略）：

```properties
sdk.dir=C\:\\Users\\<you>\\AppData\\Local\\Android\\Sdk
```

或设置环境变量 `ANDROID_HOME`。

## 构建

```bash
cd app
./gradlew :app:assembleDebug
```

Windows：

```powershell
cd app
.\gradlew.bat :app:assembleDebug
```

产物：`app/app/build/outputs/apk/debug/app-debug.apk`。

## 测试与检查

```bash
cd app
./gradlew :app:testDebugUnitTest :app:lintDebug
```

- 单测 XML：`app/app/build/test-results/testDebugUnitTest/`
- Lint：`app/app/build/reports/lint-results-debug.html`

## 签名

- 本地 `assembleDebug` 使用 Gradle 自动生成的 debug keystore；
- 官方 Release APK 使用项目维护者的开发签名（证书 SHA256 见 Release 说明）；
- **自行构建的 APK 签名不同，不能覆盖安装官方版本**，需要先卸载或使用不同包名。

公开仓库不包含任何 keystore。需要可覆盖升级的构建时，请使用你自己的签名并统一保管私钥。

## Windows 便捷脚本

`tools/build_android.ps1` 封装了本地 JDK/SDK 路径与离线/低内存参数，适合本机反复构建；其他平台直接用 `gradlew` 即可。
