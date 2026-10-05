"""Keep selected devices registered while Mesh is disabled or updating."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "esphome/components/steinel_mesh/steinel_mesh.cpp").read_text()
start = source.index("  const bool auto_update_pending = this->load_auto_update_();", source.index("void SteinelMesh::setup()"))
end = source.index("\n}\n\nvoid SteinelMesh::dump_config()", start)
production = "void SteinelMesh::setup() {\n" + source[start:end] + "\n}\n"

harness = r'''
#include "mesh_protocol.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
namespace esphome::steinel_mesh {
struct Preference {
  bool value{false};
  bool load(bool *target) { *target=value; return true; }
};
struct Sensor { void publish_state(bool) {} };
class SteinelMesh {
 public:
  static constexpr uint16_t FLAG_ENABLED=1,FLAG_REMOVE_PENDING=2,FLAG_DEVICE_CATALOG=8;
  bool has_config{true},update_pending{false},catalog_valid_{true},legacy_profile_{false},
       configured_{true},mesh_mode_enabled_{false},mesh_start_pending_{false};
  std::atomic<bool> actual_output_forced_unavailable_{true};
  struct Config { uint16_t flags{FLAG_DEVICE_CATALOG},onoff_address{2}; } config_;
  Preference import_guard_preference_;
  Sensor ready;
  Sensor *ready_binary_sensor_{&ready};
  protocol::Catalog catalog_{};
  uint16_t identity_node_address_{0};
  unsigned registrations{0},identity_scans{0};
  std::vector<uint32_t> devices;
  std::string status;
  bool load_auto_update_() { return update_pending; }
  bool load_config_() { return has_config; }
  bool load_catalog_() { return catalog_valid_; }
  void load_advertised_identity_() {}
  void load_retired_address_() {}
  void load_address_policy_() {}
  void load_address_confirmation_() {}
  void load_device_key_() {}
  void set_status_(const std::string &message) { status=message; }
  void begin_identity_scan_() { ++identity_scans; }
  void setup_node_entities_() {
    if (!catalog_valid_) return;
    ++registrations;
    for (size_t i=0;i<catalog_.count;++i)
      if (catalog_.nodes[i].selected)
        devices.push_back(protocol::node_identity(catalog_.nodes[i],catalog_.mesh_uuid));
  }
  void setup();
};
'''
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  for (unsigned count : {0,1,4,16}) {
    std::vector<uint32_t> reference;
    for (bool update : {false,true}) for (bool enabled : {true,false}) {
      SteinelMesh gateway;
      gateway.update_pending=update;
      gateway.config_.flags |= enabled ? SteinelMesh::FLAG_ENABLED : 0;
      gateway.catalog_.mesh_uuid[0]=1;
      gateway.catalog_.count=16;
      for (size_t i=0;i<16;++i) {
        auto &node=gateway.catalog_.nodes[i];
        node.address=uint16_t(2+i*8);node.selected=i<count;
        node.uuid[0]=uint8_t(i+1);
      }
      gateway.setup();
      assert(gateway.registrations==1 && gateway.devices.size()==count);
      if (!update && enabled) reference=gateway.devices;
      assert(gateway.devices==reference);
      assert(gateway.identity_scans==0);
      assert(gateway.mesh_start_pending_==(!update && enabled));
      assert(gateway.actual_output_forced_unavailable_==(!enabled || update));
    }
  }
  SteinelMesh absent;absent.has_config=false;absent.setup();
  assert(absent.registrations==0 && !absent.mesh_start_pending_);
  SteinelMesh invalid;invalid.catalog_valid_=false;invalid.setup();
  assert(invalid.registrations==0 && !invalid.mesh_start_pending_);
  SteinelMesh interrupted;interrupted.import_guard_preference_.value=true;interrupted.setup();
  assert(interrupted.registrations==0 && !interrupted.mesh_start_pending_);
  std::puts("Production startup: selected device IDs survive Mesh disable and firmware update; invalid/absent catalogs remain excluded");
}
'''

with tempfile.TemporaryDirectory(prefix="steinel-disabled-devices-") as temporary:
    binary = str(Path(temporary) / "startup")
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                    "-x", "c++", "-", "-o", binary],
                   input=harness+production+main, text=True, check=True)
    subprocess.run([binary], check=True)
