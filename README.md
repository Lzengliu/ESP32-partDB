<h1 align='center'>ESP32-partDB</h1>

<p align='center'><strong>面向 Part-DB 的 ESP32-S3 触摸终端</strong></p>

<p align='center'>元器件查询 · 库存操作 · 二维码扫描 · NFC 标签 · 本地 Web 管理</p>

<p align='center'>
  <img src='https://img.shields.io/badge/release-V1.11-00b8d9?style=flat-square' alt='V1.11'>
  <img src='https://img.shields.io/badge/ESP--IDF-v5.5.2-e7352c?style=flat-square' alt='ESP-IDF v5.5.2'>
  <img src='https://img.shields.io/badge/target-ESP32--S3-111827?style=flat-square' alt='ESP32-S3'>
  <img src='https://img.shields.io/badge/license-Apache--2.0-22a06b?style=flat-square' alt='Apache-2.0'>
</p>

<p align='center'>
  <a href='README_EN.md'><strong>English Version</strong></a> ·
  <a href='https://github.com/Lzengliu/ESP32-partDB/releases/tag/v1.11'>V1.11 Release</a> ·
  <a href='docs/CHANGES_V1.0_TO_V1.1.md'>V1.0 → V1.1 变更</a>
</p>

ESP32-partDB 是运行在 ESP32-S3 上的独立硬件终端，不是 Part-DB 服务端。它面向工作台和元器件仓库现场，提供触摸操作、扫码、NFC、库存读写、资源管理和设备诊断。

- 当前稳定版本：**V1.11**
- 作者：**灵异大队长**
- 开源地址：https://github.com/Lzengliu/ESP32-partDB
- Part-DB 上游：https://github.com/Part-DB/Part-DB-server

## 功能演示

### 终端交互

<table>
  <tr>
    <td width='50%' align='center'><strong>屏幕唤醒与主页</strong><br><sub>自动息屏、触摸唤醒与快捷入口</sub></td>
    <td width='50%' align='center'><strong>键盘、搜索与结果区域</strong><br><sub>触摸输入、结果浏览与详情跳转</sub></td>
  </tr>
  <tr>
    <td align='center' valign='top'><img src='docs/demo/terminal-screen-wake.gif' alt='屏幕唤醒功能演示' width='280'></td>
    <td align='center' valign='top'><img src='docs/demo/terminal-keyboard-search.gif' alt='键盘和搜索区域功能演示' width='480'></td>
  </tr>
  <tr>
    <td align='center'><strong>摄像头二维码扫描</strong><br><sub>本地预览、按键 AF、手动调焦与二维码识别</sub></td>
    <td align='center'><strong>NFC 工作流</strong><br><sub>后台读卡、NDEF 与 Part-DB 对象路由</sub></td>
  </tr>
  <tr>
    <td align='center' valign='top'><img src='docs/demo/terminal-camera-qr.gif' alt='摄像头扫码功能演示' width='480'></td>
    <td align='center' valign='top'><img src='docs/demo/terminal-nfc.gif' alt='NFC 功能演示' width='480'></td>
  </tr>
</table>

### Web 管理后台

<table>
  <tr>
    <td width='50%' align='center'><strong>总览</strong></td>
    <td width='50%' align='center'><strong>设备设置</strong></td>
  </tr>
  <tr>
    <td valign='top'><img src='docs/demo/web-overview.png' alt='Web 管理后台总览'></td>
    <td valign='top'><img src='docs/demo/web-settings.png' alt='Web 管理后台设备设置'></td>
  </tr>
  <tr>
    <td align='center'><strong>Part-DB 接口</strong></td>
    <td align='center'><strong>硬件诊断</strong></td>
  </tr>
  <tr>
    <td valign='top'><img src='docs/demo/web-partdb-settings.png' alt='Part-DB 接口设置'></td>
    <td valign='top'><img src='docs/demo/web-hardware-diagnostics.png' alt='硬件诊断'></td>
  </tr>
  <tr>
    <td align='center'><strong>TF 卡与资源</strong></td>
    <td align='center'><strong>维护与 OTA</strong></td>
  </tr>
  <tr>
    <td valign='top'><img src='docs/demo/web-tf-resources.png' alt='TF 卡资源管理'></td>
    <td valign='top'><img src='docs/demo/web-maintenance.png' alt='维护和 OTA'></td>
  </tr>
</table>

> [!NOTE]
> 截图中后台显示“相机异常”属于正常现象。为降低资源占用，摄像头平时会自动休眠并释放驱动；尚未执行“预览抓图”或“扫码”完成唤醒和预热时，状态检测可能显示异常。发起预览或扫码后，固件会按需初始化并预热摄像头。

## 首次连接

首次刷入固件或尚未配置 WiFi 时，设备会启动默认 AP：

| AP 名称（SSID） | AP 密码 | Web 管理地址 |
| --- | --- | --- |
| `PartDB-Terminal` | `partdb1234` | `http://192.168.4.1/` |

1. 使用电脑或手机连接 `PartDB-Terminal`。
2. 在浏览器打开 `http://192.168.4.1/`。
3. 配置 WiFi、Part-DB 地址、API Token 和其他设备选项。

## 核心能力

| 模块 | 当前源码能力 |
| --- | --- |
| Part-DB | 地址和 API Token 配置、模糊搜索、详情读取、缓存、库存写回、Part/Lot/IPN/条码路由 |
| 设备 UI | 主页、搜索结果、详情、快捷操作、信息、设置、软键盘、亮度和自动息屏 |
| 二维码 | ESP32 本地解码，ZXing-C++ 优先、quirc 回退；VGA 快速路径、条件式 SVGA 重试与解码缓存复用 |
| NFC | PN532 后台读卡、NDEF 文本读取/写入/清除、Part-DB 内容路由 |
| TF 资源 | 文件、背景图、开机图、锁屏图和字体资源管理 |
| Web 与维护 | 配置、状态、硬件诊断、相机预览、仅手动触发 AF、扫码、TF 管理和 OTA |
| 稳定性 | 8 MB PSRAM、扫码互斥、闲置资源回收、低内存恢复、堆完整性检查、OTA 回滚确认和持久化设备密钥 |

完整说明见 [功能详情](docs/FEATURES.md) 和 [已知问题](docs/KNOWN_ISSUES.md)。

## 主要硬件

- ESP32-S3，16 MB Flash，8 MB Octal PSRAM
- ILI9488 SPI 显示屏与 FT6336 触摸控制器
- PN532 NFC 模块，独立硬件 I2C
- SDMMC 1-bit TF 卡
- OV 系列摄像头；固定焦镜头可手动调焦，带执行器的 OV5640 可使用手动 AF 按钮

详细接线见 [firmware/docs/wiring-and-bringup.md](firmware/docs/wiring-and-bringup.md)。

## 构建

实测工具链为 **ESP-IDF v5.5.2**：

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py merge-bin
```

首次构建会下载锁定版本的 `esp32-camera`、`esp_jpeg` 和 `quirc` 管理组件。精简后的 ZXing-C++ 源码位于 `third_party/zxing-cpp/`。

## 从 V1.0 升级

> [!IMPORTANT]
> V1.1 修改了分区表。第一次从 V1.0 升级必须把完整 merged 镜像写入 `0x0`，不能直接在 V1.0 Web 页面上传 V1.1 OTA 应用镜像。

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 esp32_partdb_terminal_v1.1_merged.bin
```

安装过 V1.1 分区表后，后续相同布局版本可通过 Web OTA 更新。

## V1.11 发布文件

| 文件 | 用途 |
| --- | --- |
| `esp32_partdb_terminal_v1.11_ota.bin` | 已安装 V1.1 分区表设备的 Web OTA 应用镜像 |

V1.11 不提供新的 merged 镜像，因为分区布局没有变化。更新内容和兼容性见 [V1.11 发布说明](docs/GITHUB_RELEASE_V1.11.md)。

## 文档导航

- [V1.11 功能详情](docs/FEATURES.md)
- [V1.11 已知问题](docs/KNOWN_ISSUES.md)
- [V1.11 发布说明](docs/GITHUB_RELEASE_V1.11.md)
- [V1.0 → V1.1 变更对比](docs/CHANGES_V1.0_TO_V1.1.md)
- [V1.1 完整刷写说明](docs/RELEASE_V1.1.md)
- [固件工程说明](firmware/README.md)
- [第三方代码与许可证](docs/THIRD_PARTY_CODE.md)

## 开源协议

项目自有代码由灵异大队长按 Apache License 2.0 发布。第三方组件和生成字模保留各自的原始协议，详见 [NOTICE.md](NOTICE.md) 与 [第三方代码说明](docs/THIRD_PARTY_CODE.md)。
