# 引用代码与第三方组件说明

版本：V1.0

## ESP-IDF

本固件基于 Espressif ESP-IDF 构建。

- 构建目标：ESP32-S3
- V1.0 实测构建环境：ESP-IDF v5.5.2
- 协议：ESP-IDF 各组件按其原始协议发布，主体通常为 Apache-2.0
- 获取方式：用户本机安装 ESP-IDF，不在本仓库中提交 ESP-IDF 源码

## ESP-IDF Component Manager 依赖

`firmware/main/idf_component.yml` 直接声明：

```yaml
dependencies:
  espressif/esp32-camera: "^2.0.15"
  espressif/quirc: "^1.2.0"
```

`firmware/dependencies.lock` 当前锁定：

| 组件 | 版本 | 用途 | 协议 |
| --- | --- | --- | --- |
| `espressif/esp32-camera` | 2.1.7 | ESP32 摄像头驱动 | Apache-2.0 |
| `espressif/esp_jpeg` | 1.3.1 | JPEG 解码依赖 | Apache-2.0 |
| `espressif/quirc` | 1.2.0 | 二维码识别 | ISC |

建议 GitHub 源码仓库保留 `dependencies.lock`，但不要提交 `firmware/managed_components/`，让 ESP-IDF Component Manager 在构建时下载。

## quirc

quirc 是二维码识别库，上游作者为 Daniel Beer。

本项目通过 `espressif/quirc` 使用 quirc，用于摄像头扫码识别。quirc 原始协议为 ISC license，应保留其版权和许可声明。

## GNU Unifont 派生字模

`firmware/main/calendar_font_data.h` 文件头标注：

```text
Source: GNU Unifont 16.0.02
```

该文件是从 GNU Unifont 生成的点阵字模数据。它不是项目自有 Apache-2.0 代码。发布时应把它作为第三方字体数据处理，并保留来源说明。

建议发布前复核：

- 是否保留该内置字模。
- 是否需要在仓库中附带 GNU Unifont 的完整许可文本。
- 是否改为使用项目自绘的小字符集，减少字体协议复杂度。

## Part-DB

Part-DB 是外部元器件管理系统。本固件仅通过 HTTP API 对接 Part-DB，不包含 Part-DB 服务端代码。

本工作区中的 `Part-DB-server-master/` 属于本地参考资料，不应纳入本项目源码包。公开仓库中只需要说明兼容的 API 路径和配置方法。

## 硬件资料和供应商示例

本地工作区中包含屏幕、PN532、ESP32-S3 CAM 等硬件资料和供应商示例。这些资料用于开发参考，不建议直接上传到本项目 GitHub 源码仓库。

建议只在 README 中列出硬件型号和接线说明，必要时链接到厂商公开页面或数据手册原始来源。

## 自有实现

以下模块为本项目自有实现或集成代码：

- Part-DB HTTP 客户端和搜索/详情/库存逻辑。
- ILI9488 SPI 显示驱动。
- FT6336 触摸读取和坐标映射。
- PN532 I2C/NDEF 文本读写尝试实现。
- 设备端触摸 UI 和软键盘。
- Web 管理页和 HTTP API。
- TF 卡资源管理。
- 硬件诊断与状态接口。
