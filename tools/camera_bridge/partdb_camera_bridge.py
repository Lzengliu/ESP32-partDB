#!/usr/bin/env python3
"""
Concurrent ESP32 camera frame bridge for official Part-DB deployments.

It accepts JPEG frames from the MCU, decodes QR/barcode content with zbarimg,
queries the Part-DB HTTP API, and returns a compact JSON result to the MCU.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, quote, urlsplit
from urllib.request import Request, urlopen


def env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    raw = os.environ.get(name, "")
    if not raw:
        return default
    try:
        value = int(raw)
    except ValueError:
        return default
    return max(minimum, min(maximum, value))


@dataclass(frozen=True)
class Config:
    host: str
    port: int
    partdb_url: str
    partdb_token: str
    terminal_token: str
    zbarimg_bin: str
    max_frame_bytes: int
    decode_timeout_s: int
    partdb_timeout_s: int
    decode_concurrency: int
    save_dir: str


CFG = Config(
    host=os.environ.get("BRIDGE_HOST", "0.0.0.0"),
    port=env_int("BRIDGE_PORT", 8099, 1, 65535),
    partdb_url=os.environ.get("PARTDB_URL", "").rstrip("/"),
    partdb_token=os.environ.get("PARTDB_TOKEN", ""),
    terminal_token=os.environ.get("TERMINAL_TOKEN", ""),
    zbarimg_bin=os.environ.get("ZBARIMG_BIN", "zbarimg"),
    max_frame_bytes=env_int("MAX_FRAME_BYTES", 2 * 1024 * 1024, 1024, 16 * 1024 * 1024),
    decode_timeout_s=env_int("DECODE_TIMEOUT_S", 8, 1, 60),
    partdb_timeout_s=env_int("PARTDB_TIMEOUT_S", 8, 1, 60),
    decode_concurrency=env_int("DECODE_CONCURRENCY", 2, 1, 32),
    save_dir=os.environ.get("SAVE_FRAMES_DIR", ""),
)

DECODE_SEM = threading.BoundedSemaphore(CFG.decode_concurrency)
REQUEST_COUNT = 0
REQUEST_LOCK = threading.Lock()


def log(message: str) -> None:
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"{stamp} {message}", file=sys.stderr, flush=True)


def json_bytes(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def parse_auth_token(value: str | None) -> str:
    if not value:
        return ""
    value = value.strip()
    lower = value.lower()
    if lower.startswith("bearer "):
        return value[7:].strip()
    if lower.startswith("token "):
        return value[6:].strip()
    return ""


def build_partdb_url(path: str) -> str:
    if not CFG.partdb_url:
        raise RuntimeError("PARTDB_URL is not configured")
    if path.startswith("http://") or path.startswith("https://"):
        return path

    base = CFG.partdb_url.rstrip("/")
    base_is_api = base.endswith("/api")
    endpoint = path
    if endpoint.startswith("/api/"):
        endpoint = endpoint[4:] if base_is_api else endpoint
    elif endpoint == "/api":
        endpoint = "" if base_is_api else "/api"
    elif not base_is_api:
        endpoint = "/api/" + endpoint.lstrip("/")

    slash = "" if endpoint.startswith("/") or endpoint == "" else "/"
    return base + slash + endpoint


def partdb_get(path: str, token: str) -> dict[str, Any]:
    url = build_partdb_url(path)
    headers = {
        "Accept": "application/ld+json, application/json",
        "User-Agent": "esp32-partdb-camera-bridge/1.0",
    }
    if token:
        headers["Authorization"] = "Bearer " + token

    req = Request(url, headers=headers, method="GET")
    try:
        with urlopen(req, timeout=CFG.partdb_timeout_s) as resp:
            body = resp.read(1024 * 1024)
            status = int(resp.status)
    except HTTPError as exc:
        body = exc.read(1024 * 1024)
        status = int(exc.code)
    except URLError as exc:
        return {
            "ok": False,
            "err": "partdb_request_failed",
            "detail": str(exc.reason),
            "path": path,
            "http_status": 0,
        }

    text = body.decode("utf-8", "replace")
    data: Any = None
    if text:
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            data = {"raw": text[:2048]}

    return {
        "ok": 200 <= status < 300,
        "err": None if 200 <= status < 300 else "partdb_http_error",
        "path": path,
        "http_status": status,
        "data": data,
    }


def parse_id_after_marker(text: str, marker: str) -> int | None:
    pos = text.find(marker)
    if pos < 0:
        return None
    tail = text[pos + len(marker):]
    match = re.match(r"(\d+)(?:$|[/?#\s])", tail)
    if not match:
        return None
    value = int(match.group(1))
    return value if value > 0 else None


def make_target(prefix: str, entity_id: int) -> dict[str, Any] | None:
    if entity_id <= 0:
        return None
    mapping = {
        "P": ("part", "parts", "part"),
        "L": ("lot", "part_lots", "lot"),
        "S": ("location", "storage_locations", "location"),
    }
    entry = mapping.get(prefix)
    if not entry:
        return None
    entity_type, api_name, scan_name = entry
    return {
        "type": entity_type,
        "prefix": prefix,
        "id": entity_id,
        "barcode": f"{prefix}{entity_id:04d}",
        "api_path": f"/api/{api_name}/{entity_id}.jsonld",
        "scan_path": f"/scan/{scan_name}/{entity_id}",
    }


def target_from_text(text: str) -> dict[str, Any] | None:
    value = text.strip()
    for marker, prefix in (
        ("/scan/part/", "P"),
        ("/api/parts/", "P"),
        ("/parts/", "P"),
        ("/scan/lot/", "L"),
        ("/api/part_lots/", "L"),
        ("/part_lots/", "L"),
        ("/scan/location/", "S"),
        ("/api/storage_locations/", "S"),
        ("/storage_locations/", "S"),
    ):
        entity_id = parse_id_after_marker(value, marker)
        if entity_id:
            return make_target(prefix, entity_id)

    match = re.fullmatch(r"([PLS])(\d+)", value.upper())
    if match:
        return make_target(match.group(1), int(match.group(2)))

    match = re.fullmatch(r"([PLS])-(\d{6,})", value.upper())
    if match:
        return make_target(match.group(1), int(match.group(2)))

    match = re.fullmatch(r"\$L(\d{5,})", value.upper())
    if match:
        return make_target("S", int(match.group(1)))

    match = re.fullmatch(r"(\d{7})\d?", value)
    if match:
        return make_target("P", int(match.group(1)))

    return None


def first_member_id(data: Any, marker: str) -> int | None:
    if not isinstance(data, dict):
        return None
    members = data.get("hydra:member") or data.get("member") or []
    if not isinstance(members, list) or not members:
        return None
    first = members[0]
    if not isinstance(first, dict):
        return None
    entity_id = first.get("id")
    if isinstance(entity_id, int) and entity_id > 0:
        return entity_id
    iri = first.get("@id")
    if isinstance(iri, str):
        return parse_id_after_marker(iri, marker)
    return None


def target_lookup_path(target: dict[str, Any]) -> str:
    prefix = target.get("prefix")
    entity_id = int(target.get("id") or 0)
    if prefix == "P":
        return (
            f"/api/parts/{entity_id}.jsonld?"
            "properties[]=id&properties[]=name&properties[]=ipn&"
            "properties[]=description&properties[category][]=name"
        )
    if prefix == "L":
        return (
            f"/api/part_lots/{entity_id}.jsonld?"
            "properties[]=id&properties[]=description&properties[]=user_barcode&"
            "properties[part][]=id&properties[part][]=name&properties[part][]=ipn&"
            "properties[storage_location][]=id&properties[storage_location][]=name"
        )
    if prefix == "S":
        return (
            f"/api/storage_locations/{entity_id}.jsonld?"
            "properties[]=id&properties[]=name&properties[]=comment"
        )
    return str(target.get("api_path") or "")


def summarize_target_data(target: dict[str, Any], data: Any) -> dict[str, Any]:
    if not isinstance(data, dict):
        return {}
    prefix = target.get("prefix")
    if prefix == "P":
        category = data.get("category")
        return {
            "id": data.get("id"),
            "type": "part",
            "name": data.get("name"),
            "ipn": data.get("ipn"),
            "description": data.get("description"),
            "category": category.get("name") if isinstance(category, dict) else None,
        }
    if prefix == "L":
        part = data.get("part")
        location = data.get("storage_location") or data.get("storageLocation")
        return {
            "id": data.get("id"),
            "type": "lot",
            "description": data.get("description"),
            "user_barcode": data.get("user_barcode") or data.get("userBarcode"),
            "part": {
                "id": part.get("id"),
                "name": part.get("name"),
                "ipn": part.get("ipn"),
            } if isinstance(part, dict) else None,
            "storage_location": {
                "id": location.get("id"),
                "name": location.get("name"),
            } if isinstance(location, dict) else None,
        }
    if prefix == "S":
        return {
            "id": data.get("id"),
            "type": "location",
            "name": data.get("name"),
            "comment": data.get("comment"),
        }
    return {}


def resolve_with_partdb(decoded: str, token: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "barcode_kind": "unknown",
        "target": None,
        "lookups": [],
        "partdb_lookup": None,
    }

    target = target_from_text(decoded)
    if target:
        result["barcode_kind"] = "internal"
    else:
        encoded = quote(decoded, safe="")
        path = (
            "/api/part_lots.jsonld?"
            f"user_barcode={encoded}&itemsPerPage=1&"
            "properties[]=id&properties[]=user_barcode"
        )
        lookup = partdb_get(path, token)
        result["lookups"].append({k: lookup.get(k) for k in ("ok", "err", "path", "http_status")})
        lot_id = first_member_id(lookup.get("data"), "/part_lots/") if lookup.get("ok") else None
        if lot_id:
            target = make_target("L", lot_id)
            result["barcode_kind"] = "part_lot_user_barcode"

    if not target:
        encoded = quote(decoded, safe="")
        fields = ("ipn", "name", "description", "manufacturer_product_number")
        for field in fields:
            path = (
                "/api/parts.jsonld?"
                f"{field}=%25{encoded}%25&itemsPerPage=1&"
                "properties[]=id&properties[]=ipn&properties[]=name"
            )
            lookup = partdb_get(path, token)
            result["lookups"].append({k: lookup.get(k) for k in ("ok", "err", "path", "http_status")})
            part_id = first_member_id(lookup.get("data"), "/parts/") if lookup.get("ok") else None
            if part_id:
                target = make_target("P", part_id)
                result["barcode_kind"] = "part_" + field
                break

    result["target"] = target
    if not target:
        return result

    detail = partdb_get(target_lookup_path(target), token)
    summary = summarize_target_data(target, detail.get("data")) if detail.get("ok") else {}
    result["partdb_lookup"] = {
        "ok": detail.get("ok"),
        "err": detail.get("err"),
        "path": detail.get("path"),
        "http_status": detail.get("http_status"),
        "summary": summary,
    }
    return result


def content_type_base(value: str) -> str:
    return value.split(";", 1)[0].strip().lower()


def content_type_params(value: str) -> dict[str, str]:
    params: dict[str, str] = {}
    for part in value.split(";")[1:]:
        if "=" not in part:
            continue
        key, val = part.split("=", 1)
        params[key.strip().lower()] = val.strip().strip('"')
    return params


def int_param(params: dict[str, str], query: dict[str, list[str]], name: str) -> int:
    raw = params.get(name)
    if not raw:
        values = query.get(name) or []
        raw = values[0] if values else ""
    try:
        value = int(raw)
    except (TypeError, ValueError):
        return 0
    return value if value > 0 else 0


def rgb565_to_pgm(frame: bytes, width: int, height: int, endian: str) -> bytes:
    if width <= 0 or height <= 0:
        raise ValueError("bad_rgb565_dimensions")
    expected = width * height * 2
    if len(frame) != expected:
        raise ValueError(f"bad_rgb565_size:{len(frame)}!={expected}")

    pixels = bytearray(width * height)
    little = endian.lower() != "be"
    out_i = 0
    for i in range(0, len(frame), 2):
        value = frame[i] | (frame[i + 1] << 8) if little else (frame[i] << 8) | frame[i + 1]
        r = ((value >> 11) & 0x1F) * 255 // 31
        g = ((value >> 5) & 0x3F) * 255 // 63
        b = (value & 0x1F) * 255 // 31
        pixels[out_i] = (r * 30 + g * 59 + b * 11) // 100
        out_i += 1

    return f"P5\n{width} {height}\n255\n".encode("ascii") + bytes(pixels)


def decode_input_to_file(frame: bytes, content_type: str, query: dict[str, list[str]],
                         save_dir: str) -> tuple[str, bool]:
    base = content_type_base(content_type)
    params = content_type_params(content_type)
    keep_file = bool(save_dir)

    if base in ("", "image/jpeg", "application/octet-stream"):
        if len(frame) < 4 or frame[:2] != b"\xff\xd8":
            raise ValueError("invalid_jpeg")
        suffix = ".jpg"
        payload = frame
    elif base == "application/x-rgb565":
        width = int_param(params, query, "width")
        height = int_param(params, query, "height")
        if not width or not height:
            if len(frame) == 96 * 96 * 2:
                width, height = 96, 96
            elif len(frame) == 160 * 120 * 2:
                width, height = 160, 120
        payload = rgb565_to_pgm(frame, width, height, params.get("endian", "le"))
        suffix = ".pgm"
    else:
        raise ValueError("unsupported_content_type:" + base)

    if keep_file:
        os.makedirs(save_dir, exist_ok=True)
        tmp = tempfile.NamedTemporaryFile(prefix="frame-", suffix=suffix, dir=save_dir, delete=False)
    else:
        tmp = tempfile.NamedTemporaryFile(prefix="frame-", suffix=suffix, delete=False)

    tmp_path = tmp.name
    with tmp:
        tmp.write(payload)
    return tmp_path, keep_file


def decode_frame(frame: bytes, content_type: str, query: dict[str, list[str]]) -> dict[str, Any]:
    zbar = shutil.which(CFG.zbarimg_bin) or CFG.zbarimg_bin
    if not shutil.which(zbar) and "/" not in zbar:
        return {"ok": False, "err": "zbarimg_not_found", "symbols": []}

    save_dir = CFG.save_dir.strip()
    tmp_path = ""
    keep_file = bool(save_dir)
    try:
        tmp_path, keep_file = decode_input_to_file(frame, content_type, query, save_dir)

        with DECODE_SEM:
            proc = subprocess.run(
                [zbar, "--quiet", "--raw", tmp_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=CFG.decode_timeout_s,
                check=False,
            )
    except ValueError as exc:
        return {"ok": False, "err": str(exc), "symbols": []}
    except subprocess.TimeoutExpired:
        return {"ok": False, "err": "decode_timeout", "symbols": [], "frame_path": tmp_path if keep_file else None}
    finally:
        if tmp_path and not keep_file:
            try:
                os.unlink(tmp_path)
            except FileNotFoundError:
                pass

    stdout = proc.stdout.decode("utf-8", "replace")
    stderr = proc.stderr.decode("utf-8", "replace").strip()
    symbols = [line.strip() for line in stdout.splitlines() if line.strip()]
    return {
        "ok": bool(symbols),
        "err": None if symbols else (stderr or "no_symbol"),
        "symbols": symbols,
        "frame_path": tmp_path if keep_file else None,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "PartDBCameraBridge/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        log("%s - %s" % (self.address_string(), fmt % args))

    def send_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json_bytes(payload)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self) -> bool:
        if not CFG.terminal_token:
            return True
        auth_token = parse_auth_token(self.headers.get("Authorization"))
        x_token = (self.headers.get("X-Terminal-Token") or "").strip()
        allowed = {CFG.terminal_token}
        if CFG.partdb_token:
            allowed.add(CFG.partdb_token)
        return auth_token in allowed or x_token in allowed

    def do_GET(self) -> None:
        if self.path == "/" or self.path.startswith("/health"):
            self.send_json(HTTPStatus.OK, {
                "ok": True,
                "service": "partdb_camera_bridge",
                "partdb_configured": bool(CFG.partdb_url),
                "zbarimg": shutil.which(CFG.zbarimg_bin) or CFG.zbarimg_bin,
                "decode_concurrency": CFG.decode_concurrency,
            })
            return
        self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "err": "not_found"})

    def do_POST(self) -> None:
        if urlsplit(self.path).path not in ("/api/terminal/scan/frame", "/scan/frame"):
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "err": "not_found"})
            return
        if not self.authorized():
            self.send_json(HTTPStatus.UNAUTHORIZED, {"ok": False, "err": "unauthorized"})
            return

        try:
            self.handle_frame_upload()
        except Exception as exc:  # pragma: no cover - defensive logging for field use
            traceback.print_exc()
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {
                "ok": False,
                "err": "internal_error",
                "detail": str(exc),
            })

    def handle_frame_upload(self) -> None:
        length_raw = self.headers.get("Content-Length")
        if not length_raw:
            self.send_json(HTTPStatus.LENGTH_REQUIRED, {"ok": False, "err": "content_length_required"})
            return
        try:
            length = int(length_raw)
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "err": "bad_content_length"})
            return
        if length <= 0:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "err": "empty_frame"})
            return
        if length > CFG.max_frame_bytes:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {
                "ok": False,
                "err": "frame_too_large",
                "max_bytes": CFG.max_frame_bytes,
            })
            return

        content_type = (self.headers.get("Content-Type") or "").lower()
        base_content_type = content_type_base(content_type)
        if base_content_type and base_content_type not in (
            "image/jpeg",
            "application/octet-stream",
            "application/x-rgb565",
        ):
            self.send_json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {
                "ok": False,
                "err": "unsupported_content_type",
                "content_type": content_type,
            })
            return

        started = time.monotonic()
        frame = self.rfile.read(length)
        if len(frame) != length:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "err": "truncated_frame"})
            return

        with REQUEST_LOCK:
            global REQUEST_COUNT
            REQUEST_COUNT += 1
            request_id = REQUEST_COUNT

        query = parse_qs(urlsplit(self.path).query)
        decode = decode_frame(frame, content_type, query)
        symbols = decode.get("symbols") or []
        decoded = symbols[0] if symbols else ""
        token = CFG.partdb_token or parse_auth_token(self.headers.get("Authorization"))

        response: dict[str, Any] = {
            "ok": True,
            "request_id": request_id,
            "received_bytes": len(frame),
            "decode": decode,
            "found": bool(decoded),
            "decoded": decoded,
            "partdb_configured": bool(CFG.partdb_url and token),
            "elapsed_ms": int((time.monotonic() - started) * 1000),
        }

        if decoded and CFG.partdb_url and token:
            response.update(resolve_with_partdb(decoded, token))
        elif decoded:
            response.update({
                "barcode_kind": "unresolved",
                "target": target_from_text(decoded),
                "lookups": [],
                "partdb_lookup": None,
            })

        response["elapsed_ms"] = int((time.monotonic() - started) * 1000)
        self.send_json(HTTPStatus.OK, response)


def main() -> int:
    if any(arg in ("-h", "--help") for arg in sys.argv[1:]):
        print(
            "Usage: partdb_camera_bridge.py\n\n"
            "Environment:\n"
            "  BRIDGE_HOST=0.0.0.0\n"
            "  BRIDGE_PORT=8099\n"
            "  PARTDB_URL=http://127.0.0.1:8080\n"
            "  PARTDB_TOKEN=<partdb-api-token>\n"
            "  TERMINAL_TOKEN=<optional-upload-token>\n"
            "  DECODE_CONCURRENCY=2\n"
            "  ZBARIMG_BIN=zbarimg\n"
        )
        return 0

    if not CFG.partdb_url:
        log("PARTDB_URL is not set; decoding will work but Part-DB lookup will be skipped")
    if not CFG.partdb_token:
        log("PARTDB_TOKEN is not set; the incoming Authorization token will be used if present")
    if not shutil.which(CFG.zbarimg_bin):
        log(f"warning: {CFG.zbarimg_bin!r} was not found in PATH")

    server = ThreadingHTTPServer((CFG.host, CFG.port), Handler)
    server.daemon_threads = True

    def stop(_signum: int, _frame: Any) -> None:
        log("stopping")
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    log(f"listening on http://{CFG.host}:{CFG.port}/api/terminal/scan/frame")
    server.serve_forever()
    server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
