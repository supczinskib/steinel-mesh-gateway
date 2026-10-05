"""Exercise production Mesh/Wi-Fi radio state transitions."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "esphome/components/steinel_mesh/steinel_mesh.cpp").read_text()
begin = source.index("void SteinelMesh::update_radio_coexistence_()")
production = source[begin:source.index("void SteinelMesh::loop()", begin)]
harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
using esp_err_t = int;
constexpr int ESP_OK=0,ESP_COEX_ST_TYPE_BLE=1;
constexpr uint32_t ESP_COEX_BLE_ST_MESH_CONFIG=8,ESP_COEX_BLE_ST_MESH_TRAFFIC=16,
                   ESP_COEX_BLE_ST_MESH_STANDBY=32;
uint32_t radio_bits=0;
uint32_t now_ms=100;
uint32_t millis(){return now_ms;}
unsigned calls=0;
bool fail_clear=false,fail_set=false;
int esp_coex_status_bit_clear(int type,uint32_t bits){
  assert(type==ESP_COEX_ST_TYPE_BLE);++calls;
  assert(bits==ESP_COEX_BLE_ST_MESH_CONFIG||bits==ESP_COEX_BLE_ST_MESH_TRAFFIC||bits==ESP_COEX_BLE_ST_MESH_STANDBY);
  if(fail_clear)return -1;
  radio_bits&=~bits;return ESP_OK;
}
int esp_coex_status_bit_set(int type,uint32_t bits){
  assert(type==ESP_COEX_ST_TYPE_BLE);++calls;
  assert(bits==ESP_COEX_BLE_ST_MESH_CONFIG||bits==ESP_COEX_BLE_ST_MESH_TRAFFIC||bits==ESP_COEX_BLE_ST_MESH_STANDBY);
  if(fail_set)return -1;
  radio_bits|=bits;return ESP_OK;
}
#define ESP_LOGW(...) ((void)0)
class SteinelMesh {
 public:
  enum class AccessOperation {NONE,NODE};
  enum class ControlKind {NONE,MODE};
  bool mesh_started_=false;
  uint32_t radio_coexistence_status_=0;
  uint32_t radio_coexistence_retry_at_=0;
  std::atomic<bool> mesh_ready_{false},node_completion_pending_{false};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  ControlKind control_kind_=ControlKind::NONE;
  void update_radio_coexistence_();
};
'''
main = r'''
int main(){
  SteinelMesh gateway;
  gateway.update_radio_coexistence_();assert(calls==0&&radio_bits==0);
  gateway.mesh_started_=true;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_TRAFFIC);
  const unsigned started_calls=calls;
  gateway.update_radio_coexistence_();assert(calls==started_calls);
  gateway.mesh_ready_=true;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_STANDBY);
  gateway.access_operation_=SteinelMesh::AccessOperation::NODE;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_TRAFFIC);
  gateway.access_operation_=SteinelMesh::AccessOperation::NONE;
  gateway.node_completion_pending_=true;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_TRAFFIC);
  gateway.node_completion_pending_=false;
  gateway.control_kind_=SteinelMesh::ControlKind::MODE;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_TRAFFIC);
  gateway.control_kind_=SteinelMesh::ControlKind::NONE;
  fail_clear=true;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_TRAFFIC);
  const unsigned failed_calls=calls;
  gateway.update_radio_coexistence_();assert(calls==failed_calls);
  now_ms+=1000;
  fail_clear=false;fail_set=true;
  gateway.update_radio_coexistence_();assert(radio_bits==0&&gateway.radio_coexistence_status_==0);
  fail_set=false;
  now_ms+=1000;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_STANDBY);
  gateway.mesh_started_=false;
  gateway.update_radio_coexistence_();assert(radio_bits==0);
  const unsigned stopped_calls=calls;
  gateway.update_radio_coexistence_();assert(calls==stopped_calls);
  now_ms=0xffffff00;
  gateway.mesh_started_=true;fail_set=true;
  gateway.update_radio_coexistence_();
  const unsigned wrap_calls=calls;
  now_ms=200;fail_set=false;
  gateway.update_radio_coexistence_();assert(calls==wrap_calls);
  now_ms=744;
  gateway.update_radio_coexistence_();assert(radio_bits==ESP_COEX_BLE_ST_MESH_STANDBY);
  std::puts("Production radio coexistence: initialization, traffic, idle, cleanup and SDK retry passed");
}
'''
with tempfile.TemporaryDirectory(prefix="steinel-radio-") as directory:
    binary = str(Path(directory) / "radio")
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-x", "c++", "-", "-o", binary],
                   input=harness+production+main, text=True, check=True)
    subprocess.run([binary], check=True)
