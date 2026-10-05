#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT_DIR/scripts/lib.sh"

"$ROOT_DIR/scripts/00_self_test.sh"
ESPHOME_BIN="$(find_esphome)"

cd "$ROOT_DIR/esphome"
"$ESPHOME_BIN" config steinel-c3.yaml
echo 'Steinel Mesh ESPHome configuration passed validation.'
