# GitHub Release 文案草稿：V1.0

## 标题

ESP32 Part-DB Terminal V1.0

## 简介

V1.0 是 ESP32 Part-DB Terminal 的首次开源准备版本。该固件面向 ESP32-S3 + ILI9488 触摸屏终端，可连接 Part-DB API，实现元器件搜索、详情查看、快捷出入库、扫码、资源管理和 Web 配置。

Part-DB 官方仓库：https://github.com/Part-DB/Part-DB-server

## 下载

- `esp32_partdb_terminal_v1.0_firmware.zip`：成品固件包，包含合并固件和分区刷写文件。
- `esp32_partdb_terminal_v1.0_source.zip`：清理后的源码包。
- `SHA256SUMS`：校验和。

## 快速刷写

合并固件：

```sh
python -m esptool --chip esp32s3 -b 460800 write_flash \
  0x0 esp32_partdb_terminal_v1.0_merged.bin
```

分区刷写：

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 esp32_partdb_terminal.bin
```

## 主要功能

- Part-DB API 配置、测试、搜索和详情查询。
- 设备端触摸 UI：主页、搜索结果、详情、快捷、信息、设置。
- 元器件详情展示：基础信息、简介、备注、参数、库位、条码、库存。
- 详情页出入库操作。
- TF 卡资源管理和 Web 文件上传。
- 摄像头二维码扫描。
- OTA 上传和硬件诊断。

## 已知问题

- NFC 在当前硬件上仍可能离线，常见错误为 `ESP_ERR_NOT_FOUND`。
- 外部字体上传已支持反馈和保存，但运行时未解析 TTF/OTF。
- 小屏详情页长文本仍可能被截断。
- 多批次库存选择流程仍需增强。

详细说明见源码包中的 `docs/KNOWN_ISSUES.md`。
