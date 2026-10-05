"""Exercise production write acknowledgements, including rejected values and transitions."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "esphome/components/steinel_mesh/steinel_nodes.cpp").read_text()
start = source.index("bool SteinelMesh::node_event_(")
end = source.index("bool SteinelMesh::node_sensor_event_(", start)
constants = sorted(set(re.findall(r"\bESP_BLE_MESH_[A-Z0-9_]+", source[start:end])))
requests = source[source.index("enum Request :"):source.index("template<class Base>")]
harness = r'''
#include "mesh_protocol.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#define ESP_LOGI(...) ((void)0)
namespace esphome::steinel_mesh {
uint32_t millis(){return 1000;}
struct esp_ble_mesh_client_common_param_t { struct { uint16_t addr; uint32_t recv_op; } ctx; uint32_t opcode; };
struct Buffer { uint8_t *data; size_t len; };
using esp_ble_mesh_generic_client_cb_event_t=int;
using esp_ble_mesh_light_client_cb_event_t=int;
struct esp_ble_mesh_generic_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params;int error_code{};
  struct { struct { bool op_en{};uint8_t present_onoff{},target_onoff{},remain_time{}; } onoff_status; } status_cb;
};
struct esp_ble_mesh_light_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params;int error_code{};
  union {
    struct { uint16_t property_id;Buffer *property_value; } lc_property_status;
    struct { uint8_t mode; } lc_mode_status;
    struct { bool op_en;uint8_t present_light_onoff,target_light_onoff,remain_time; } lc_light_onoff_status;
    struct { bool op_en;uint16_t present_lightness,target_lightness; } lightness_status;
  } status_cb;
};
class SteinelMesh {
 public:
  protocol::DeviceDiagnostics device_diagnostics_{};
  protocol::StateTrace state_trace_{};
  template<class P>bool diagnostic_reply_(const P*,uint32_t,bool,uint16_t=0){return false;}
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0){}
  enum class AccessOperation { NONE, NODE };
  struct State {
    bool composition_checked=true,composition_matches=true,dirty=false,brightness_known=false;
    uint16_t supported=0,brightness=0;int8_t on=-1,automatic=-1;
    uint32_t threshold=0,run_time=0,last_seen=0,value_seen[7]{};
  };
  struct NodeRequest { uint8_t node,element,kind,attempts,tid;uint32_t value;uint8_t step{};bool diagnostic{false}; };
  bool catalog_valid_=true,completed=false,last_success=false;
  protocol::Catalog catalog_;std::array<State,protocol::MAX_NODES> node_states_{};std::mutex node_mutex_;
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  std::atomic<uint16_t> node_destination_{10},node_expected_property_{0};
  std::atomic<uint32_t> access_opcode_{0},node_expected_status_{0},mesh_rx_messages_{0},mesh_generic_rx_{0},mesh_timeouts_{0};
  NodeRequest active_node_request_{};
  template<class T>void record_mesh_rssi_(const T&){}
  void finish_node_request_(bool success){completed=true;last_success=success;}
  void complete_access_operation_(uint32_t,bool){access_operation_=AccessOperation::NONE;}
  bool node_event_(const esp_ble_mesh_client_common_param_t*,uint32_t,bool,uint16_t=0);
  bool node_response_value_matches_(const esp_ble_mesh_client_common_param_t*,uint32_t,uint32_t)const;
  bool node_generic_event_(int,esp_ble_mesh_generic_client_cb_param_t*);
  bool node_light_event_(int,esp_ble_mesh_light_client_cb_param_t*);
};
'''
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(IDENTITY_INTERVAL==21600000);
  assert(POLL_INTERVAL==30000 && STALE_INTERVAL==180000 && POLL_STEPS>0);
  SteinelMesh g;g.catalog_.count=1;auto&n=g.catalog_.nodes[0];n.address=10;n.element_count=1;n.selected=true;
  n.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS|protocol::LC;
  esp_ble_mesh_client_common_param_t params{};params.ctx.addr=10;
  auto begin=[&](uint8_t kind,uint32_t value,uint8_t step,uint32_t status,uint16_t property=0){
    g.access_operation_=SteinelMesh::AccessOperation::NODE;g.active_node_request_={0,0,kind,0,1,value,step};
    g.access_opcode_=params.opcode=0x8000+kind;g.node_expected_status_=params.ctx.recv_op=status;
    g.node_expected_property_=property;g.completed=false;g.last_success=false;
  };
  esp_ble_mesh_light_client_cb_param_t light{};light.params=&params;
  begin(SET_AUTO,0,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS);light.status_cb.lc_mode_status.mode=1;
  assert(g.node_light_event_(0,&light) && g.completed && !g.last_success && g.node_states_[0].automatic==1);
  begin(SET_AUTO,0,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS);light.status_cb.lc_mode_status.mode=0;
  assert(g.node_light_event_(0,&light) && g.last_success);
  begin(SET_ON,1,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS);light.status_cb.lc_mode_status.mode=1;
  assert(g.node_light_event_(0,&light) && !g.last_success);
  begin(SET_MODE,0,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS);light.status_cb.lc_mode_status.mode=1;
  assert(g.node_light_event_(0,&light) && g.last_success);
  uint8_t data[]={0xC4,9,0};Buffer buffer{data,3};light.status_cb.lc_property_status={THRESHOLD_PROPERTY,&buffer};
  begin(SET_THRESHOLD,1000,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS,THRESHOLD_PROPERTY);
  assert(g.node_light_event_(0,&light) && !g.last_success && g.node_states_[0].threshold==2500);
  begin(SET_THRESHOLD,2500,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS,THRESHOLD_PROPERTY);
  assert(g.node_light_event_(0,&light) && g.last_success);
  begin(SET_RUN,3000,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS,RUN_PROPERTY);
  assert(g.node_light_event_(0,&light) && !g.completed); // unrelated property must not release the slot
  buffer.len=2;
  assert(g.node_light_event_(0,&light) && !g.completed);
  buffer.len=3;light.status_cb.lc_property_status.property_id=RUN_PROPERTY;
  data[0]=0xB8;data[1]=0x0B;
  assert(g.node_light_event_(0,&light) && g.completed && g.last_success);
  n.nightmatiq=true;
  g.node_states_[0].supported &= ~protocol::F_RUN_TIME;
  g.node_states_[0].run_time=protocol::UNKNOWN_24;g.node_states_[0].value_seen[4]=0;
  data[0]=data[1]=data[2]=0;
  begin(RUN,0,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS,RUN_PROPERTY);
  assert(g.node_light_event_(0,&light) && g.last_success);
  assert(!(g.node_states_[0].supported & protocol::F_RUN_TIME));
  assert(g.node_states_[0].run_time==protocol::UNKNOWN_24 && g.node_states_[0].value_seen[4]==0);
  n.nightmatiq=false;
  begin(RUN,0,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS,RUN_PROPERTY);
  assert(g.node_light_event_(0,&light) && g.last_success);
  assert((g.node_states_[0].supported & protocol::F_RUN_TIME) && g.node_states_[0].run_time==0);
  begin(SET_RUN,3000,0,ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS,RUN_PROPERTY);
  params.ctx.recv_op=0;
  assert(g.node_light_event_(ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT,&light) && g.completed && !g.last_success);
  begin(SET_LEVEL,5000,1,ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS);
  light.status_cb.lightness_status={true,100,5000};
  assert(g.node_light_event_(0,&light) && g.last_success && g.node_states_[0].brightness==100);
  begin(SET_LEVEL,5000,1,ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS);
  light.status_cb.lightness_status={false,100,5000};
  assert(g.node_light_event_(0,&light) && !g.last_success);
  esp_ble_mesh_generic_client_cb_param_t generic{};generic.params=&params;
  begin(SET_ON,1,1,ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS);
  generic.status_cb.onoff_status={false,0,1};
  assert(g.node_generic_event_(0,&generic) && !g.last_success);
  begin(SET_ON,1,1,ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS);
  generic.status_cb.onoff_status={true,0,1};
  assert(g.node_generic_event_(0,&generic) && g.last_success && g.node_states_[0].on==0);
  begin(SET_ON,1,1,ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS);params.ctx.addr=11;
  g.node_generic_event_(0,&generic);assert(!g.completed);
  // LC output publications must refresh the actual output, not LC Mode.
  g.access_operation_=SteinelMesh::AccessOperation::NONE;
  params.ctx.addr=10;params.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS;
  g.node_states_[0].automatic=1;g.node_states_[0].on=0;g.node_states_[0].value_seen[0]=0;
  light.status_cb.lc_light_onoff_status={false,1,0,0};
  assert(g.node_light_event_(0,&light));
  assert(g.node_states_[0].on==1 && g.node_states_[0].automatic==1 && g.node_states_[0].value_seen[0]==1000);
  light.status_cb.lc_light_onoff_status.present_light_onoff=0;
  assert(g.node_light_event_(ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT,&light));
  assert(g.node_states_[0].on==1 && g.node_states_[0].automatic==1);
  light.status_cb.lc_light_onoff_status.present_light_onoff=2;
  assert(g.node_light_event_(0,&light));assert(g.node_states_[0].on==1);
  params.ctx.addr=11;light.status_cb.lc_light_onoff_status.present_light_onoff=0;
  assert(!g.node_light_event_(0,&light));assert(g.node_states_[0].on==1);
  g.state_trace_.clear();
  params.ctx.addr=10;params.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS;
  generic.status_cb.onoff_status={true,0,1,0x41};
  assert(g.node_generic_event_(0,&generic));
  const auto &trace=g.state_trace_.entry(0);
  assert(g.state_trace_.captured && trace.before_on==1 && trace.after_on==0);
  assert(trace.before_auto==1 && trace.after_auto==1 && trace.length==3);
  assert(trace.raw[0]==0 && trace.raw[1]==1 && trace.raw[2]==0x41);
  assert(trace.request==params.opcode && trace.opcode==params.ctx.recv_op);
  std::puts("Production callbacks: requested values, LC preparation, unrelated responses and transition targets passed");
}
'''
if __name__ == "__main__":
    definitions = "enum { " + ", ".join(f"{name}={i+1}" for i, name in enumerate(constants)) + " };\n"
    with tempfile.TemporaryDirectory(prefix="steinel-callbacks-") as directory:
        binary = str(Path(directory) / "callbacks")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=harness+definitions+requests+source[start:end]+main,text=True,check=True)
        subprocess.run([binary],check=True)
