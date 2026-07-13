# ESP32-partDB

ESP32-partDB 是面向 Part-DB 元器件仓库的 ESP32-S3 触摸终端。它支持现场查询元器件、查看库存、扫码、出入库、NFC 标签读写，以及通过 Web 页面管理终端配置和资源。

- 当前稳定版本：V1.1
- 作者：灵异大队长
- 开源地址：https://github.com/Lzengliu/ESP32-partDB
- Part-DB 上游：https://github.com/Part-DB/Part-DB-server

## 主要硬件

- ESP32-S3，16 MB Flash，8 MB Octal PSRAM
- ILI9488 SPI 显示屏与 FT6336 触摸控制器
- PN532 NFC 模块
- SDMMC 1-bit TF 卡
- ESP32-S3 摄像头，用于二维码扫描

## V1.1 功能

- Part-DB 地址和 API Token 配置、搜索、详情与库存写回
- 设备端主页、搜索结果、详情、快捷操作、状态和设置页面
- 摄像头本地二维码识别，ZXing-C++ 优先、quirc 回退
- PN532 后台读卡、NDEF 文本写入和清除
- TF 卡文件、背景、开机图、锁屏图和字体资源管理
- Web 配置、硬件诊断、相机预览、扫码和 OTA
- 8/12/16 px 内置中英文字模与符号字模
- OTA 回滚保护和启动健康确认

完整功能和限制见 [功能说明](docs/FEATURES.md) 与 [已知问题](docs/KNOWN_ISSUES.md)。V1.0 到 V1.1 的逐项差异见 [版本对比](docs/CHANGES_V1.0_TO_V1.1.md)。

## 构建

实测环境为 ESP-IDF v5.5.2：

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py merge-bin
```

首次构建需要下载 `esp32-camera`、`esp_jpeg` 和 `quirc` 管理组件。ZXing-C++ 的精简源码已保存在 `third_party/zxing-cpp/`。

## 从 V1.0 升级

V1.1 修改了分区表。第一次从 V1.0 升级时，必须把 V1.1 合并固件写入 `0x0`，不要使用 V1.0 Web 页只上传应用固件，否则设备仍会使用旧分区表。

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 esp32_partdb_terminal_v1.1_merged.bin
```

安装过 V1.1 分区表后，可使用 Web OTA 上传 `esp32_partdb_terminal_v1.1_ota.bin`。

## 发布文件

- `esp32_partdb_terminal_v1.1_merged.bin`：完整首次刷写/跨分区版本升级镜像
- `esp32_partdb_terminal_v1.1_ota.bin`：仅应用镜像，供相同分区布局的 OTA 使用
- `esp32_partdb_terminal_v1.1_firmware.zip`：完整固件附件
- `esp32_partdb_terminal_v1.1_source.zip`：清理后的公开源码
- `SHA256SUMS`：发布文件 SHA-256 校验和

发布和刷写细节见 [V1.1 发布说明](docs/RELEASE_V1.1.md)。

## 开源协议

项目自有代码由灵异大队长按 Apache License 2.0 发布。第三方组件和生成字模继续使用各自的原始协议，详见 [NOTICE.md](NOTICE.md) 与 [第三方代码说明](docs/THIRD_PARTY_CODE.md)。
