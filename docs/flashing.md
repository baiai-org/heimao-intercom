# 刷机、OTA 与回退

## 两种镜像，别弄混

| 文件 | 用途 | 能不能 OTA |
|---|---|---|
| `黑猫互联-full-r11-83.bin` | 0x0 起始的完整合并镜像（bootloader + 分区表 + OTA 初始数据 + 应用） | **不能** |
| `黑猫互联-app-OTA-r11-83.bin` | 仅应用镜像 | 能 |

OTA 上传 full.bin 会失败；USB 刷机也不要用 OTA bin 当完整镜像。刷机工具会拒绝错误类型。

## 分区布局

| 区域 | 偏移 | 大小 |
|---|---|---|
| bootloader | `0x0` | ~20 KB |
| 分区表 | `0x8000` | 4 KB |
| NVS | `0x9000` | 24 KB |
| PHY | `0x11000` | 4 KB |
| otadata | `0xF000` | 8 KB |
| ota_0（应用） | `0x20000` | 4 MB |
| ota_1 | `0x420000` | 4 MB |
| storage（自定义尾音等） | `0x820000` | 1 MB |
| coredump | `0x920000` | 256 KB |

以 `firmware/partitions.csv` 与实际构建分区表为准。

## USB 刷机

### 刷机工具（推荐）

1. 下载 `full.bin` 与刷机工具；
2. 连接 USB，勾选 COM 口；
3. “选择 BIN”选中 `full.bin`；
4. 选择模式并开始。

| 模式 | 行为 |
|---|---|
| 保留配置 | 只写 bootloader / 分区表 / otadata / 应用四个区段；不写 FF 填充区，保留 NVS、PHY、storage、另一 OTA 槽、coredump |
| 全新刷机 | 擦除整片 Flash 后写入，清空 Wi-Fi、信道、参数、自定义尾音 |

保留模式会先读回目标板的分区表并比对布局，不一致时拒绝写入。

### esptool 手动刷写

```bash
# 保留配置：只写必要区段（示例，地址以实际分区表为准）
esptool.py --chip esp32s3 -p COM5 -b 460800 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 openradiolink.bin

# 全新刷机：整片擦除 + 合并镜像
esptool.py --chip esp32s3 -p COM5 -b 460800 erase_flash
esptool.py --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 黑猫互联-full-r11-83.bin
```

`write_flash 0x0 full.bin` 会把镜像中间的 `0xFF` 填充区一并写入，**等于清掉 NVS/PHY**；需要保留配置时用上面的分段方式或刷机工具的保留模式。

### 没有 COM 口

- 换一根**数据线**（很多线只能充电）；
- 按住 BOOT，点按 RST，松开 BOOT，再刷新；
- 设备管理器里应出现 `USB-Serial/JTAG`；仍无端口时检查 Type-C 焊接与 USB 供电。

## OTA

面板“升级”或 `POST /api/ota`（`application/octet-stream`，body 为 app bin）：

- 约 6 秒，写入另一个 OTA 槽后切换启动；
- 会先停止发射/收听等运行态，失败不会破坏当前运行镜像；
- 升级后设备重启，`/api/status.build.sha256` 应等于新 ELF 的 SHA256。

APP 也提供升级入口，流程相同。

## 回退

- 固件**没有启用 OTA 自动回滚**（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 未开启），新镜像启动失败不会自动切回；
- 保留上一版 app bin，出现问题时用 OTA 或 USB 重新写入；
- OTA 双槽意味着上一版通常还在另一个槽里，但没有切换接口，最稳妥仍是保留 BIN 文件；
- 跨大版本降级前确认 NVS schema 兼容；不确定时用“全新刷机”，再重新配网。

## 常见错误

| 现象 | 原因 | 处理 |
|---|---|---|
| `这是 OTA 应用 BIN` | 选了 app bin 当完整镜像 | 选 `full.bin` |
| `固件分区布局与此手台不匹配` | 固件不是本板布局，或分区表被改过 | 换正确固件；确认后再考虑全新刷机 |
| `目录名无效`（旧版工具） | `COM1`–`COM9` 被当作 Windows 保留设备名 | 升级到 1.1.3+ |
| OTA 后配置丢失 | 上传了 full.bin 或做了全新刷机 | 用保留配置模式重刷，重新配网 |
| 刷完无法启动 | 供电不足 / 镜像损坏 / Flash 模式不对 | 重新下载并校验 SHA256，换 USB 口与线 |
