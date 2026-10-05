#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT_DIR/scripts/lib.sh"
CONFIG="$ROOT_DIR/esphome/steinel-c3.yaml"
ESPHOME_BIN="$(find_esphome)"
[[ $# -le 1 ]] || { echo 'Usage: 10_prepare_release.sh [version]' >&2; exit 1; }
DEFAULT_VERSION="$(sed -n 's/^[[:space:]]*project_version:[[:space:]]*"\([^"]*\)"/\1/p' "$CONFIG")"
VERSION="${1:-$DEFAULT_VERSION}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Invalid project version: ${VERSION:-missing}" >&2
  exit 1
}

cd "$ROOT_DIR/esphome"
python3 "$ROOT_DIR/scripts/generate_steinel_page.py"
bash "$ROOT_DIR/scripts/00_self_test.sh"
env -u IDF_PATH -u IDF_TOOLS_PATH "$ESPHOME_BIN" -s project_version "$VERSION" clean steinel-c3.yaml
env -u IDF_PATH -u IDF_TOOLS_PATH "$ESPHOME_BIN" -s project_version "$VERSION" compile steinel-c3.yaml

FACTORY_SOURCE="$(find .esphome/build -type f -name firmware.factory.bin -print -quit)"
OTA_SOURCE="$(find .esphome/build -type f -name firmware.ota.bin -print -quit)"
[[ -n "$FACTORY_SOURCE" && -f "$FACTORY_SOURCE" ]] || {
  echo 'Compiled firmware.factory.bin was not found.' >&2
  exit 1
}
[[ -n "$OTA_SOURCE" && -f "$OTA_SOURCE" ]] || {
  echo 'Compiled firmware.ota.bin was not found.' >&2
  exit 1
}

RELEASE_DIR="$ROOT_DIR/output/v$VERSION"
BASE_NAME="steinel-mesh-esp32-c3-gateway-v$VERSION"
mkdir -p "$RELEASE_DIR"
cp "$FACTORY_SOURCE" "$RELEASE_DIR/$BASE_NAME-factory.bin"
cp "$OTA_SOURCE" "$RELEASE_DIR/$BASE_NAME-ota.bin"

cd "$RELEASE_DIR"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$BASE_NAME-factory.bin" "$BASE_NAME-ota.bin" > SHA256SUMS
else
  shasum -a 256 "$BASE_NAME-factory.bin" "$BASE_NAME-ota.bin" > SHA256SUMS
fi

printf 'Release files prepared in %s\n' "$RELEASE_DIR"
cat SHA256SUMS
