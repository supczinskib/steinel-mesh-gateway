#!/usr/bin/env python3
from __future__ import annotations

import gzip
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / "esphome/components/steinel_mesh/steinel_page.html"
target = root / "esphome/components/steinel_mesh/steinel_page.h"
translations = source.with_name("steinel_i18n.js").read_text(encoding="utf-8")
page = source.read_text(encoding="utf-8").replace(
    "<!-- STEINEL_I18N -->", "<script>\n" + translations + "\n</script>"
)
revision = hashlib.sha256(page.encode("utf-8")).hexdigest()[:16]
raw = page.replace('__STEINEL_PAGE_REVISION__', revision).encode("utf-8")
compressed = gzip.compress(raw, compresslevel=9, mtime=0)
rows = [
    "  " + ", ".join(f"0x{value:02x}" for value in compressed[offset : offset + 16]) + ","
    for offset in range(0, len(compressed), 16)
]
target.write_text(
    "#pragma once\n#include <cstddef>\n#include <cstdint>\n"
    "namespace esphome::steinel_mesh {\n"
    f'static constexpr char STEINEL_PAGE_REVISION[] = "{revision}";\n'
    f"static constexpr size_t STEINEL_PAGE_RAW_SIZE = {len(raw)};\n"
    f"static const uint8_t STEINEL_PAGE_GZ[{len(compressed)}] = {{\n"
    + "\n".join(rows)
    + "\n};\n}  // namespace esphome::steinel_mesh\n",
    encoding="utf-8",
)
print(f"Generated {target.name}: {len(raw)} -> {len(compressed)} bytes")
