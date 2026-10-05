"""Exercise production manual/periodic identity reads and composition callbacks."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
poll = runpy.run_path(str(ROOT / "tests/test_node_polling.py"))
mesh = (ROOT / "esphome/components/steinel_mesh/steinel_mesh.cpp").read_text()
start = mesh.index("void SteinelMesh::request_refresh() {")
end = mesh.index("  if (!this->legacy_profile_) return;", start)
refresh = mesh[start:end] + "  if (!this->legacy_profile_) return;\n}\n"
start = poll["source"].index("bool SteinelMesh::node_composition_event_(")
end = poll["source"].index("void SteinelMesh::advance_nodes_(", start)
composition = poll["source"][start:end]
harness = poll["harness"].replace("  void test_completion(uint32_t now);", r'''
  enum class AccessOperation {NONE,NODE};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  std::atomic<uint16_t> advertised_product_id_{0},advertised_composition_version_id_{0};
  std::atomic<uint8_t> advertised_firmware_major_{0},advertised_firmware_minor_{0},advertised_firmware_patch_{0},advertised_hardware_version_{0};
  std::atomic<bool> advertised_identity_valid_{false},identity_found_this_boot_{false},advertised_identity_save_pending_{false};
  std::atomic<uint32_t> mesh_rx_messages_{0};
  uint16_t identity_node_address_{1};
  bool completed{},last_success{};
  template<class T>void record_mesh_rssi_(const T&){}
  void request_refresh();
  bool node_event_(const struct Params*,uint32_t,bool success){completed=true;last_success=success;return true;}
  bool node_composition_event_(int,struct esp_ble_mesh_cfg_client_cb_param_t*);
  void test_completion(uint32_t now);
''')
harness = harness.replace("node_destination_{0}", "node_destination_{1}")
boundary = r'''
constexpr int ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT=1;
constexpr uint32_t ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET=0x8008,ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS=2;
using esp_ble_mesh_cfg_client_cb_event_t=int;
struct Buffer {uint8_t *data;size_t len;};
struct Params {struct {uint16_t addr;}ctx;uint32_t opcode;};
struct esp_ble_mesh_cfg_client_cb_param_t {Params *params;int error_code{};
  struct {struct {uint8_t page{};Buffer *composition_data;}comp_data_status;}status_cb;};
'''
main = r'''
}
int main(){
  using namespace esphome::steinel_mesh;
  assert(THRESHOLD_PROPERTY==0x2B&&RUN_PROPERTY==0x3C);
  SteinelMesh g;g.catalog_.count=1;auto &n=g.catalog_.nodes[0];auto &s=g.node_states_[0];
  n.address=1;n.product_id=0x1E79;n.element_count=1;n.selected=true;n.elements[0].capabilities=protocol::ONOFF;
  uint8_t data[]={0x63,0x05,0x79,0x1E,0x83,0x08,0,0,0,0,0,0,1,0,0,0x10};
  Buffer buffer{data,sizeof(data)};Params params{{1},ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET};
  esp_ble_mesh_cfg_client_cb_param_t reply{};reply.params=&params;reply.status_cb.comp_data_status.composition_data=&buffer;
  unsigned compositions=0,controls=0;
  for(uint32_t minute=0;minute<1440;++minute){
    test_now=1000+minute*60000;
    if(minute==5||minute==400){
      s.sensor_probes[0]=s.lux_probes[0]=s.lc_probes[0]=15;
      s.value_poll_at[3]=test_now;
      g.request_refresh();
      assert(s.composition_at==0&&s.value_poll_at[3]==0&&s.sensor_probes[0]==0&&s.lux_probes[0]==0&&s.lc_probes[0]==0);
    }
    g.sent=false;g.poll_nodes_(test_now);
    if(!g.sent)continue;
    auto r=g.captured;
    if(r.kind==COMPOSITION){
      ++compositions;g.active_node_request_=r;g.access_operation_=SteinelMesh::AccessOperation::NODE;
      if(minute>=400)data[4]=0x84;
      assert(g.node_composition_event_(ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT,&reply)&&g.last_success);
      assert(s.composition_matches&&s.composition_at==test_now+IDENTITY_INTERVAL);
    }else{++controls;s.last_seen=test_now;s.value_seen[0]=test_now;}
  }
  assert(compositions>=5&&controls>0&&std::strcmp(s.firmware,"1.2.4")==0);
  // Failed refresh leaves the last authenticated identity and working functions intact.
  g.active_node_request_={0,0,COMPOSITION,0,0,0};g.access_operation_=SteinelMesh::AccessOperation::NODE;
  reply.error_code=1;
  assert(g.node_composition_event_(0,&reply)&&!g.last_success&&s.composition_matches);
  g.node_completion_pending_=true;g.node_completion_success_=false;g.test_completion(test_now);
  assert(s.retry_at==0&&std::strcmp(s.firmware,"1.2.4")==0);
  reply.error_code=0;
  for(uint16_t product : {0x1B1B,0x1E74}) {
    n.product_id=product;data[2]=product&255;data[3]=product>>8;
    assert(g.node_composition_event_(1,&reply)&&s.composition_matches&&std::strcmp(s.firmware,"1.2.4")==0);
  }
  // A confirmed composition mismatch still blocks the device.
  data[2]^=1;
  assert(g.node_composition_event_(1,&reply)&&!s.composition_matches);
  data[2]^=1;
  // A VID change must not reapply an old advertisement, including on the next refresh.
  n.nightmatiq=true;n.product_id=0x1DCE;data[2]=0xCE;data[3]=0x1D;data[4]=1;
  s.composition_checked=false;g.advertised_identity_valid_=true;g.identity_found_this_boot_=true;
  g.advertised_product_id_=0x1DCE;g.advertised_firmware_major_=9;
  g.advertised_composition_version_id_=0;
  assert(g.node_composition_event_(1,&reply)&&std::strcmp(s.firmware,"9.0.0")==0);
  data[4]=2;
  assert(g.node_composition_event_(1,&reply)&&s.firmware[0]==0);
  assert(g.node_composition_event_(1,&reply)&&s.firmware[0]==0);
  // Re-reading after clock wrap still schedules a bounded six-hour interval.
  test_now=0xFFFFFFF0U;
  assert(g.node_composition_event_(1,&reply)&&protocol::deadline_pending(test_now,s.composition_at));
  std::printf("Identity refresh: %u reads/24h, manual and periodic refresh, changed firmware, failed replies and clock wrap passed\n",compositions);
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-identity-refresh-") as directory:
        binary = str(Path(directory) / "identity")
        code = harness + boundary + poll["queue"]["REQUESTS"] + poll["queue"]["COMPLETION"]
        code += refresh + composition + poll["source"][poll["start"]:poll["end"]] + main
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary], input=code, text=True, check=True)
        subprocess.run([binary],check=True)
