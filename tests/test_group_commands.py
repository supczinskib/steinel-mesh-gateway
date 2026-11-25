"""Exercise production group admission and SDK dispatch without a radio."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome" / "components" / "steinel_mesh"
SOURCE = (COMPONENT / "steinel_groups.cpp").read_text(encoding="utf-8")
SOURCE = SOURCE[SOURCE.index("namespace esphome::steinel_mesh {"):]

HARNESS = r'''
#include "mesh_protocol.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#define ESP_LOGW(...) ((void) 0)
#define ESP_LOGE(...) ((void) 0)
namespace esphome::steinel_mesh {
uint32_t clock_now = 1000;
uint32_t millis() { return clock_now; }
enum { ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET=1, ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET,
  ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET,
  ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK, ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK,
  ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET_UNACK };
struct Model { int kind; };
struct Buffer { uint8_t *data{}, *__buf{}; size_t size{}, len{}; };
struct Params {
  struct { uint16_t net_idx{}, app_idx{}, addr{}; uint8_t send_ttl{}; } ctx;
  uint32_t msg_timeout{}, opcode{}; Model *model{};
};
using esp_ble_mesh_client_common_param_t = Params;
struct esp_ble_mesh_generic_client_set_state_t { struct { uint8_t onoff{}, tid{}; } onoff_set; };
struct esp_ble_mesh_light_client_set_state_t {
  struct { uint16_t lightness{}; uint8_t tid{}; } lightness_set;
  struct { uint8_t mode{}; } lc_mode_set;
  struct { uint16_t property_id{}; Buffer *property_value{}; } lc_property_set;
};
using esp_err_t = int;
Params captured; uint32_t captured_value{}, captured_property{}, captured_tid{}; int sends=0, sdk_result=0;
int esp_ble_mesh_generic_client_set_state(Params *p, esp_ble_mesh_generic_client_set_state_t *v) {
  captured=*p; captured_value=v->onoff_set.onoff; captured_tid=v->onoff_set.tid; ++sends; return sdk_result;
}
int esp_ble_mesh_light_client_set_state(Params *p, esp_ble_mesh_light_client_set_state_t *v) {
  captured=*p; ++sends;
  if(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK) {
    captured_value=v->lightness_set.lightness; captured_tid=v->lightness_set.tid;
  } else if(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK) captured_value=v->lc_mode_set.mode;
  else {
    captured_property=v->lc_property_set.property_id;
    auto *b=v->lc_property_set.property_value;
    assert(b && b->len==3 && b->size==3 && b->data==b->__buf);
    captured_value=protocol::le24(b->data);
  }
  return sdk_result;
}
class SteinelMesh {
public:
  enum class NodeCommand : uint8_t { ONOFF, BRIGHTNESS, AUTO, THRESHOLD, RUN_TIME, MODE };
  enum class AccessOperation { NONE, NODE, OTHER };
  enum class ControlKind { NONE, OTHER };
  struct GroupRequest { uint16_t address; NodeCommand command; uint32_t value; };
  struct NodeRequest { uint8_t kind{}; } active_node_request_;
  bool catalog_valid_=true;
  std::atomic<bool> mesh_ready_{true}, reboot_pending_{false}, node_completion_pending_{false},
      composition_query_in_flight_{false}, node_read_obsolete_{false};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  std::atomic<uint32_t> access_opcode_{0};
  bool node_write_pending_=false, control_pending=false;
  ControlKind control_kind_=ControlKind::NONE;
  uint8_t poll_stage_=0, group_queue_count_=0, tid_=0;
  uint32_t node_send_at_=0, node_next_request_at_=0;
  struct { uint16_t net_key_index=2, app_key_index=3; } config_;
  std::mutex node_mutex_;
  std::array<GroupRequest,8> group_queue_{};
  std::array<uint8_t,3> group_property_storage_{};
  Buffer group_property_buffer_{};
  Model model_{};
  Model *node_model_(uint8_t kind) { model_.kind=kind; return &model_; }
  bool control_request_pending_() const { return control_pending; }
  uint8_t next_tid_() { return ++tid_; }
  bool record_send_result_(int result) { return result==0; }
  bool queue_group_command(uint16_t,NodeCommand,uint32_t);
  bool advance_group_commands_(uint32_t);
};
}
'''

MAIN = r'''
int main() {
  using namespace esphome::steinel_mesh;
  using C = SteinelMesh::NodeCommand;
  SteinelMesh g;
  assert(!g.queue_group_command(0xBFFF,C::ONOFF,1));
  assert(!g.queue_group_command(0xFF00,C::ONOFF,1));
  assert(!g.queue_group_command(0xFFFF,C::ONOFF,1));
  assert(!g.queue_group_command(0xC000,C::ONOFF,2));
  assert(!g.queue_group_command(0xC000,C::AUTO,2));
  assert(!g.queue_group_command(0xC000,C::BRIGHTNESS,65536));
  assert(!g.queue_group_command(0xC000,C::THRESHOLD,0xFFFFFF));
  assert(!g.queue_group_command(0xC000,C::RUN_TIME,0xFFFFFF));
  assert(!g.queue_group_command(0xC000,C::MODE,0));
  assert(g.queue_group_command(0xC000,C::ONOFF,1));
  assert(g.queue_group_command(0xFEFF,C::BRIGHTNESS,65535));
  assert(g.queue_group_command(0xC001,C::AUTO,0));
  assert(g.queue_group_command(0xC002,C::THRESHOLD,0xFFFFFE));
  assert(g.queue_group_command(0xC003,C::RUN_TIME,123456));
  assert(g.advance_group_commands_(clock_now));
  assert(captured.ctx.addr==0xC000 && captured.ctx.net_idx==2 && captured.ctx.app_idx==3 &&
         captured.ctx.send_ttl==7 && captured.model->kind==7 &&
         captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK && captured_value==1 && captured_tid==1);
  assert(!g.advance_group_commands_(clock_now+199));
  clock_now+=200; assert(g.advance_group_commands_(clock_now));
  assert(captured.ctx.addr==0xFEFF && captured.model->kind==8 &&
         captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK && captured_value==65535 && captured_tid==2);
  clock_now+=200; assert(g.advance_group_commands_(clock_now));
  assert(captured.model->kind==9 && captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK && captured_value==0);
  clock_now+=200; assert(g.advance_group_commands_(clock_now));
  assert(captured.model->kind==10 && captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET_UNACK &&
         captured_property==0x002B && captured_value==0xFFFFFE);
  clock_now+=200; assert(g.advance_group_commands_(clock_now));
  assert(captured_property==0x003C && captured_value==123456);
  for(int i=0;i<8;i++) assert(g.queue_group_command(0xC001,C::ONOFF,i%2));
  assert(!g.queue_group_command(0xC001,C::ONOFF,1));
  clock_now+=200; g.access_operation_=SteinelMesh::AccessOperation::OTHER;
  assert(!g.advance_group_commands_(clock_now) && g.group_queue_count_==8);
  g.access_operation_=SteinelMesh::AccessOperation::NODE; g.active_node_request_.kind=0;
  assert(!g.advance_group_commands_(clock_now));
  g.active_node_request_.kind=1; g.access_opcode_=ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET;
  assert(g.advance_group_commands_(clock_now));
  assert(g.node_read_obsolete_ && g.access_operation_==SteinelMesh::AccessOperation::NODE &&
         g.access_opcode_==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET);
  g.mesh_ready_=false; assert(!g.advance_group_commands_(clock_now) && g.group_queue_count_==0);
  assert(!g.queue_group_command(0xC000,C::ONOFF,1));
  g.mesh_ready_=true; clock_now+=200; sdk_result=1;
  assert(g.queue_group_command(0xC000,C::AUTO,1));
  assert(g.advance_group_commands_(clock_now) && g.group_queue_count_==0);
  clock_now+=200; g.node_read_obsolete_=false; g.access_opcode_=123;
  assert(g.queue_group_command(0xC000,C::AUTO,1));
  assert(g.advance_group_commands_(clock_now) && !g.node_read_obsolete_);
}
'''

WEB = (COMPONENT / "steinel_web.cpp").read_text(encoding="utf-8")
API_SOURCE = WEB[WEB.index("void SteinelMesh::handle_group_control_("):WEB.index("void SteinelMesh::handleBody(")]
API_HARNESS = r'''
#include "mesh_protocol.h"
#include <cassert>
#include <map>
#include <string>
namespace esphome::steinel_mesh {
struct AsyncWebServerRequest {
  std::map<std::string,std::string> args;
  int status=0; std::string message;
  std::string arg(const char *key) { return args[key]; }
};
void send_json_(AsyncWebServerRequest *r,int status,const std::string &message) {
  r->status=status; r->message=message;
}
class SteinelMesh {
public:
  enum class NodeCommand : uint8_t { ONOFF, BRIGHTNESS, AUTO, THRESHOLD, RUN_TIME, MODE };
  bool ready=true; uint16_t address=0; uint32_t value=0; NodeCommand command{};
  bool parse_u32_(const std::string &s,uint32_t min,uint32_t max,uint32_t &out) {
    return protocol::decimal_u32(s.data(),s.size(),min,max,out);
  }
  bool queue_group_command(uint16_t a,NodeCommand c,uint32_t v) {
    address=a; command=c; value=v; return ready;
  }
  void handle_group_control_(AsyncWebServerRequest *);
};
'''
API_MAIN = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  SteinelMesh g; AsyncWebServerRequest r;
  r.args={{"address","49152"},{"command","output"},{"value","1"}};
  g.handle_group_control_(&r);
  assert(r.status==200 && g.address==0xC000 && g.value==1 &&
         g.command==SteinelMesh::NodeCommand::ONOFF && r.message.find("not confirmed")!=std::string::npos);
  r.args["address"]="65279"; r.args["command"]="run_time"; r.args["value"]="16777214";
  g.handle_group_control_(&r); assert(r.status==200 && g.address==0xFEFF && g.value==0xFFFFFE);
  for (const char *address : {"49151","65280","0xC000",""}) {
    r.args["address"]=address; g.handle_group_control_(&r); assert(r.status==400);
  }
  r.args["address"]="49152";
  for (const auto &item : {std::pair<const char *,const char *>{"output","2"},
      {"automatic","2"},{"brightness","65536"},{"threshold","16777215"},
      {"run_time","-1"},{"mode","0"}}) {
    r.args["command"]=item.first; r.args["value"]=item.second;
    g.handle_group_control_(&r); assert(r.status==400);
  }
  r.args["command"]="automatic"; r.args["value"]="1"; g.ready=false;
  g.handle_group_control_(&r); assert(r.status==409);
}
'''


if __name__ == "__main__":
    assert 'url == "/steinel/group"' in WEB
    with tempfile.TemporaryDirectory(prefix="steinel-groups-") as directory:
        source = Path(directory) / "groups.cpp"
        binary = Path(directory) / ("groups.exe" if os.name == "nt" else "groups")
        source.write_text(HARNESS + SOURCE + MAIN, encoding="utf-8")
        flags = (["-static-libstdc++", "-static-libgcc"] if os.name == "nt"
                 else ["-fsanitize=address,undefined"])
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        *flags, "-I", str(COMPONENT), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        source.write_text(API_HARNESS + API_SOURCE + API_MAIN, encoding="utf-8")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        *flags, "-I", str(COMPONENT), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("Group API, admission, queue and SDK transport passed")
