# 硬件与固件准备方案

## 当前结论

本固件按 ESP32-S3 + OV 系列 DVP 摄像头 + ILI9488 3.5 寸 SPI 屏 + FT6336 电容触摸 + PN532 NFC + TF 卡设计。

摄像头与 TF 卡使用开发板资料中的 Freenove ESP32-S3 EYE 示例引脚。ILI9488 不使用并口，只使用 4 线 SPI；否则在保留摄像头时 GPIO 不足。当前手工飞线原型上 FT6336 使用低速软件 I2C `GPIO3/GPIO44(RX)`，地址为 `0x38`，`GPIO46` 为触摸复位，触摸 `INT` 暂不接。`GPIO35/GPIO36/GPIO37` 是当前 ESP32-S3R8 板的 Octal PSRAM/MSPI 线，不能再接触摸或 NFC；`GPIO44` 当前作为触摸 SCL 使用。PN532 使用独立硬件 I2C `GPIO43/GPIO41`，避开触摸软件 I2C、LCD SPI 和摄像头 SCCB；摄像头 SCCB 保持在 `GPIO4/GPIO5`。

## 暂定 GPIO 分配

| 功能 | GPIO | 说明 |
| --- | --- | --- |
| CAM XCLK | 15 | Freenove ESP32S3_EYE 示例 |
| CAM SCCB SDA/SCL | 4 / 5 | 摄像头 SCCB |
| CAM D0-D7 | 11, 9, 8, 10, 12, 18, 17, 16 | `esp_camera` DVP 数据线 |
| CAM VSYNC/HREF/PCLK | 6 / 7 / 13 | `esp_camera` 同步线 |
| TF CLK/CMD/D0 | 39 / 38 / 40 | SDMMC 1-bit |
| LCD SCLK/MOSI | 21 / 47 | ILI9488 SPI 写入 |
| LCD CS/DC/RST/BL | 14 / 2 / 1 / 42 | 屏幕控制 |
| Touch SDA/SCL | 3 / 44(RX) | 软件 I2C 总线，当前原型两线触摸 |
| NFC I2C SDA/SCL | 43(TX) / 41 | 独立硬件 I2C，不与触摸、LCD SPI、摄像头共用 |
| Touch RST/INT | 46 / NC | 当前原型复位已接，FT6336 轮询读取 |

> 这是一版可联调引脚表。真实接线前需要对照开发板丝印确认 GPIO 是否引出、是否被板载外设占用，以及 GPIO47/42 是否方便焊接。

## 软件模块

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 配置存储 | `app_config.*` | WiFi、Part-DB API、设备密钥、资源路径 |
| WiFi/AP | `wifi_portal.*` | 初次 AP、STA 连接、状态查询 |
| HTTP 管理端 | `http_server.*` | 配置、OTA、状态、后续 TF 管理页面 |
| 显示 | `display_ili9488.*` | SPI 初始化、清屏、基础绘制；后续接 LVGL |
| 触摸 | `touch_ft6336.*` | 读 FT6336 坐标 |
| NFC | `nfc_pn532.*` | PN532 初始化、UID 读取；后续 NDEF 写入 |
| 摄像头 | `camera_mgr.*` | `esp_camera` 初始化、二维码扫描预览、释放资源 |
| TF 卡 | `storage_sd.*` | SDMMC 挂载、空间查询 |
| Part-DB | `partdb_client.*` | Bearer Token API 调用 |

## Part-DB 集成方向

设备端只保存 Part-DB API 地址和 token。常规数据通过 `/api` 调用；扫码和 NFC 结果复用 Part-DB 已有的标识语义，不单独新增远程 NFC 数据库。

当前标识策略：

- 零件：优先支持 Part-DB 内部码 `P0001`，也支持用户自定义 IPN。
- 批次：支持内部码 `L0001`，并可在需要时写回 `part_lots.user_barcode`。
- 库位：支持内部码 `S0001`。
- NFC 写入 NDEF 文本，隐藏接口先 dry-run，只有 `commit:true` 才会写 Part-DB 或 NFC 卡。

读取 NFC 后，设备按内部码、PartLot 用户条码、Part IPN 的顺序解析，并查询 Part-DB 打开对应元器件、批次或库位流程。

## 字库与 TF 卡

中文字库借鉴“喵哎 ESP32 S3 墨水屏”：

- 固件内置完整 16x16 中文点阵字库，用于启动画面和设备屏幕 UI。
- 独立 `fontfs` 分区预留给后续固件随附 24/32px 中文位图字库。
- TF 卡放可替换字体、开机图、缓存资源和后续 OTA 可利用文件。

Web 管理端需要逐步补齐：

- 查看 TF 卡空间。
- 上传/删除字体和开机图。
- 配置默认字体。
- 配置资源区使用策略。

## 开发顺序

1. 编译通过：固件骨架、NVS、WiFi AP、HTTP 配置页。
2. 外设联调：ILI9488 清屏、FT6336 坐标、PN532 UID、TF 挂载、摄像头扫码预览。
3. UI 接入：LVGL 首页、状态区、摄像头按钮、出入库页面。
4. Part-DB API：统计、搜索、元器件详情、库存修改。
5. NFC/Part-DB 隐藏接口：内部码、IPN、PartLot 用户条码读写与后续出入库跳转。
6. OTA/资源管理完善：固件 OTA、TF 资源管理、字库替换。
