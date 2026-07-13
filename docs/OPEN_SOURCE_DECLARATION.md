# 开源协议与代码声明

版本：V1.11

## 项目信息

- 项目：ESP32-partDB / ESP32 Part-DB Terminal
- 作者与维护者：灵异大队长
- 开源地址：https://github.com/Lzengliu/ESP32-partDB
- 项目自有代码协议：Apache License 2.0

```text
Copyright 2026 灵异大队长

Licensed under the Apache License, Version 2.0.
```

根目录 `LICENSE` 是 Apache License 2.0 全文，`AUTHORS.md` 和 `NOTICE.md` 记录作者、仓库与第三方边界。

## 公开源码范围

V1.1 源码包包含：

- 根目录 README、LICENSE、NOTICE、AUTHORS 和 `.gitignore`
- `docs/`
- `firmware/`，不含 `build/` 与 `managed_components/`
- `third_party/zxing-cpp/` 精简源码与许可证，不含其嵌套 `.git/`
- `tools/`，不含 Python 缓存

本地历史包、硬件资料、Part-DB 服务端参考源码、供应商归档、密钥、构建缓存和编辑器配置不进入公开源码包。

## 第三方边界

Apache-2.0 只覆盖项目自有代码。ESP-IDF 管理组件、ZXing-C++、quirc、GNU Unifont 和江城斜黑体继续按其原始协议分发，详见 `NOTICE.md` 与 `docs/THIRD_PARTY_CODE.md`。生成字模使用 SIL Open Font License 1.1，许可证全文见 `docs/licenses/OFL-1.1.txt`。

Part-DB 是独立外部系统，本项目仅通过 HTTP API 对接，不包含 Part-DB 服务端源码。

## 固件发布

Release 固件包应同时包含 LICENSE、NOTICE、AUTHORS、OFL 文本、V1.1 发布说明、完整合并镜像、OTA 应用镜像、分区刷写文件与 SHA-256 校验和。

本文件是工程发布说明，不构成法律意见。
