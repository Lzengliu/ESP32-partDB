# ESP32-partDB

ESP32-partDB 是围绕 Part-DB 元器件仓库开发的 ESP32-S3 硬件终端。它用于现场查询元器件、查看库存资料、扫码、出入库、写入 NFC 标签和管理终端资源。

Part-DB 官方仓库：

- https://github.com/Part-DB/Part-DB-server

详细硬件运行逻辑、IO 引脚和后台功能见：

- [ESP32-partDB V1.0 项目说明](docs/PROJECT_BRIEF_V1.0.md)
- [ESP32-partDB V1.0 Project Brief](docs/PROJECT_BRIEF_V1.0_EN.md)

## V1.0 定位

- 固件主体：C / ESP-IDF
- 设备端 UI：C 直接绘制到 ILI9488 屏幕
- 后台服务：ESP32 本机 HTTP Server
- 后台页面：内嵌 HTML/CSS/JavaScript
- 外部系统：Part-DB，通过 HTTP API 对接

## 主要硬件

- ESP32-S3 主控
- ILI9488 SPI 触摸屏
- FT6336 触摸控制器
- PN532 NFC 模块
- TF 卡，SDMMC 1-bit
- ESP32-S3 摄像头，用于二维码扫描
- 支持在线 OTA 升级，后期除非有重大更新会发布整包，其余版本都将只发布 OTA 固件包

## 主要功能

- Part-DB 地址和 API Token 配置
- 元器件模糊搜索
- 搜索结果直达详情页
- 元器件基础信息、简介、备注、参数、库存展示
- 入库、出库写回 Part-DB
- 摄像头扫码
- NFC 读写流程
- Web 后台配置、诊断、资源上传、OTA
- V1.0 只支持固件内置中文字库；上传字体文件暂不参与运行时渲染

## 构建

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

V1.0 实测构建环境为 ESP-IDF v5.5.2。

## 刷写

推荐：

```sh
cd firmware
idf.py -p PORT flash monitor
```

GitHub Release 中的合并固件可直接刷写到 `0x0`：

```sh
python -m esptool --chip esp32s3 -b 460800 write_flash \
  0x0 esp32_partdb_terminal_v1.0_merged.bin
```

## 当前限制

- NFC 在当前硬件上仍可能离线，常见错误为 `ESP_ERR_NOT_FOUND`。
- 外部字体上传可以保存文件，但 V1.0 未实现 TTF/OTF 实时渲染。
- 详情页长文本仍可能被小屏空间截断。
- 多批次元器件还没有完整批次选择页。

## 开源协议

项目自有代码按 Apache License 2.0 发布。第三方依赖和字体数据按其原始协议保留声明，见 [NOTICE.md](NOTICE.md)。
