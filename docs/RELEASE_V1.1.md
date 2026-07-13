# ESP32 Part-DB Terminal V1.1 发布说明

[English Version](RELEASE_V1.1_EN.md)

发布日期：2026-07-14

- 作者：灵异大队长
- 项目地址：https://github.com/Lzengliu/ESP32-partDB

## 版本定位

V1.1 是一次较大规模的稳定性和功能更新，重点包括 PSRAM/分区调整、设备 UI 重构、PN532 后台服务、本地 ZXing-C++ 扫码、TF 资源管理、硬件诊断和 OTA 回滚确认。

完整变更见 `CHANGES_V1.0_TO_V1.1.md`，当前限制见 `KNOWN_ISSUES.md`。

## 首次启动与 AP 管理地址

首次刷入固件或尚未配置 WiFi 时，连接设备默认 AP：

- AP 名称（SSID）：`PartDB-Terminal`
- AP 密码：`partdb1234`
- Web 管理地址：`http://192.168.4.1/`

连接后在浏览器打开管理地址，即可配置 WiFi、Part-DB 和其他设备选项。

## 构建环境

- ESP-IDF：v5.5.2
- Target：ESP32-S3
- Flash：16 MB，DIO，40 MHz
- PSRAM：8 MB Octal
- 应用版本：`1.1.0`
- 分区表：`firmware/partitions.csv`

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py merge-bin
```

## V1.0 升级警告

V1.1 修改了分区表。第一次从 V1.0 升级时必须擦写/覆盖包含新分区表的完整合并镜像：

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 esp32_partdb_terminal_v1.1_merged.bin
```

不要在 V1.0 Web 管理页直接上传 V1.1 OTA 应用镜像。安装 V1.1 分区表以后，后续相同布局版本才可以通过 Web OTA 更新。

## 固件文件

| 文件 | 用途 | 偏移 |
| --- | --- | --- |
| `esp32_partdb_terminal_v1.1_merged.bin` | 完整首次刷写/跨分区升级 | `0x0` |
| `bootloader.bin` | 分文件刷写 | `0x0` |
| `partition-table.bin` | V1.1 分区表 | `0x8000` |
| `ota_data_initial.bin` | OTA 初始状态 | `0xf000` |
| `esp32_partdb_terminal_v1.1_ota.bin` | 应用/相同布局 OTA | `0x20000`（手动刷写时） |

分文件刷写：

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 esp32_partdb_terminal_v1.1_ota.bin
```

## Release 附件

- `esp32_partdb_terminal_v1.1_firmware.zip`
- `esp32_partdb_terminal_v1.1_source.zip`
- `esp32_partdb_terminal_v1.1_merged.bin`
- `esp32_partdb_terminal_v1.1_ota.bin`
- `SHA256SUMS`

下载后在附件所在目录执行：

```sh
shasum -a 256 -c SHA256SUMS
```

## 已知限制

- 相机模组无 AF，二维码扫描前需要手动调焦。
- 摄像头按需唤醒；未执行预览/扫码预热时，Web 后台显示相机异常属于正常状态。
- Web 管理页/API 无认证，只适合可信局域网。
- 上传字体尚未接入运行时栅格化。
- GIF/WebP 开机动画尚未逐帧播放。
- 长文本滚动和多批次选择仍需完善。

## 发布验证

- 完整 merged 镜像从 `0x0` 刷写并校验成功，新分区表从 factory 启动。
- `/api/status` 返回版本 `1.1.0`、作者、仓库地址及正常硬件状态。
- ILI9488、FT6336、8 MB PSRAM、TF 卡、PN532 1.6、WiFi、HTTP 和 UI 实机启动正常。
- 手动调焦后，OV5640 + ZXing-C++ 成功识别 800 x 600 QR 图像。
- 12 个并发状态请求全部返回 HTTP 200，没有再次出现 socket error 23。
- Web OTA 成功写入 `ota_0`，打印 `OTA image confirmed valid`；再次重启仍从 `ota_0` 启动，无回滚。

## 许可证

项目自有代码使用 Apache License 2.0。发布包同时保留第三方软件和 OFL 字体数据声明，见 `NOTICE.md`、`THIRD_PARTY_CODE.md` 和 `OFL-1.1.txt`。
