# ESP32-partDB V1.11

V1.11 是基于 V1.1 分区版本的 OTA 功能与稳定性更新，重点改善二维码扫描速度、可选自动对焦硬件支持和设备长期运行。

## 本次更新

- 二维码扫描默认使用 VGA 灰度快速路径，仅在检测到二维码轮廓但解码失败时条件式升级到 SVGA。
- 复用亮度缓冲、quirc 解码上下文和相同相机配置，连续扫码无需重复初始化和预热。
- 移除屏幕扫码前低分辨率预解码，避免无效等待；实机稳定快速路径完整响应约 150 ms。
- 设备相机页和 Web 管理页新增独立“AF 对焦”按钮，只在用户点击后执行，启动、预览和扫码不会自动触发 AF。
- 启用 OV5640 手动模式 AF 驱动；实机首次加载约 6.3 秒，同一相机会话再次对焦约 1.0 秒。
- 新增长期运行维护任务：监控内部 RAM/PSRAM、回收闲置扫码缓存和相机、定期检查堆完整性，不使用周期性重启掩盖问题。
- Web 相机预览、上传、扫码与设备端预览增加占用保护，避免并发重配摄像头。
- 更新中英文 README、功能说明和 AF 镜头兼容性说明。

## OTA 升级

本 Release 只提供 OTA 应用镜像。设备必须已经安装过 V1.1 的完整固件和新分区表，才能在 Web 管理页上传本次 OTA 文件。

从 V1.0 或更早分区直接升级时，请先安装 V1.1 完整 merged 固件，不能直接上传 V1.11 OTA。

## 注意

OV5640 传感器被驱动识别为支持 AF，不等于镜头模组一定包含音圈马达。固定焦或无执行器镜头仍需手动旋转调焦。

---

V1.11 is an OTA feature and stability update built on the V1.1 partition layout. It focuses on faster QR scanning, optional autofocus hardware support, and long-running reliability.

## Changes

- Added a VGA grayscale fast path with conditional SVGA retry only after QR geometry is detected.
- Reused luma buffers, the quirc context, and matching camera configurations so consecutive scans skip redundant initialization and warm-up.
- Removed the low-resolution pre-decode step from on-device scanning; the validated warm fast path completed the full response in about 150 ms.
- Added separate AF buttons to the on-device camera page and web interface. AF runs only after an explicit click and never during startup, preview, or scanning.
- Enabled manual-mode OV5640 AF support. Hardware validation measured about 6.3 seconds for the first firmware load and about 1.0 second for a repeated AF request in the same camera session.
- Added a runtime guard that monitors internal RAM and PSRAM, releases idle scanner/camera resources, and periodically checks heap integrity without scheduled reboots.
- Added camera ownership checks across web preview, upload, scan, and on-device preview operations.
- Updated Chinese and English documentation and AF lens compatibility guidance.

## OTA Upgrade

This Release contains only the OTA application image. The device must already run the V1.1 full firmware and partition table before uploading the V1.11 OTA file through the web management interface.

Devices using the V1.0 or earlier partition layout must install the V1.1 merged image first and cannot upgrade directly with this OTA file.

## Note

Driver detection of an OV5640 sensor does not prove that the complete lens assembly includes a voice-coil actuator. Fixed-focus or actuator-free modules still require physical lens adjustment.
