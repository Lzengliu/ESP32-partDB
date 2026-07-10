# 开源协议与代码声明

版本：V1.0

## 开源范围

本次准备开源的主体是 `firmware/` 下的 ESP32-S3 固件工程，以及仓库根目录和 `docs/` 下的项目文档。

建议上传到 GitHub 的源码范围：

- `README.md`
- `LICENSE`
- `NOTICE.md`
- `.gitignore`
- `docs/`
- `firmware/CMakeLists.txt`
- `firmware/README.md`
- `firmware/sdkconfig.defaults`
- `firmware/sdkconfig`
- `firmware/partitions.csv`
- `firmware/dependencies.lock`
- `firmware/docs/`
- `firmware/main/`

不建议直接上传到 GitHub 源码仓库的本地资料：

- `firmware/build/`
- `firmware/managed_components/`
- `backups/`
- `硬件资料/`
- `Part-DB-server-master/`
- 其他本地历史包、临时包和供应商原始资料

其中 `firmware/build/` 的 V1.0 成品固件会单独整理为 GitHub Release 下载附件。

## 项目自有代码协议

项目自有代码建议按 Apache License 2.0 发布：

```text
Copyright 2026 ESP32 Part-DB Terminal Project contributors

Licensed under the Apache License, Version 2.0.
```

根目录 `LICENSE` 已放入 Apache License 2.0 全文。

## 第三方代码和资源边界

Apache-2.0 只覆盖项目自有代码，不覆盖第三方代码和第三方资源。第三方依赖应按其原始协议保留声明。

已识别的第三方依赖：

- ESP-IDF：Espressif Systems，主要为 Apache-2.0。
- `espressif/esp32-camera`：Apache-2.0。
- `espressif/esp_jpeg`：Apache-2.0。
- `espressif/quirc` / upstream quirc：ISC license。
- `firmware/main/calendar_font_data.h`：由 GNU Unifont 16.0.02 生成的字模数据，需按 GNU Unifont 原始协议和字体例外处理。
- Part-DB：外部系统，本固件只通过 HTTP API 对接，不打包 Part-DB 服务端源码。

## 发布前需要确认

- 是否把版权主体从 `ESP32 Part-DB Terminal Project contributors` 改成个人、公司或组织名称。
- 是否保留 `firmware/main/calendar_font_data.h` 中的 GNU Unifont 派生字模；如果保留，应在 Release 和源码中保留第三方声明。
- 是否把 `firmware/sdkconfig` 一并提交。它有利于复现 V1.0 构建，但以后维护时可考虑只保留 `sdkconfig.defaults`。
- GitHub Release 附件需要包含 `NOTICE.md`、`docs/RELEASE_V1.0.md` 和校验和。

## 非法律意见

本文件是工程发布准备说明，不构成法律意见。正式公开前，如果项目会商业分发或随硬件销售，建议由版权负责人复核第三方协议边界。
