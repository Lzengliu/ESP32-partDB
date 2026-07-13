#!/bin/sh
set -eu

VERSION="v1.1"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/firmware/build"
DIST="$ROOT/dist/$VERSION"
SOURCE="$DIST/source"
RELEASE="$DIST/release-files"
APP="$BUILD/esp32_partdb_terminal.bin"
MERGED="$BUILD/merged-binary.bin"

require_file() {
    if [ ! -f "$1" ]; then
        echo "Missing required file: $1" >&2
        exit 1
    fi
}

require_file "$BUILD/bootloader/bootloader.bin"
require_file "$BUILD/partition_table/partition-table.bin"
require_file "$BUILD/ota_data_initial.bin"
require_file "$APP"
require_file "$MERGED"

if [ "$MERGED" -ot "$APP" ]; then
    echo "Merged image is older than the app image; run 'idf.py merge-bin' first." >&2
    exit 1
fi

rm -rf "$DIST"
mkdir -p "$SOURCE" "$RELEASE"

for name in .gitignore AUTHORS.md LICENSE NOTICE.md README.md; do
    cp "$ROOT/$name" "$SOURCE/$name"
done

rsync -a --exclude '.DS_Store' "$ROOT/docs/" "$SOURCE/docs/"
rsync -a \
    --exclude '.DS_Store' \
    --exclude 'build/' \
    --exclude 'managed_components/' \
    --exclude 'sdkconfig.old' \
    "$ROOT/firmware/" "$SOURCE/firmware/"
rsync -a \
    --exclude '.DS_Store' \
    --exclude '.git/' \
    "$ROOT/third_party/" "$SOURCE/third_party/"
rsync -a \
    --exclude '.DS_Store' \
    --exclude '__pycache__/' \
    --exclude '*.pyc' \
    "$ROOT/tools/" "$SOURCE/tools/"

cp "$BUILD/bootloader/bootloader.bin" "$RELEASE/bootloader.bin"
cp "$BUILD/partition_table/partition-table.bin" "$RELEASE/partition-table.bin"
cp "$BUILD/ota_data_initial.bin" "$RELEASE/ota_data_initial.bin"
cp "$APP" "$RELEASE/esp32_partdb_terminal_v1.1_ota.bin"
cp "$MERGED" "$RELEASE/esp32_partdb_terminal_v1.1_merged.bin"
cp "$ROOT/LICENSE" "$RELEASE/LICENSE"
cp "$ROOT/NOTICE.md" "$RELEASE/NOTICE.md"
cp "$ROOT/AUTHORS.md" "$RELEASE/AUTHORS.md"
cp "$ROOT/docs/RELEASE_V1.1.md" "$RELEASE/README_V1.1.md"
cp "$ROOT/docs/CHANGES_V1.0_TO_V1.1.md" "$RELEASE/CHANGES_V1.0_TO_V1.1.md"
cp "$ROOT/docs/licenses/OFL-1.1.txt" "$RELEASE/OFL-1.1.txt"
cp "$ROOT/docs/licenses/ISC-quirc.txt" "$RELEASE/ISC-quirc.txt"
cp "$ROOT/third_party/zxing-cpp/LICENSE" "$RELEASE/ZXING-CXX-LICENSE"

(
    cd "$RELEASE"
    shasum -a 256 \
        bootloader.bin \
        partition-table.bin \
        ota_data_initial.bin \
        esp32_partdb_terminal_v1.1_ota.bin \
        esp32_partdb_terminal_v1.1_merged.bin \
        > SHA256SUMS_FIRMWARE
)

(
    cd "$SOURCE"
    zip -qry "$DIST/esp32_partdb_terminal_v1.1_source.zip" .
)
(
    cd "$RELEASE"
    zip -qry "$DIST/esp32_partdb_terminal_v1.1_firmware.zip" .
)

cp "$RELEASE/esp32_partdb_terminal_v1.1_merged.bin" "$DIST/"
cp "$RELEASE/esp32_partdb_terminal_v1.1_ota.bin" "$DIST/"

(
    cd "$DIST"
    shasum -a 256 \
        esp32_partdb_terminal_v1.1_firmware.zip \
        esp32_partdb_terminal_v1.1_source.zip \
        esp32_partdb_terminal_v1.1_merged.bin \
        esp32_partdb_terminal_v1.1_ota.bin \
        > SHA256SUMS
)

echo "Release prepared at: $DIST"
