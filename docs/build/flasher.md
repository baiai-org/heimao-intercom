# 构建刷机工具

刷机工具是 Windows x64 单文件程序，Python + Tkinter + esptool，打包用 PyInstaller。

## 环境

- Windows 10/11 x64
- Python **3.11**
- 构建依赖：

```bash
python -m venv .tmp/flasher-build-env
.tmp/flasher-build-env/Scripts/pip install -r flasher/requirements-build.txt
```

## 构建（公开版，不含内置固件）

```powershell
powershell -ExecutionPolicy Bypass -File flasher/build.ps1 `
  -Python .tmp/flasher-build-env/Scripts/python.exe `
  -OutputDirectory dist\flasher
```

产物：`dist/flasher/黑猫互联刷机工具.exe`。

公开构建**不内嵌固件**：

- 刷机工具是 GPL-3.0 衍生作品；
- 固件按 PolyForm Noncommercial 分发；
- 内嵌会把固件带进 GPL，因此工具只提供“选择 BIN”。

用户在工具中点“选择 BIN”，选中 `黑猫互联-full.bin` 后开始刷写。工具会校验：

- 不是 OTA 应用 BIN；
- 分区布局与本板一致；
- bootloader / 应用段的校验和与 SHA256；
- 芯片为 ESP32-S3、Flash 为 16 MB、DIO/80 MHz。

## 构建（内部版，内嵌固件）

仅供自己使用、不对外分发：

```powershell
powershell -ExecutionPolicy Bypass -File flasher/build.ps1 `
  -Python .tmp/flasher-build-env/Scripts/python.exe `
  -OutputDirectory dist\flasher-internal `
  -WithFirmware -FirmwareBin dist\黑猫互联-full.bin
```

> 一旦把内嵌固件的 EXE 发给客户，GPL 会覆盖整个分发物（包括其中的固件），与固件的商业授权冲突。对客户请始终发“无内嵌”版本 + 单独的 `full.bin`。

## 测试

14 项离线测试，不需要串口；没有夹具时固件解析/并发相关的 11 项会跳过，仍运行 3 项解析与计划校验：

```bash
set ORL_FLASHER_FIXTURE=D:\path\to\黑猫互联-full.bin
set HM_FLASHER_APP=D:\path\to\openradiolink.bin
python flasher/test_flasher.py
```

## 许可证

`flasher/` 衍生自 [arnaudlvq/ESP32-Flasher](https://github.com/arnaudlvq/ESP32-Flasher)（GPL-3.0），按 **GPL-3.0-or-later** 分发：

- 源码随仓库提供，`flasher/upstream/flasher_tk.py` 为上游原文件；
- 再分发 EXE 时必须同时提供源码；
- esptool 为 GPL-2.0-or-later，随 EXE 分发；
- 详见 `flasher/LICENSE`、`flasher/THIRD_PARTY.md`。
