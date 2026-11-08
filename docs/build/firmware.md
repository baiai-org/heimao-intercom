# 构建固件

## 环境

| 项 | 要求 |
|---|---|
| ESP-IDF | **5.4.1**（其他版本未验证） |
| 目标芯片 | `esp32s3`，ESP32-S3-WROOM-1 **N16R8**（16 MB Flash / 8 MB PSRAM） |
| Python | ESP-IDF 自带的 Python 3.11 环境 |
| 依赖组件 | `esphome/micro-opus`（由 Component Manager 下载） |

IDF 安装与环境变量配置见官方文档；本项目不提交工具链（`.idf/`、`.tools/` 均在 `.gitignore` 中）。

## 构建

```bash
cd firmware
idf.py build
```

版本串来自 `firmware/VERSION`（当前 `r11-83`），由 CMake 在 `project()` 之前读入，因此源码压缩包/干净克隆也能得到确定的版本号，不依赖 `git describe`。发版时修改该文件。

预期产物：

| 文件 | 说明 |
|---|---|
| `firmware/build/openradiolink.bin` | 应用镜像（OTA 用） |
| `firmware/build/bootloader/bootloader.bin` | bootloader |
| `firmware/build/partition_table/partition-table.bin` | 分区表 |
| `firmware/build/ota_data_initial.bin` | OTA 选择初始数据 |

### 关于 `sdkconfig`

`firmware/sdkconfig` 是**手工维护的源码**，不是构建产物。不要用 `idf.py menuconfig` 或 `idf.py set-target` 覆盖它。

- 新克隆的仓库已固定 `CONFIG_IDF_TARGET="esp32s3"`，直接 `idf.py build` 即可；
- 确实需要改配置时，直接编辑 `sdkconfig` 并在 PR 中说明原因与影响。

### HTTPS 证书

固件内嵌自签证书用于 HTTPS（浏览器麦克风需要安全上下文）。证书**不提交**：

- `firmware/main/CMakeLists.txt` 在配置阶段检测 `servercert.pem` / `serverkey.pem`，缺失时调用 `tools/gen_dev_cert.py` 生成；
- 也可以手动生成：

```bash
python tools/gen_dev_cert.py --out firmware/main --cn orl.local
```

生成的文件在 `.gitignore` 中，每台设备/每次构建可以不同。浏览器首次访问会出现证书告警，接受即可。

## 合并完整 BIN

USB 刷机需要 0x0 起始的合并镜像。用刷机工具自带的打包脚本（需要 `esptool`，见 [刷机工具构建](flasher.md)）：

```bash
python flasher/make_firmware.py --output dist
```

产物 `dist/黑猫互联-full-r11-83.bin` 包含 bootloader + 分区表 + OTA 初始数据 + 应用；脚本会校验分区布局与本板一致。输出文件名默认取 `flasher/brand.json` 的 `default_bin`（当前带 `r11-83`）；发版时同步修改 `firmware/VERSION` 与 `default_bin`，或用 `--name 黑猫互联-full-<版本>.bin` 显式指定。

## 主机回归

部分协议/网页逻辑可以在 PC 上跑，不需要实板：

```bash
python tools/run_host_tests.py
```

依赖：`gcc`（部分测试编译真实 C 函数）、`node`。脚本直接使用仓库内的源码、网页文件与 `tools/vendor/cJSON`。

> 有 4 个历史测试在旧仓库即失败（测试脚本未跟上生产代码），runner 会把它们标成 `XFAIL` 并继续；新增失败才会返回非零。`--strict` 可把 XFAIL 也当失败。

## 资源约束（改代码前必读）

- HTTP 任务栈 10 KB；`net_http.c` 有 `-Werror=frame-larger-than=6144`，状态响应缓冲走 PSRAM 每请求分配；
- 发射任务使用 6 KB 静态栈，避免内部 RAM 碎片导致 `ESP_ERR_NO_MEM`；
- 不要在 HTTP 处理函数里放大局部数组；改完看 `firmware/build/esp-idf/main/CMakeFiles/__idf_main.dir/net_http.c.su` 的栈报告；
- MQTT、mDNS、ES8311 是本地覆盖组件（`firmware/components/`），修改时保留 Apache-2.0 文件头与 `PATCH.md`。
