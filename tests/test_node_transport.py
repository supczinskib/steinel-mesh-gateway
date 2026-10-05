"""Check actual request construction at the SDK boundary, without a radio."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "esphome/components/steinel_mesh/steinel_nodes.cpp").read_text()
START = SOURCE.index("bool SteinelMesh::send_node_request_(")
END = SOURCE.index("void SteinelMesh::finish_node_request_(", START)
REQUESTS = SOURCE[SOURCE.index("enum Request :"):SOURCE.index("template<class Base>")]
CONSTANTS = sorted(set(re.findall(r"\bESP_BLE_MESH_[A-Z0-9_]+", SOURCE[START:END])))
HARNESS = r'''
#include "mesh_protocol.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
namespace esphome::steinel_mesh {
uint32_t millis() { return 1000; }
struct Model { unsigned id; uint16_t keys[1]{}; };
struct Buffer { uint8_t *data{}, *__buf{}; size_t size{}, len{}; };
struct esp_ble_mesh_client_common_param_t {
  struct { uint16_t net_idx{}, app_idx{}, addr{}; uint8_t send_ttl{}; } ctx;
  uint32_t opcode{}, msg_timeout{}; Model *model{};
};
struct esp_ble_mesh_generic_client_get_state_t {};
struct esp_ble_mesh_generic_client_set_state_t { struct { uint8_t onoff{}, tid{}; } onoff_set; };
struct esp_ble_mesh_light_client_get_state_t { struct { uint16_t property_id{}; } lc_property_get; };
struct esp_ble_mesh_light_client_set_state_t {
  struct { uint16_t lightness{}; uint8_t tid{}; } lightness_set;
  struct { uint8_t mode{}; } lc_mode_set;
  struct { uint16_t property_id{}; Buffer *property_value{}; } lc_property_set;
};
struct esp_ble_mesh_sensor_client_get_state_t {
  struct { bool op_en{}; uint16_t property_id{}; } sensor_get;
  struct { bool op_en{}; } descriptor_get;
};
struct esp_ble_mesh_cfg_client_get_state_t { struct { uint8_t page{}; } comp_data_get; };
struct esp_ble_mesh_time_scene_client_set_state_t { struct { uint16_t scene_number{}; uint8_t tid{}; } scene_recall; };
using esp_err_t = int;
esp_ble_mesh_client_common_param_t captured;
uint32_t value{}, property{}, tid{}; int sdk_result = 0;
int capture(esp_ble_mesh_client_common_param_t *p) { captured = *p; return sdk_result; }
int esp_ble_mesh_config_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_cfg_client_get_state_t *) { return capture(p); }
int esp_ble_mesh_generic_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_generic_client_get_state_t *) { return capture(p); }
int esp_ble_mesh_generic_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_generic_client_set_state_t *s) { value=s->onoff_set.onoff; tid=s->onoff_set.tid; return capture(p); }
int esp_ble_mesh_sensor_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_sensor_client_get_state_t *s) { property=s->sensor_get.property_id; value=s->sensor_get.op_en; return capture(p); }
int esp_ble_mesh_light_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_light_client_get_state_t *s) { property=s->lc_property_get.property_id; return capture(p); }
int esp_ble_mesh_light_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_light_client_set_state_t *s);
int esp_ble_mesh_time_scene_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_time_scene_client_set_state_t *s) { value=s->scene_recall.scene_number; tid=s->scene_recall.tid; return capture(p); }
class SteinelMesh {
 public:
  protocol::DeviceDiagnostics device_diagnostics_{};
  protocol::StateTrace state_trace_{};
  template<class P>bool diagnostic_reply_(const P*,uint32_t,bool,uint16_t=0){return false;}
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0){}
  struct NodeRequest { uint8_t node, element, kind, attempts, tid; uint32_t value; uint8_t step{0}; bool diagnostic{false}; };
  bool diagnostic_request_allowed_(const NodeRequest &request) { return request.diagnostic && !(request.kind>=7&&request.kind<=11)&&request.kind!=14; }
  struct State { bool composition_checked{true},composition_matches{true}; uint32_t last_seen{1000},stale_after{180000}; uint16_t supported{255}; int8_t on{-1},automatic{-1}; };
  struct Config { uint16_t net_key_index{2}, app_key_index{3}; } config_;
  enum class AccessOperation { NODE };
  protocol::Catalog catalog_; std::array<State, protocol::MAX_NODES> node_states_{};
  std::mutex node_mutex_;
  std::atomic<bool> mesh_ready_{true}, reboot_pending_{false};
  std::atomic<uint32_t> access_deadline_{0}, node_destination_{0}, node_expected_status_{0}, node_expected_property_{0};
  std::array<uint8_t, 3> node_property_storage_{}; Buffer node_property_buffer_;
  NodeRequest active_node_request_{}; Model model_{}; bool failed{false};
  Model *node_model_(uint8_t kind) { model_.id=kind; return &model_; }
  bool begin_access_operation_(AccessOperation, uint32_t) { return true; }
  bool record_access_send_result_(AccessOperation, uint32_t, int result) { return result==0; }
  bool complete_access_operation_(uint32_t, bool) { return true; }
  void finish_node_request_(bool success) { failed=!success; }
  bool send_node_request_(const NodeRequest &);
};
'''
SDK_SET = r'''
int esp_ble_mesh_light_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_light_client_set_state_t *s) {
  if (p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET) { value=s->lightness_set.lightness; tid=s->lightness_set.tid; }
  else if (p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET || p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK) value=s->lc_mode_set.mode;
  else { property=s->lc_property_set.property_id; const auto *b=s->lc_property_set.property_value;
    assert(b && b->len==3 && b->size==3); value=b->data[0] | uint32_t(b->data[1])<<8 | uint32_t(b->data[2])<<16; }
  return capture(p);
}
'''
MAIN = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(POLL_INTERVAL==30000 && STALE_INTERVAL==180000 && POLL_STEPS==6+protocol::MAX_ELEMENTS*5 && IDENTITY_INTERVAL==21600000);
  SteinelMesh g; g.catalog_.count=1;
  auto &n=g.catalog_.nodes[0]; n.address=10; n.selected=true; n.element_count=4; n.scene=42;
  n.elements[0].capabilities=protocol::ONOFF | protocol::SCENE;
  n.elements[1].capabilities=protocol::LC; n.elements[2].capabilities=protocol::SENSOR;
  n.elements[3].capabilities=protocol::ONOFF | protocol::LIGHTNESS;
  auto send=[&](uint8_t kind, uint8_t element, uint32_t v=0, uint8_t step=0) {
    assert(g.send_node_request_({0,element,kind,0,37,v,step}));
    assert(captured.ctx.net_idx==2 && captured.ctx.send_ttl==7);
  };
  send(COMPOSITION,0); assert(captured.ctx.app_idx==ESP_BLE_MESH_KEY_DEV && captured.model->keys[0]==ESP_BLE_MESH_KEY_DEV && captured.msg_timeout==4000);
  send(ON,3);assert(captured.msg_timeout==3000);
  send(AUTO,1);assert(captured.msg_timeout==3000);
  send(SET_ON,3,1); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK && captured.ctx.addr==11 && value==0);
  send(SET_ON,3,1,1); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET && captured.ctx.addr==13 && value==1 && tid==37);
  send(SET_LEVEL,3,65535); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK && captured.ctx.addr==11 && value==0);
  send(SET_LEVEL,3,65535,1); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET && value==65535 && tid==37);
  send(SET_AUTO,1,1); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET && value==1);
  send(SET_THRESHOLD,1,123456); assert(property==0x002B && value==123456 && g.node_expected_property_==0x002B);
  send(SET_RUN,1,654321); assert(property==0x003C && value==654321);
  send(SENSOR_PROPERTY,2,0x004E); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_SENSOR_GET && property==0x004E && value==1);
  send(SENSOR,2); assert(value==0);
  send(DESCRIPTORS,2); assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_GET);
  n.nightmatiq=true;
  send(ON,3);assert(captured.msg_timeout==1200);
  send(AUTO,1);assert(captured.msg_timeout==1200);
  send(THRESHOLD,1);assert(captured.msg_timeout==3000);
  assert(!g.send_node_request_({0,1,SET_RUN,0,37,1000}));
  for (uint32_t mode=0; mode<3; ++mode) for (uint8_t step=0; step<4; ++step) {
    send(SET_MODE,1,mode,step);
    if (step < 2) assert(captured.ctx.addr==11 && captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK && value==(mode==0));
    else if (mode==0) assert(captured.ctx.addr==10 && captured.opcode==ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK && value==42 && tid==37);
    else assert(captured.ctx.addr==13 && captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK && value==(mode==1) && tid==37);
  }
  n.nightmatiq=false;
  g.catalog_.nodes[0].selected=false;
  assert(g.send_node_request_({0,0,COMPOSITION,0,0,0,0,true}));
  assert(g.send_node_request_({0,2,SENSOR,0,0,0,0,true}));
  assert(!g.send_node_request_({0,3,SET_ON,0,1,1,0,true}));
  assert(!g.send_node_request_({0,2,SENSOR,0,0,0}));
  g.catalog_.nodes[0].selected=true;
  g.node_states_[0].supported=0; assert(!g.send_node_request_({0,3,SET_ON,0,1,1}));
  g.node_states_[0].supported=255; g.node_states_[0].last_seen=0;
  assert(!g.send_node_request_({0,3,SET_ON,0,1,1}));
  g.node_states_[0].last_seen=1000; sdk_result=1;
  assert(!g.send_node_request_({0,3,SET_ON,0,1,1}) && g.failed);
  sdk_result=0;
  for(uint16_t product : {0x1B1B,0x1E74}) {
    n.company_id=0x0563;n.product_id=product;n.address=0x100;
    n.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS|protocol::SCENE|protocol::SCHEDULER;
    n.elements[1].capabilities=protocol::ONOFF|protocol::LC;
    n.elements[2].capabilities=n.elements[3].capabilities=protocol::SENSOR;
    send(SET_ON,0,1);
    assert(captured.ctx.addr==0x101&&captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK&&value==0);
    send(SET_ON,0,1,1);
    assert(captured.ctx.addr==0x100&&captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET&&value==1);
    send(SET_LEVEL,0,32768);
    assert(captured.ctx.addr==0x101&&captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK&&value==0);
    send(SET_LEVEL,0,32768,1);
    assert(captured.ctx.addr==0x100&&captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET&&value==32768);
    send(SET_AUTO,1,1);assert(captured.ctx.addr==0x101&&value==1);
    send(SENSOR,2);assert(captured.ctx.addr==0x102&&value==0);
    send(SENSOR,3);assert(captured.ctx.addr==0x103&&value==0);
    send(SENSOR_PROPERTY,3,0x004E);assert(captured.ctx.addr==0x103&&property==0x004E&&value==1);
  }
  std::puts("Actual transport: opcodes, destinations, TIDs, properties, mode sequence and send failure passed");
  std::puts("L 820 SC / L 830 SC transport: LC preparation, primary output and separate Sensor destinations passed");
}
'''

if __name__ == "__main__":
    constants = "enum { " + ", ".join(f"{name}={i+1}" for i, name in enumerate(CONSTANTS)) + " };\n"
    with tempfile.TemporaryDirectory(prefix="steinel-transport-") as directory:
        binary = str(Path(directory) / "transport")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=HARNESS + constants + REQUESTS + SDK_SET + SOURCE[START:END] + MAIN,
                       text=True, check=True)
        subprocess.run([binary], check=True)
