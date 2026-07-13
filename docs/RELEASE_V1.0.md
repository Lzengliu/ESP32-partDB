# ESP32 Part-DB Terminal V1.0 发布说明

发布日期：2026-07-10

## 版本定位

V1.0 是 ESP32 Part-DB Terminal 的首次开源准备版本。目标是提供一个可运行的 ESP32-S3 触摸终端固件，用于连接 Part-DB，完成元器件搜索、详情查看、快捷出入库、资源配置和硬件状态诊断。

## 构建环境

- ESP-IDF：v5.5.2
- Target：ESP32-S3
- Flash：16MB
- 分区表：`firmware/partitions.csv`
- 构建命令：

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py merge-bin
```

本机实测使用 arm64 ninja：

```sh
/usr/bin/arch -arm64 /opt/homebrew/bin/ninja -C build
/usr/bin/arch -arm64 /opt/homebrew/bin/ninja -C build merge-bin
```

## Release 附件建议

建议 GitHub Release 上传以下附件：

- `esp32_partdb_terminal_v1.0_firmware.zip`
  - 分区刷写文件和合并固件。
- `esp32_partdb_terminal_v1.0_source.zip`
  - 清理后的源码和文档。
- `SHA256SUMS`
  - 固件和源码包校验和。

## 固件文件

分区刷写：

| 文件 | 偏移 |
| --- | --- |
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `esp32_partdb_terminal.bin` | `0x20000` |

合并刷写：

| 文件 | 偏移 |
| --- | --- |
| `esp32_partdb_terminal_v1.0_merged.bin` | `0x0` |

## 手动刷写

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

合并固件刷写：

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 esp32_partdb_terminal_v1.0_merged.bin
```

## V1.0 已包含

- WiFi STA/AP 和 Web 配置页。
- Part-DB 地址和 API Token 配置。
- Part-DB 模糊搜索、搜索列表、详情页直达。
- 元器件详情展示、参数/备注/库位/条码/库存信息展示。
- 详情页入库、出库、写 NFC 操作入口。
- 写 NFC 确认弹窗。
- 设备端底部导航和软键盘。
- ILI9488 屏幕和 FT6336 触摸支持。
- TF 卡资源管理、字体/背景/动画上传。
- 摄像头预览抓图和二维码扫描。
- OTA 上传和硬件诊断。

## 已验证

- 固件可编译。
- 固件已刷写到 ESP32-S3 并 hard reset。
- HTTP 状态接口可返回。
- 屏幕和触摸状态正常。
- Part-DB HTTP 测试接口可返回 200。

## 已知问题

- NFC 当前仍可能显示 `ESP_ERR_NOT_FOUND`，需继续验证 PN532 I2C 模式、接线、上拉和上电时序。
- 外部字体上传仅保存文件和路径，运行时未真正解析 TTF/OTF。
- 详情页长文本仍有截断风险。
- 多批次元器件需要更明确的批次选择流程。

详见 `docs/KNOWN_ISSUES.md`。
