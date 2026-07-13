# ESP32 Part-DB Terminal V1.1

## 简介

V1.1 完成了 PSRAM/分区布局、触摸与 NFC 总线、设备 UI、本地二维码识别、Web 资源管理和 OTA 安全确认等大幅更新。

- 作者：灵异大队长
- 项目地址：https://github.com/Lzengliu/ESP32-partDB

## 主要更新

- 启用 8 MB Octal PSRAM，应用分区扩大到 4.5 MiB。
- 重构设备 UI，增加资源管理、自动息屏和更完整的诊断状态。
- PN532 使用独立硬件 I2C，增加后台读卡和 NDEF 读写/清除。
- 二维码改为 ESP32 本地 ZXing-C++ 优先、quirc 回退。
- 增加 Part-DB 缓存、条码/IPN/Lot 路由和库存工作流改进。
- 修复 OTA rollback 镜像未确认、失败上传残留文件、默认设备密钥未持久化等问题。
- Web 项目声明补齐作者和开源地址。

## 下载

- `esp32_partdb_terminal_v1.1_firmware.zip`：完整固件包。
- `esp32_partdb_terminal_v1.1_source.zip`：清理后的源码包。
- `esp32_partdb_terminal_v1.1_merged.bin`：完整刷写镜像。
- `esp32_partdb_terminal_v1.1_ota.bin`：相同分区布局的 OTA 应用镜像。
- `SHA256SUMS`：SHA-256 校验和。

## 从 V1.0 升级

V1.1 修改了分区表。第一次升级必须将 merged 镜像刷到 `0x0`，不能在 V1.0 Web 页面直接上传 OTA 文件：

```sh
python -m esptool --chip esp32s3 -b 460800 write_flash \
  0x0 esp32_partdb_terminal_v1.1_merged.bin
```

## 已知限制

- 当前相机无 AF，需要手动旋转镜头调焦。
- Web 管理接口没有认证，请仅用于可信局域网。
- 外部字体运行时渲染、开机动画逐帧播放、详情长文本滚动和多批次选择仍待完善。

完整刷写说明、V1.0 对比、许可证和已知问题都包含在源码包及固件包文档中。
