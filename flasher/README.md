# 黑猫互联刷机工具

Windows 10/11 x64，单文件 EXE，免安装、离线运行，不需要 Python/Java/.NET。基于 [arnaudlvq/ESP32-Flasher](https://github.com/arnaudlvq/ESP32-Flasher)（GPL-3.0）的 Tkinter 界面改造，底层使用 esptool 4.8.1。

> **许可证：GPL-3.0-or-later。** 源码随仓库提供（`upstream/` 为上游原文件）；再分发 EXE 时必须同时提供源码。见 [LICENSE](LICENSE) 与 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 使用

1. 从 Releases 下载 `黑猫互联刷机工具-<版本>.exe` 和 `黑猫互联-full-r11-83.bin`，放在同一目录；
2. 双击运行，USB 连接手台；
3. 勾选要刷写的 COM 口（可多选，真正并行）；
4. 点击“选择 BIN”，选中 `full.bin`；
5. 选择模式，点击“开始刷写”。

> 公开版**不内嵌固件**：工具是 GPL-3.0、固件是另一套许可，混装会把固件带进 GPL。工具会在开始前校验 BIN 类型、分区布局、芯片与 Flash 大小。

| 模式 | 行为 |
|---|---|
| 保留配置（默认） | 只写 bootloader / 分区表 / otadata / 应用；保留 NVS、PHY、storage、另一 OTA 槽、coredump |
| 全新刷机 | 擦除整片 Flash 后写入，清空配置与自定义内容 |

## 构建

```powershell
python -m venv .tmp/flasher-build-env
.tmp/flasher-build-env/Scripts/pip install -r flasher/requirements-build.txt
powershell -ExecutionPolicy Bypass -File flasher/build.ps1 `
  -Python .tmp/flasher-build-env/Scripts/python.exe `
  -OutputDirectory dist\flasher
```

内部构建可加 `-WithFirmware -FirmwareBin <full.bin>` 内嵌固件；**不要把这个版本发给客户**（会把固件置于 GPL 之下）。

## 测试

```bash
set ORL_FLASHER_FIXTURE=D:\path\to\黑猫互联-full-r11-83.bin
python flasher/test_flasher.py
```

没有夹具时，解析/并发相关的 11 项会跳过，仍会运行 3 项无夹具的解析与计划校验。

## 目录

| 文件 | 作用 |
|---|---|
| `app.py` | Tkinter GUI 与入口 |
| `firmware.py` | 合并 BIN 解析、分区布局校验、刷写计划 |
| `runner.py` | 多串口并行、取消、事件汇总 |
| `worker.py` | 单端口 worker（esptool 调用） |
| `make_firmware.py` | 从 `firmware/build/` 生成完整合并 BIN |
| `test_flasher.py` | 离线回归 |
| `upstream/flasher_tk.py` | 上游 GPL 源码（未修改） |
| `licenses/` | CPython/Tk/Tcl 等运行时许可证 |
