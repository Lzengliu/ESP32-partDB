# Part-DB Camera Bridge

Linux-side bridge for the ESP32 camera workflow. It receives JPEG still frames from the MCU, decodes QR/barcode content with `zbarimg`, queries the official Part-DB API, and returns JSON to the MCU.

This does not modify the official Part-DB Docker image.

## Install

Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y python3 zbar-tools
```

## Run

```bash
export PARTDB_URL="http://127.0.0.1:8080"
export PARTDB_TOKEN="your-partdb-api-token"
export BRIDGE_PORT=8099
export DECODE_CONCURRENCY=2
python3 tools/camera_bridge/partdb_camera_bridge.py
```

Optional access token for ESP32 uploads:

```bash
export TERMINAL_TOKEN="change-me"
```

If `TERMINAL_TOKEN` is set, uploads must send either `TERMINAL_TOKEN` or the
configured `PARTDB_TOKEN`. The current firmware sends the normal Part-DB token
when it uploads a still frame, so this can stay empty while testing on a trusted
LAN.

Accepted upload headers:

```text
Authorization: Bearer change-me
```

or:

```text
X-Terminal-Token: change-me
```

## ESP32 Setting

Set the device Web UI field `静帧上传地址` to:

```text
http://<linux-host-ip>:8099/api/terminal/scan/frame
```

Keep the normal `Part-DB 地址` and `Part-DB API Token` pointing at the official Part-DB container.

## API

Health:

```bash
curl http://127.0.0.1:8099/health
```

Upload a JPEG frame:

```bash
curl -X POST \
  -H 'Content-Type: image/jpeg' \
  --data-binary @frame.jpg \
  http://127.0.0.1:8099/api/terminal/scan/frame
```

Low-memory ESP32 firmware may upload raw RGB565 instead of JPEG:

```text
Content-Type: application/x-rgb565; width=96; height=96; endian=le
```

The bridge converts RGB565 to a temporary grayscale PGM image before running
`zbarimg`, so the MCU does not need to spend memory on JPEG encoding.

The response includes:

- `decode.symbols`: decoded barcode strings
- `decoded`: first decoded string
- `target`: parsed Part-DB target when recognized
- `partdb_lookup.summary`: compact Part-DB object summary

## Concurrency

The server uses `ThreadingHTTPServer`, so multiple ESP32 terminals can upload at the same time. Barcode decoding is limited by `DECODE_CONCURRENCY` so the Linux host does not spawn too many `zbarimg` processes.

For multiple MCUs, point all terminals at the same bridge URL. If the camera is moved to a dedicated camera MCU later, that MCU can use the same endpoint.
