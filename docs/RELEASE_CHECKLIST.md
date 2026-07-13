# GitHub 开源发布检查清单

版本：V1.1

## 源码

- [x] 作者和仓库地址已写入固件、README、AUTHORS、NOTICE 和开源声明。
- [x] V1.0 历史文档保留，V1.1 差异和 Release 文案单独新增。
- [x] 公开源码范围排除构建缓存、本地资料、参考服务端、密钥和嵌套 `.git`。
- [x] `dependencies.lock` 和 ZXing-C++ 固定提交信息保留。
- [x] 第三方软件与字体许可证声明补齐。
- [x] 检查最终源码包文件清单和敏感信息扫描结果。

## 构建与固件

- [x] ESP-IDF v5.5.2 完整构建通过。
- [x] 应用版本为 `1.1.0`，应用镜像未超出 `0x480000` 分区。
- [x] 重新生成 `merged-binary.bin`。
- [x] 生成 V1.1 固件包、源码包和 `SHA256SUMS`。
- [x] 校验每个发布附件的 SHA-256。

## 实机回归

- [x] 在 ESP32-S3 上刷写完整合并镜像并冷启动。
- [x] `/api/status` 返回 `1.1.0`、作者、仓库地址和硬件状态。
- [x] Web 项目声明显示可点击开源地址。
- [x] 相机预览/扫码、触摸、TF、NFC 和 Part-DB 基础状态无启动回归。
- [x] 使用 OTA 应用镜像升级，确认 `OTA image confirmed valid`。
- [x] OTA 后再次重启，确认没有回滚。

## GitHub

- [ ] 确认目标仓库远端当前分支和需要保留的历史。
- [ ] 使用整理后的 V1.1 源码内容更新仓库。
- [ ] 创建 `v1.1` Release。
- [ ] 上传 firmware zip、source zip、merged bin、OTA bin 与 `SHA256SUMS`。
- [ ] Release 文案采用 `docs/GITHUB_RELEASE_V1.1.md`。
