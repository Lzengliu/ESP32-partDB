# Third-party source

## ZXing-C++

- Path: `third_party/zxing-cpp`
- Upstream: https://github.com/zxing-cpp/zxing-cpp
- Commit: `6c2961d2a9ea4bc4e4ae8f37b1497299f04dd861`
- License: Apache License 2.0 (`zxing-cpp/LICENSE`)

V1.1 links a sparse, reader-only QR Code subset through
`firmware/components/zxing_qr`. Camera grayscale frames are decoded with
ZXing-C++ first; `espressif/quirc` remains as a fallback. Writers and unrelated
barcode formats are not linked, which limits flash and heap use on ESP32-S3.

The nested upstream `.git` directory is development metadata and is excluded
from public source packages.
