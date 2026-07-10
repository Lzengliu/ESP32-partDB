# GitHub 开源发布检查清单

版本：V1.0

## 源码仓库

- [ ] 确认版权主体名称。
- [ ] 确认根目录 `LICENSE`、`NOTICE.md`、`README.md` 已保留。
- [ ] 确认 `docs/` 中的说明不包含私人路径、密钥或内部地址。
- [ ] 确认 `.gitignore` 生效，不上传 `firmware/build/`。
- [ ] 确认不上传本地 `硬件资料/`、`Part-DB-server-master/`、`backups/`。
- [ ] 确认 `firmware/sdkconfig` 是否保留；如不保留，至少保留 `sdkconfig.defaults`。
- [ ] 确认 `firmware/dependencies.lock` 保留，便于复现 V1.0 依赖。

## 固件包

- [ ] 确认 `esp32_partdb_terminal_v1.0_firmware.zip` 内含合并固件。
- [ ] 确认分区刷写文件偏移写在 Release 说明中。
- [ ] 确认 `SHA256SUMS` 已生成。
- [ ] 确认固件已实机启动并返回 `/api/status`。

## 第三方协议

- [ ] 确认 ESP-IDF、esp32-camera、esp_jpeg、quirc 声明已保留。
- [ ] 复核 GNU Unifont 派生字模的发布方式。
- [ ] 确认 Part-DB 服务端源码不混入本项目源码包。

## GitHub Release

- [ ] Release 标题使用 `ESP32 Part-DB Terminal V1.0`。
- [ ] 上传 `esp32_partdb_terminal_v1.0_firmware.zip`。
- [ ] 上传 `esp32_partdb_terminal_v1.0_source.zip`。
- [ ] 上传 `SHA256SUMS`。
- [ ] Release 文案引用 `docs/GITHUB_RELEASE_V1.0.md`。
