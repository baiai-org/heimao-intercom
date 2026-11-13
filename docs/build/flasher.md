# 构建独立刷机工具

Windows 10/11 x64，Python 3.11、Tkinter、esptool与PyInstaller。工具1.1.4不内嵌整机固件；固件单独提供。

```powershell
python -m venv .tmp/flasher-build-env
.tmp/flasher-build-env/Scripts/pip install -r flasher/requirements-build.txt
powershell -ExecutionPolicy Bypass -File flasher/build.ps1 `
  -Python .tmp/flasher-build-env/Scripts/python.exe `
  -OutputDirectory dist/flasher `
  -FixtureBin dist/heimao-full-r11-93.bin `
  -AppBin dist/heimao-app-OTA-r11-93.bin
```

FixtureBin/AppBin只供离线测试和打包审计，不进入EXE。构建只从本次新建的隔离目录复制两个品牌图片，旧输出目录的assets不参与打包。已经移除内嵌构建参数。

产物：`dist/flasher/heimao-flasher-1.1.4.exe`及`bundle-verification.json`。17项离线测试；提供匹配full/OTA时不应跳过。无夹具时6项可执行、11项依赖真实映像而跳过。

```powershell
./dist/flasher/heimao-flasher-1.1.4.exe --diagnose empty.json
./dist/flasher/heimao-flasher-1.1.4.exe --firmware dist/heimao-full-r11-93.bin --diagnose external.json
```

检查空目录报告无固件、外部报告版本/摘要匹配，并且两者`embedded_firmware=false`、`bundled_firmware_files=[]`、`serial_opened=false`、`flash_requested=false`。单看所选外部文件不能证明EXE没有其他内置固件，因此构建还会检查PyInstaller归档资源。

工具可自动识别EXE同目录的配套full BIN，或由“选择BIN”加载其他版本。工具会验证完整镜像、分区布局、ESP32-S3及16 MB Flash；OTA应用文件不能当作完整镜像使用。

许可证及上游信息见[flasher/LICENSE](../../flasher/LICENSE)与[THIRD_PARTY](../../flasher/THIRD_PARTY.md)。固件与工具分别遵循各自许可证。
