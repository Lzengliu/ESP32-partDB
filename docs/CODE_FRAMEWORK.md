# 代码框架说明

版本：V1.11

## 启动流程

入口为 `firmware/main/app_main.c`。启动时依次初始化 NVS/配置、PSRAM、TF 卡、显示与启动画面、触摸、NFC、相机、WiFi、HTTP 服务和设备 UI。若当前镜像处于 OTA 待验证状态，只有 WiFi、HTTP 与 UI 等核心服务成功启动后才确认镜像有效。

```text
app_main.c
  app_config_load()
  psram_diag_run()
  storage_sd_init()
  display_ili9488_init()
  touch_ft6336_init()
  nfc_service_start()
  camera_mgr_init()
  wifi_portal_start()
  http_server_start()
  device_ui_start()
  runtime_guard_start()
  confirm_pending_ota()
```

## 模块边界

- `board_config.h`：板级 GPIO、资源路径、作者与仓库信息。
- `app_config.c/.h`：NVS 配置默认值、加载、校验和保存。
- `display_ili9488.c/.h`：SPI 显示、背光和绘图基础接口。
- `touch_ft6336.c/.h`、`soft_i2c.c/.h`：触摸读取、映射与总线恢复。
- `device_ui.c/.h`：页面状态机、搜索、详情、库存、NFC 和扫码工作流。
- `ui_font.c/.h`：8/12/16 px 内置字模渲染。
- `wifi_portal.c/.h`：STA/AP 生命周期和联网状态。
- `http_server.c/.h`：内嵌 Web 页与 HTTP API。
- `partdb_client.c/.h`：Part-DB HTTP 请求、缓存和库存写回。
- `storage_sd.c/.h`：TF 挂载、格式化、目录和文件访问。
- `camera_mgr.c/.h`、`qr_scanner.c/.h`：相机帧、ZXing-C++ 与 quirc 解码。
- `nfc_i2c.c/.h`、`nfc_pn532.c/.h`、`nfc_service.c/.h`：PN532 传输、NDEF 和后台服务。
- `peripheral_arbiter.c/.h`：相机与 NFC 等耗时外设操作互斥。
- `hardware_diag.c/.h`、`psram_diag.c/.h`：硬件状态与诊断。

## 第三方组件

- `firmware/main/idf_component.yml` 声明 ESP-IDF 管理组件。
- `firmware/dependencies.lock` 锁定组件版本。
- `firmware/components/zxing_qr` 只编译 `third_party/zxing-cpp` 中扫码所需源码。
- `firmware/managed_components` 是本地构建缓存，不进入源码包。

## 存储布局

`firmware/partitions.csv` 定义 NVS、OTA 数据、factory、两个 OTA 应用区、coredump、fontfs 和 spiffs。V1.1 的三个应用区均为 `0x480000`，与 V1.0 不兼容，因此首次升级必须刷写完整合并镜像。

TF 卡用于缓存和用户资源，不是关键数据唯一存储位置。上传路径在服务端做相对路径校验，失败上传会清理零长度或不完整文件。
