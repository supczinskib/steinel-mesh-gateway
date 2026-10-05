"""Exclude unused fixed-capacity device records from native API discovery."""
from pathlib import Path
from functools import wraps


def patch_device_info(source: str) -> str:
    loop = "  for (const auto &it : this->devices) {\n"
    filtered = loop + "    if (it.device_id == 0 && it.name.empty()) continue;\n"
    for method in ("encode", "calculate_size"):
        marker = f"DeviceInfoResponse::{method}("
        start = source.index(marker)
        end = source.index("\n}\n", start) + 3
        body = source[start:end]
        if body.count(loop) != 1:
            raise RuntimeError(f"Unsupported native API device discovery: {method}")
        if filtered not in body:
            source = source[:start] + body.replace(loop, filtered, 1) + source[end:]
    return source


def patch_build(source_path: Path) -> None:
    original = source_path.read_text()
    updated = patch_device_info(original)
    if updated != original:
        source_path.write_text(updated)


def install_build_hook() -> None:
    from esphome import writer
    from esphome.core import CORE

    if getattr(writer.copy_src_tree, "_steinel_devices", False):
        return
    copy_sources = writer.copy_src_tree

    @wraps(copy_sources)
    def copy_src_tree():
        copy_sources()
        patch_build(Path(CORE.relative_src_path("esphome/components/api/api_pb2.cpp")))

    copy_src_tree._steinel_devices = True
    writer.copy_src_tree = copy_src_tree
