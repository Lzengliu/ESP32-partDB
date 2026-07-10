# ESP32-partDB V1.0 项目说明

ESP32-partDB 是围绕 Part-DB 元器件仓库开发的硬件终端。它不是 Part-DB 服务端本身，而是一个运行在 ESP32-S3 上的现场终端，用来快速查询元器件、查看库存资料、扫码、出入库和管理终端资源。

Part-DB 官方仓库：

- GitHub: https://github.com/Part-DB/Part-DB-server
- 文档: https://docs.part-db.de/

## 整体框架和语言

| 部分 | 使用技术/语言 | 说明 |
| --- | --- | --- |
| 固件主体 | C，ESP-IDF | 所有硬件驱动、UI、后台接口、Part-DB 通信逻辑都在 ESP-IDF 工程中实现 |
| 设备端 UI | C | 直接在 ILI9488 屏幕 framebuffer 上绘制页面、按钮、软键盘和弹窗 |
| 后台 Web 服务 | C，ESP-IDF HTTP Server | ESP32 本机启动 HTTP 服务，提供配置页和 API |
| 后台 Web 页面 | HTML/CSS/JavaScript | 以内嵌字符串方式打包在 `http_server.c` 中，不需要额外服务器文件 |
| Part-DB 服务端 | PHP/Symfony | 外部系统，本项目只通过 Part-DB API 访问它 |
| 二维码识别 | C，quirc | 摄像头取图后在 ESP32 本地解码二维码 |

## 固件启动逻辑

1. 初始化 NVS，读取本机配置。
2. 初始化显示屏，显示启动进度。
3. 初始化 Part-DB HTTP 客户端。
4. 启动 WiFi STA/AP。
5. 启动 ESP32 本机 Web 后台。
6. 执行硬件诊断。
7. 启动 NFC 轮询服务。
8. 启动设备端触摸 UI。

入口文件：`firmware/main/app_main.c`

## 硬件运行逻辑

### ESP32-S3 主控

ESP32-S3 负责所有本地逻辑：

- 连接 WiFi。
- 启动 Web 后台。
- 驱动屏幕和触摸。
- 访问 Part-DB API。
- 管理 TF 卡资源。
- 调用摄像头扫码。
- 尝试读写 PN532 NFC。

### ILI9488 屏幕

屏幕通过 SPI 驱动。固件直接绘制页面，不依赖 LVGL。

运行逻辑：

- 启动时显示进度条。
- 正常运行后显示主页、搜索页、详情页、快捷页、信息页、设置页。
- 详情页显示 Part-DB 元器件资料和库存操作。

### 字库显示

V1.0 只支持固件内置点阵中文字库和少量内置符号字模。

运行逻辑：

- UI 文本由 `firmware/main/ui_font.c` 绘制。
- 中文、英文、数字和常用符号使用固件内置字模显示。
- 已额外补充 `Ω`、`°C`、`℃`、`±`、`≤`、`≥` 等常用元器件参数符号。
- Web 后台可以上传字体文件并保存路径，但 V1.0 不解析 TTF/OTF/TTC/WOFF/WOFF2，也不会用上传字体替换运行时渲染。

因此，当前版本可以理解为“只支持内置中文字库”，外部字体上传属于预留配置功能。

### FT6336 触摸

触摸通过硬件 I2C 读取。

运行逻辑：

- 周期读取触摸坐标。
- 根据配置进行旋转、翻转和坐标转换。
- 识别点击和滑动。
- 控制页面切换、搜索框、软键盘、详情页翻页和按钮操作。

### PN532 NFC

NFC 使用 PN532 模块，当前配置为和触摸共用硬件 I2C。

运行逻辑：

- 启动后 NFC 服务周期轮询卡片。
- 读到文本后尝试作为 Part-DB 对象编号、条码、IPN 或链接进入详情页。
- 写 NFC 必须在详情页点击按钮后弹窗确认，再写入 NDEF 文本。

V1.0 遗留问题：当前 PN532 在实机上仍可能显示 `ESP_ERR_NOT_FOUND`，说明 ESP32 和 PN532 的 I2C 通信仍需继续排查。

### TF 卡

TF 卡使用 1-bit SDMMC。

运行逻辑：

- 挂载到 `/sdcard`。
- 保存缓存、背景图、开机动画、锁屏图、字体文件。
- Web 后台可以上传、浏览、删除文件。

### 摄像头

摄像头使用 ESP32-S3 EYE/Freenove 示例引脚。

运行逻辑：

- Web 后台可抓取 JPEG。
- 设备端或后台可触发二维码扫描。
- 扫码结果进入 Part-DB 查询和详情页。

### 物理按键

V1.0 物理按键预留，当前没有配置 GPIO。主要交互通过触摸屏完成。

## IO 引脚表

### 屏幕 ILI9488 SPI

| 信号 | GPIO |
| --- | --- |
| SCLK | GPIO21 |
| MOSI | GPIO47 |
| MISO | NC |
| CS | GPIO14 |
| DC | GPIO2 |
| RST | GPIO1 |
| BL | GPIO42 |

### 触摸 FT6336

| 信号 | GPIO / 地址 |
| --- | --- |
| I2C SDA | GPIO35 |
| I2C SCL | GPIO36 |
| I2C 地址 | `0x38` |
| RST | GPIO37 |
| INT | GPIO41 |

### NFC PN532

| 信号 | GPIO / 地址 |
| --- | --- |
| I2C SDA | GPIO35 |
| I2C SCL | GPIO36 |
| I2C 地址 | `0x24` |
| 软件 I2C | V1.0 未启用 |

### TF 卡 SDMMC 1-bit

| 信号 | GPIO |
| --- | --- |
| CMD | GPIO38 |
| CLK | GPIO39 |
| D0 | GPIO40 |
| D3 | NC |

### 摄像头

| 信号 | GPIO |
| --- | --- |
| XCLK | GPIO15 |
| SIOD | GPIO4 |
| SIOC | GPIO5 |
| D0 | GPIO11 |
| D1 | GPIO9 |
| D2 | GPIO8 |
| D3 | GPIO10 |
| D4 | GPIO12 |
| D5 | GPIO18 |
| D6 | GPIO17 |
| D7 | GPIO16 |
| VSYNC | GPIO6 |
| HREF | GPIO7 |
| PCLK | GPIO13 |
| PWDN | NC |
| RESET | NC |

### 物理按键

| 功能 | GPIO |
| --- | --- |
| UP | NC |
| DOWN | NC |
| OK | NC |
| WAKE | NC |

## 后台可以实现的功能

ESP32 本机后台地址为设备 IP 的根路径，例如：

```text
http://设备IP/
```

后台能力：

- 查看 WiFi、Part-DB、TF 卡、屏幕、触摸、NFC、摄像头、UI 状态。
- 配置设备名称。
- 配置 WiFi/AP。
- 配置 Part-DB 地址和 API Token。
- 测试 Part-DB API。
- 设置屏幕尺寸、方向、翻转、亮度。
- 设置触摸旋转、翻转和坐标范围。
- 执行硬件诊断。
- 查看触摸状态和按键状态。
- 切换设备端 UI 页面。
- 读取 NFC 当前状态。
- 对 Part-DB 对象执行 NFC 读写请求。
- 查看 TF 卡容量和挂载状态。
- 准备 TF 卡目录。
- 格式化 TF 卡。
- 上传、浏览、下载、删除 TF 卡文件。
- 上传和选择字体文件。
- 上传、选择、删除屏幕背景图。
- 上传、选择、删除开机动画。
- 上传、选择、删除锁屏背景图。
- 获取摄像头 JPEG。
- 触发摄像头扫码。
- 上传 OTA 固件并重启。

主要后台 API 在 `firmware/main/http_server.c` 中注册。

## Part-DB 相关功能

设备端：

- 模糊搜索元器件。
- 显示搜索结果列表。
- 点击搜索结果后直接进入详情页。
- 显示名称、IPN、分类、厂家、MPN、封装、库位、条码、库存、批次、简介、备注和参数。
- 输入数量。
- 入库/出库并写回 Part-DB。
- 写入 NFC 标签前弹窗确认。

后台：

- 保存 Part-DB base URL。
- 保存 API Token。
- 发送 Part-DB 测试请求。
- 通过 `/api/partdb/get` 代理查询 Part-DB API。

## V1.0 当前限制

- NFC 硬件通信仍不稳定，可能离线。
- 当前只支持内置中文字库；外部字体上传可以保存，但运行时未解析 TTF/OTF/TTC/WOFF/WOFF2。
- 详情长文本仍可能被屏幕空间截断。
- 多批次元器件还没有完整批次选择页。
