"""Exercise the flash-backed JSON parser with synthetic Mesh backups."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "esphome/components/steinel_mesh/steinel_web.cpp").read_text()
PARSER = SOURCE[SOURCE.index("bool decode_hex("):SOURCE.index("}  // namespace\n\nesp_err_t SteinelMesh::cloud_http_event_")]
PREFIX = r'''
#include "mesh_protocol.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
using namespace esphome::steinel_mesh;
using esp_err_t = int;
constexpr int ESP_OK = 0;
unsigned parser_yields = 0;
namespace esphome { void delay(uint32_t ms) { assert(ms == 1); ++parser_yields; } }
struct esp_partition_t { const std::string *data; };
int esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t count) {
  if (offset > p->data->size() || count > p->data->size() - offset) return 1;
  std::memcpy(out, p->data->data() + offset, count); return 0;
}
'''
SUFFIX = r'''
int main() {
  uint8_t bytes[16]{}; uint16_t address = 0;
  if (decode_hex("", bytes, 16) || decode_hex("0", bytes, 16) ||
      parse_hex_address("-1", address) || parse_hex_address("0000", address) ||
      !parse_hex_value("C123", address) || address != 0xC123) return 2;
  std::string input((std::istreambuf_iterator<char>(std::cin)), {});
  esp_partition_t partition{&input}; FlashJsonReader reader(&partition, input.size());
  BackupSummary summary{};
  bool ok = read_backup(reader, summary) && reader.healthy();
  reader.skip_whitespace(); char trailing;
  ok &= !reader.peek(trailing);
  if (!ok) { std::cout << "invalid"; return 0; }
  std::cout << summary.node_count << ':';
  for (size_t i = 0; i < summary.node_count; ++i) {
    const auto &node = summary.nodes[i];
    std::cout << node.details.name << '/' << node.details.product_id << '/'
              << node.bound_app_key << '/' << node.details.elements[0].sensor_group << '/'
              << node.details.elements[0].capabilities << ';';
  }
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix="steinel-parser-") as tmp:
        binary = str(Path(tmp) / "parser")
        compiler = os.environ.get("CXX", "c++")
        subprocess.run([compiler, "-std=c++17", "-fsanitize=address,undefined", "-g",
                        "-I", str(ROOT / "esphome/components/steinel_mesh"), "-x", "c++",
                        "-", "-o", binary], input=PREFIX + PARSER + SUFFIX, text=True, check=True)
        def parse(data):
            return subprocess.run([binary], input=data, text=True, capture_output=True, check=True).stdout
        model = lambda mid, key=3, address="0000": {"modelId": mid, "bind": [key], "publish": {"address": address}}
        node = {"name": "Garden", "UUID": "0102030405060708090a0b0c0d0e0f10", "unicastAddress": "0001",
                "deviceKey": "ab" * 16, "cid": "0563", "pid": "1DE0",
                "elements": [{"models": [model("0000", 7), model("1000"), model("1300"), model("1100", address="C123")]}]}
        backup = {"meshUUID": "01" * 16, "meshName": "Synthetic network", "nodes": [node],
                  "appKeys": [{"index": 3, "boundNetKey": 0, "key": "02" * 16}],
                  "netKeys": [{"index": 0, "key": "03" * 16}],
                  "provisioners": [{"allocatedUnicastRange": [{"lowAddress": "0001", "highAddress": "7FFF"}]}]}
        encoded = json.dumps(backup)
        assert parse(encoded) == "1:Garden/7648/3/49443/11;"  # skip Config Server's unrelated binding
        # Time Server must not authorize Scene Recall; Scene Server must be bound.
        node["elements"][0]["models"].append(model("1200"))
        assert parse(json.dumps(backup)).endswith("/11;")
        node["elements"][0]["models"].append(model("1203", key=3))
        assert parse(json.dumps(backup)).endswith("/27;")
        node["elements"][0]["models"][-1]["bind"] = []
        assert parse(json.dumps(backup)).endswith("/11;")
        del node["elements"][0]["models"][-2:]
        assert parse(encoded + "garbage") == "invalid"
        assert parse(encoded[:-1]) == "invalid"
        assert parse('{"nodes":[') == "invalid"
        for invalid in ['{"unknown":garbage}', '{"unknown":01}', '{"unknown":1.}',
                        '{"unknown":1e}', '{"unknown":"\\uD800"}', '{"unknown":"\\uDC00"}']:
            assert parse(invalid) == "invalid"
        assert parse('{"unknown":-1.2e+3,"valid":true,"nothing":null,"list":[false]}') == '0:'
        node["name"] = "Entrée Łąka 🌙"
        assert "Entrée Łąka 🌙" in parse(json.dumps(backup))
        assert "Entrée Łąka 🌙" in parse(json.dumps(backup, ensure_ascii=False))
        node["name"] = "Garden"
        # Unknown phone metadata with zero CID/PID must not reject the network.
        node["cid"] = "0000"; node["pid"] = "0000"
        assert parse(json.dumps(backup)).startswith("1:Garden/0/3/")
        node["elements"][0]["models"][-1]["bind"] = [4]
        assert parse(json.dumps(backup)).endswith("/11;")
        node["elements"][0]["models"][1]["bind"] = []
        assert parse(json.dumps(backup)).endswith("/10;")  # unbound OnOff is not advertised as supported
        # Truncation at every byte boundary: never a read outside the partition.
        for end in range(len(encoded)):
            assert parse(encoded[:end]) == "invalid"
        print("Actual backup parser: synthetic models, groups, keys, zero IDs, trailing data and truncations passed")


if __name__ == "__main__":
    run()
