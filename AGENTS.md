# 黑猫互联开发入口

当前主线：固件 r11-93、APP 1.2.18（versionCode 30）、独立刷机工具1.1.4。
用户当前指令优先；上层工作区如提供接手主本，应先完整阅读。公开仓库不保存内部交接主本、实机快照或个人绝对路径。

- 先读README、docs/build及docs/release.md；实现说明为docs/resource-profile-panel-r11-93.md。
- 保留已有工作区更改。默认不刷机、不真实键控、不清NVS、不推送远端；按用户授权范围执行。
- 刷机工具必须不内嵌整机固件。使用flasher/build.ps1，FixtureBin/AppBin仅供离线测试，不复制进资源目录。
- 发布前运行实际EXE的--diagnose（空目录及显式外部BIN）和flasher/verify_bundle.py，确认embedded_firmware=false、资源仅品牌图片。
- 固件修改保留16对端、监听分流、固定接收保护与56字节配置布局；源码和交付二进制的构建提交分别记录，不将未重编HEAD写成固件编译输入。
- 共享SDK不可直接修改。HTTPS修复由tools/patch_https_server.py校验规范换行后的原源SHA并生成构建副本；修改SDK版本要复核。
- 重型构建串行；不随意set-target/fullclean或改sdkconfig。HTTP单函数帧上限6144字节。
- 主机测试入口tools/run_host_tests.py；实际SDK清理测试tools/verify_https_cleanup.py；浏览器测试tools/verify_resource_panel.cjs。
- 不提交证书私钥、签名密钥、本机配置、构建缓存、设备身份/地址或完整Flash/NVS；发布二进制放在Release资产而非Git源树。
- 交付先在新暂存目录完成清单、ZIP与sidecar校验，再切换正式目录；历史版本不覆盖。
