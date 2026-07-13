# 引用代码与第三方组件说明

版本：V1.1

## ESP-IDF 与管理组件

V1.1 使用 ESP-IDF v5.5.2，目标为 ESP32-S3。`firmware/dependencies.lock` 锁定：

| 组件 | 版本 | 用途 | 协议 |
| --- | --- | --- | --- |
| ESP-IDF | 5.5.2 | 框架与芯片支持 | 各组件原始协议，主体 Apache-2.0 |
| `espressif/esp32-camera` | 2.1.7 | 相机驱动 | Apache-2.0 |
| `espressif/esp_jpeg` | 1.3.1 | JPEG 解码 | Apache-2.0 |
| `espressif/quirc` | 1.2.0 | QR 回退解码 | ISC |

源码包不提交 `firmware/managed_components/`；ESP-IDF Component Manager 在首次构建时按锁文件获取它们。

## ZXing-C++

- 上游：https://github.com/zxing-cpp/zxing-cpp
- 固定提交：`6c2961d2a9ea4bc4e4ae8f37b1497299f04dd861`
- 协议：Apache License 2.0
- 本地路径：`third_party/zxing-cpp`

`firmware/components/zxing_qr` 只编译 QR Code reader 所需的稀疏源码。V1.1 本地扫码先调用 ZXing-C++，未识别时回退到 quirc。上游 LICENSE 随源码包保留。

## 生成字模

`firmware/main/calendar_font_data.h` 的 16 px 字模来自 GNU Unifont 16.0.02。

`firmware/main/ui_font_sizes_data.h` 的 8/12 px 字模主要来自：

- 江城斜黑体 200W / JiangChengXieHei 200W，Version 2.0
- 字体作者：Liu Peng / 刘鹏
- 原始字体元数据协议：SIL Open Font License 1.1
- 缺字回退：GNU Unifont

本项目按 SIL OFL 1.1 分发生成后的字体数据，完整文本在 `docs/licenses/OFL-1.1.txt`。这些字模不适用项目 Apache-2.0 许可证。字体生成脚本引用的本地 TTF 和 Unifont 源文件不随公开源码包发布。

## Part-DB

Part-DB 是独立上游项目：https://github.com/Part-DB/Part-DB-server 。本固件只调用其 HTTP API，不打包服务端源码。

## 本项目自有实现

Part-DB 客户端与工作流、ILI9488 显示、FT6336 触摸适配、PN532/NDEF 服务、设备 UI、Web 管理页、TF 资源管理、外设仲裁和硬件诊断属于本项目自有实现，并按根目录 Apache-2.0 许可证发布。
