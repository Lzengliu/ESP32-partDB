# 代码框架说明

版本：V1.0

## 总体结构

本项目是一个 ESP-IDF 单应用工程，入口在 `firmware/main/app_main.c`。启动后按硬件、配置、网络、Web 服务、设备 UI 的顺序初始化。

```text
app_main.c
  app_config_load()
  storage_sd_init()
  display_ili9488_init()
  i2c_bus_init()
  touch_ft6336_init()
  nfc_pn532_init()
  camera_mgr_init()
  wifi_portal_start()
  http_server_start()
  device_ui_start()
```

## 核心模块

### 配置和硬件定义

- `board_config.h`
  - 集中定义屏幕、触摸、NFC、摄像头、TF 卡和按键引脚。
  - V1.0 硬件验证阶段只建议改这个文件里的 GPIO。
- `app_config.c/.h`
  - 使用 NVS 保存 WiFi、Part-DB、屏幕、触摸、资源路径等配置。
  - Web 配置页和设备 UI 共用同一份配置结构。

### 屏幕与触摸

- `display_ili9488.c/.h`
  - SPI 初始化、ILI9488 命令、方向、亮度、基础绘制接口。
- `touch_ft6336.c/.h`
  - FT6336 触摸读取和坐标转换。
- `device_ui.c/.h`
  - 设备端主 UI、搜索列表、详情页、快捷出入库、NFC 写入确认弹窗、底部导航、软键盘。

### 网络与 Web 管理

- `wifi_portal.c/.h`
  - STA/AP 启动和 WiFi 配置入口。
- `http_server.c/.h`
  - Web 管理页面和设备 HTTP API。
  - 包含配置保存、状态查询、硬件诊断、Part-DB 测试、文件上传、资源选择、OTA、NFC/扫码接口。

### Part-DB 对接

- `partdb_client.c/.h`
  - 封装 HTTP GET/PATCH/POST。
  - 自动拼接保存的 Part-DB base URL 和 Bearer Token。
- `device_ui.c`
  - 搜索、详情查询、库存写回逻辑在 UI 侧组织。
  - 搜索列表进入详情页时使用搜索结果里的 Part ID 直接定位，避免二次精准 IPN/条码验证。

### 存储与资源

- `storage_sd.c/.h`
  - TF 卡挂载、目录准备、文件读写、FAT/FAT32 支持。
- Web 资源管理
  - 背景图、开机动画、锁屏背景、字体文件上传。
  - V1.0 中字体上传可保存和选择，但运行时仍以固件内置点阵字体为主。

### 摄像头和二维码

- `camera_mgr.c/.h`
  - 摄像头初始化和 JPEG 帧获取。
- `qr_scanner.c/.h`
  - 使用 quirc 解码二维码。
  - 扫码结果可进入详情页。

### NFC

- `nfc_i2c.c/.h`
  - PN532 I2C 访问适配，可切换硬件 I2C 或软件 I2C。
- `nfc_pn532.c/.h`
  - PN532 初始化、读卡、写 NDEF 文本。
- `nfc_service.c/.h`
  - 周期读卡服务、状态缓存、与摄像头等外设的互斥。

V1.0 的 NFC UI 和接口已经接入，但当前实机状态仍可能显示 `ESP_ERR_NOT_FOUND`，详见 `KNOWN_ISSUES.md`。

## 构建配置

- `firmware/sdkconfig.defaults`
  - ESP32-S3 目标、16MB Flash、自定义分区、HTTPD 参数、FATFS 长文件名等。
- `firmware/partitions.csv`
  - factory/ota_0/ota_1 各 3MB。
  - 预留 coredump、fontfs、spiffs 区域。
- `firmware/dependencies.lock`
  - 锁定 ESP-IDF 管理组件版本，便于 V1.0 复现。

## 设备端 UI 页面

- Home：库存概览、搜索入口、状态摘要。
- Results：模糊搜索结果列表。
- Detail：元器件详情、参数/备注展示、出入库和写 NFC。
- Shortcuts：入库、出库、扫码、NFC 快捷入口。
- Info：网络、硬件、Part-DB、TF 卡状态。
- Settings：亮度、AP、资源选择、硬件诊断等。
