"""Regression contracts exercised across production transport and callbacks.

LC preparation cannot gate output on a missing Mode Status. Sustained writes
must leave polling slots. Valid diagnostic replies must prevent address recovery
without changing imported selection or Home Assistant device state.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "esphome/components/steinel_mesh"
nodes = (DIRECTORY / "steinel_nodes.cpp").read_text()
mesh = (DIRECTORY / "steinel_mesh.cpp").read_text()
header = (DIRECTORY / "steinel_mesh.h").read_text()
transport = runpy.run_path(str(ROOT / "tests/test_node_transport.py"))
callbacks = runpy.run_path(str(ROOT / "tests/test_node_callbacks.py"))


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


production = section(nodes, "enum Request :", "template<class Base>")
production += section(nodes, "bool SteinelMesh::diagnostic_request_allowed_(", "bool SteinelMesh::queue_node_command(")
production += section(nodes, "bool SteinelMesh::queue_node_command(", "void SteinelMesh::advance_nodes_(")
completion = nodes.index("  if (this->node_completion_pending_.exchange(false))")
polling = nodes.index("void SteinelMesh::poll_nodes_(", completion)
production += "void SteinelMesh::dispatch_(uint32_t now) {\n" + nodes[completion:polling]
production += nodes[polling:nodes.rindex("}  // namespace")]
production += section(mesh, "bool SteinelMesh::record_send_result_(", "void SteinelMesh::bind_model_(")
production += section(mesh, "void SteinelMesh::advance_address_recovery_(", "void SteinelMesh::keys_bound_(")

# Only SDK types, send calls, entity registration and RSSI hardware access are doubled.
sdk = transport["HARNESS"][:transport["HARNESS"].index("class SteinelMesh")]
sdk = sdk.replace("uint32_t millis() { return 1000; }", "uint32_t test_now=1000;\nuint32_t millis() { return test_now; }")
sdk = sdk.replace("uint8_t send_ttl{};", "uint8_t send_ttl{}; uint32_t recv_op{}; int8_t recv_rssi{-51};")
sdk = sdk.replace("#include <mutex>", "#include <mutex>\n#include <string>\n#include <vector>")
sdk += section(callbacks["harness"], "using esp_ble_mesh_generic_client_cb_event_t", "class SteinelMesh")
sdk += r'''
using esp_ble_mesh_sensor_client_cb_event_t=int;
using esp_ble_mesh_time_scene_client_cb_event_t=int;
using esp_ble_mesh_cfg_client_cb_event_t=int;
struct esp_ble_mesh_sensor_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params; int error_code{};
  struct { struct { Buffer *descriptor{}; } descriptor_status;
           struct { Buffer *marshalled_sensor_data{}; } sensor_status; } status_cb;
};
struct esp_ble_mesh_time_scene_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params; int error_code{};
  struct { struct { uint8_t status_code{}; bool op_en{};
                   uint16_t current_scene{},target_scene{}; } scene_status; } status_cb;
};
struct esp_ble_mesh_cfg_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params; int error_code{};
  struct { struct { uint8_t page{}; Buffer *composition_data{}; } comp_data_status; } status_cb;
};
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr uint32_t MESSAGE_TIMEOUT_MS=1200, AUTO_ADDRESS_RECOVERY_DELAY_MS=60000,
                  AUTO_ADDRESS_MIN_ACCEPTED_TX=10,AUTO_ADDRESS_MIN_TIMEOUTS=10;
constexpr int ESP_OK=0;
class SteinelMesh {
 public:
  struct NodeEntities {};
'''
sdk += section(header, "  struct NodeState {", "  struct StoredFunctions {")
sdk += r'''
  enum class NodeCommand : uint8_t { ONOFF, BRIGHTNESS, AUTO, THRESHOLD, RUN_TIME, MODE };
  enum class AccessOperation { NONE, NODE };
  enum class ControlKind { NONE };
  static constexpr uint8_t NODE_QUEUE_SIZE=16;
  protocol::Catalog catalog_{};
  std::array<NodeState,protocol::MAX_NODES> node_states_{};
  std::array<NodeRequest,NODE_QUEUE_SIZE> node_queue_{};
  uint8_t node_queue_count_{0},tid_{0},node_poll_index_{0},node_poll_step_{0},node_priority_burst_{0};
  uint8_t node_write_burst_{0};
  uint32_t node_next_request_at_{0},node_poll_at_{0};
  NodeRequest active_node_request_{};
  std::atomic<bool> node_completion_pending_{false},node_completion_success_{false};
  std::mutex node_mutex_;
  std::atomic<uint16_t> node_destination_{0},node_expected_property_{0};
  std::atomic<uint32_t> node_expected_status_{0},access_opcode_{0},access_deadline_{0};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE},access_last_completed_{AccessOperation::NONE};
  std::atomic<bool> access_last_success_{false};
  std::atomic<uint32_t> mesh_tx_attempts_{0},mesh_tx_accepted_{0},mesh_tx_errors_{0},
                        mesh_rx_messages_{0},mesh_generic_rx_{0},mesh_sensor_rx_{0},mesh_timeouts_{0};
  std::atomic<int> mesh_last_tx_error_{0};
  std::array<uint8_t,3> node_property_storage_{}; Buffer node_property_buffer_{};
  struct Config { uint16_t net_key_index{2},app_key_index{3},onoff_address{1}; } config_;
  Model model_{};
  Model *node_model_(uint8_t kind) { model_.id=kind;return &model_; }
  bool catalog_valid_{true},legacy_profile_{false},configured_{true},mesh_mode_enabled_{true},
       address_policy_valid_{true},address_recovery_attempted_this_boot_{false};
  std::atomic<bool> mesh_ready_{true},reboot_pending_{false},composition_query_in_flight_{false},cloud_busy_{false};
  ControlKind control_kind_{ControlKind::NONE}; unsigned poll_stage_{0},rssi_samples{0},rotations{0};
  uint32_t mesh_ready_at_{1};
  bool control_request_pending_() const { return false; }
  bool sync_node_functions_() { return false; }
  bool current_address_confirmed_() const { return mesh_rx_messages_.load()!=0; }
  bool rotate_local_address_(std::string &) { ++rotations;return true; }
  void set_status_(const std::string &) {}
  template<class T> void record_mesh_rssi_(const T &) { ++rssi_samples; }
  uint8_t next_tid_() { return ++tid_; }
  protocol::DeviceDiagnostics device_diagnostics_{};
  protocol::StateTrace state_trace_{};
  std::atomic<bool> advertised_identity_valid_{false},identity_found_this_boot_{false},advertised_identity_save_pending_{false};
  std::atomic<uint16_t> advertised_product_id_{0},advertised_composition_version_id_{0};
  std::atomic<uint8_t> advertised_firmware_major_{0},advertised_firmware_minor_{0},advertised_firmware_patch_{0},advertised_hardware_version_{0};
  uint16_t identity_node_address_{0};
  bool queue_node_command(uint16_t,NodeCommand,uint32_t);
  bool send_node_request_(const NodeRequest &);
  void finish_node_request_(bool);
  bool node_event_(const esp_ble_mesh_client_common_param_t *,uint32_t,bool,uint16_t=0);
  bool node_response_value_matches_(const esp_ble_mesh_client_common_param_t *,uint32_t,uint32_t) const;
  bool node_generic_event_(int,esp_ble_mesh_generic_client_cb_param_t *);
  bool node_light_event_(int,esp_ble_mesh_light_client_cb_param_t *);
  bool node_sensor_event_(int,esp_ble_mesh_sensor_client_cb_param_t *);
  bool node_scene_event_(int,esp_ble_mesh_time_scene_client_cb_param_t *);
  bool node_composition_event_(int,esp_ble_mesh_cfg_client_cb_param_t *);
  bool diagnostic_request_allowed_(const NodeRequest &);
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0);
  bool diagnostic_reply_(const esp_ble_mesh_client_common_param_t *,uint32_t,bool,uint16_t=0);
  bool next_diagnostic_request_(uint32_t,NodeRequest &);
  bool record_send_result_(int);
  bool begin_access_operation_(AccessOperation,uint32_t);
  bool record_access_send_result_(AccessOperation,uint32_t,int);
  bool complete_access_operation_(uint32_t,bool);
  void expire_access_operation_(uint32_t);
  void advance_address_recovery_(uint32_t);
  void dispatch_(uint32_t);
  void poll_nodes_(uint32_t);
};
'''

constants = set(re.findall(r"\bESP_BLE_MESH_[A-Z0-9_]+", production))
constants.update({"ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK",
                  "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK", "ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK"})
definitions = "enum { " + ", ".join(f"{name}={i+1}" for i, name in enumerate(sorted(constants))) + " };\n"
sdk_set = transport["SDK_SET"].replace(
    "p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET)",
    "(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET || p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK))")

MAIN = r'''
}
using namespace esphome::steinel_mesh;
using Command=SteinelMesh::NodeCommand;
uint8_t physical_auto[16]{};
uint8_t physical_on[16]{};
uint16_t physical_level[16]{};
void setup(SteinelMesh &g,unsigned count,bool lighting=true) {
  test_now=1000;sdk_result=0;g.catalog_.count=count;
  for(unsigned i=0;i<count;++i) {
    auto &n=g.catalog_.nodes[i];n.address=1+8*i;n.selected=true;n.element_count=lighting?3:1;n.scene=42;
    n.company_id=0x0563;n.product_id=0x1DCE;n.nightmatiq=lighting;
    n.elements[0].capabilities=protocol::ONOFF|(lighting?protocol::LIGHTNESS|protocol::SCENE:0);
    if(lighting){n.elements[1].capabilities=protocol::LC;n.elements[2].capabilities=protocol::SENSOR;}
    auto &s=g.node_states_[i];s.last_seen=test_now;s.composition_checked=s.composition_matches=true;
    physical_auto[i]=lighting?1:0;
    physical_on[i]=0;physical_level[i]=0;
    s.composition_at=test_now+IDENTITY_INTERVAL;s.supported=lighting?255:protocol::F_OUTPUT;
    s.on=0;s.brightness_known=lighting;s.automatic=lighting?1:-1;
    s.value_seen[0]=s.value_seen[1]=s.value_seen[2]=test_now;
    std::fill(std::begin(s.sensor_probes),std::end(s.sensor_probes),15);
    std::fill(std::begin(s.lux_probes),std::end(s.lux_probes),5);
  }
}
void reply(SteinelMesh &g,bool drop_lc_set=true) {
  if(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK) {
    physical_auto[g.active_node_request_.node]=value;return;
  }
  if(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
    physical_on[g.active_node_request_.node]=value;return;
  }
  if(captured.opcode==ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK) {
    physical_on[g.active_node_request_.node]=1;return;
  }
  if(g.access_operation_!=SteinelMesh::AccessOperation::NODE)return;
  auto params=captured;
  const uint32_t opcode=params.opcode;
  params.ctx.recv_op=g.node_expected_status_.load();
  if(opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET&&drop_lc_set) {
    test_now=g.access_deadline_.load();g.expire_access_operation_(test_now);return;
  }
  if(params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
    if(opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET)physical_on[g.active_node_request_.node]=value;
    esp_ble_mesh_generic_client_cb_param_t p{};p.params=&params;
    p.status_cb.onoff_status.present_onoff=physical_on[g.active_node_request_.node];
    assert(g.node_generic_event_(0,&p));
  }else if(params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_SCENE_STATUS) {
    physical_on[g.active_node_request_.node]=1;
    esp_ble_mesh_time_scene_client_cb_param_t p{};p.params=&params;p.status_cb.scene_status.current_scene=42;
    assert(g.node_scene_event_(0,&p));
  }else if(params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS || params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS) {
    uint8_t data[]={0xC4,0x09,0xC4,0x09,0};Buffer b{};b.data=data;b.len=sizeof(data);
    esp_ble_mesh_sensor_client_cb_param_t p{};p.params=&params;
    p.status_cb.sensor_status.marshalled_sensor_data=&b;p.status_cb.descriptor_status.descriptor=&b;
    g.node_sensor_event_(0,&p);
    if(g.access_operation_==SteinelMesh::AccessOperation::NODE){test_now=g.access_deadline_;g.expire_access_operation_(test_now);}
  }else if(params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS) {
    test_now=g.access_deadline_;g.expire_access_operation_(test_now);
  }else {
    esp_ble_mesh_light_client_cb_param_t p{};p.params=&params;
    uint8_t data[]={uint8_t(value),uint8_t(value>>8),uint8_t(value>>16)};Buffer b{};b.data=data;b.len=3;
    if(params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS)
      p.status_cb.lc_mode_status.mode=opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET?value:physical_auto[g.active_node_request_.node];
    else if(params.ctx.recv_op==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
      if(opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET)physical_level[g.active_node_request_.node]=value;
      p.status_cb.lightness_status.present_lightness=physical_level[g.active_node_request_.node];
    }else p.status_cb.lc_property_status={uint16_t(property),&b};
    g.node_light_event_(0,&p);
  }
}
void lc_workflow() {
  for(auto command : {Command::MODE,Command::ONOFF,Command::BRIGHTNESS}) {
    const unsigned cases=command==Command::MODE?3:1;
    for(unsigned m=0;m<cases;++m) {
      SteinelMesh g;setup(g,1);
      if(command==Command::BRIGHTNESS) {
        g.catalog_.nodes[0].nightmatiq=false;
        g.catalog_.nodes[0].product_id=0x1e79;
      }
      const unsigned v=command==Command::MODE?m:command==Command::ONOFF?1:40000;
      assert(g.queue_node_command(1,command,v));
      unsigned actions=0,reads=0,unacks=0;uint32_t first_action=0;
      for(unsigned tick=0;tick<150;++tick) {
        const auto before=g.mesh_tx_attempts_.load();g.dispatch_(test_now);
        if(g.mesh_tx_attempts_!=before) {
          if(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK){
            ++unacks;assert(g.node_states_[0].automatic==1 && g.node_states_[0].value_seen[2]==1000);
          }
          if(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET||captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK||
             captured.opcode==ESP_BLE_MESH_MODEL_OP_SCENE_RECALL||captured.opcode==ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK||
             captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET){++actions;if(!first_action)first_action=test_now;}
          if(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET||captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET)++reads;
          reply(g);
        }
        if(!g.node_queue_count_ && !g.node_completion_pending_ && g.access_operation_==SteinelMesh::AccessOperation::NONE &&
           reads && g.node_states_[0].value_seen[2]>1000)break;
        test_now+=200;
      }
      assert(actions>0 && first_action<2000 && reads>0 && unacks>0);
      assert(g.mesh_timeouts_==0 && g.node_states_[0].value_seen[2]>1000 &&
             g.node_states_[0].automatic==int(command==Command::MODE&&m==0));
      std::printf("LC missing SET response: command=%u value=%u actions=%u verified reads=%u\n",unsigned(command),v,actions,reads);
    }
  }
}
void polling_workflow() {
  for(unsigned count : {2U,16U})for(uint32_t latency : {100U,2900U}) {
    SteinelMesh g;setup(g,count,false);
    for(unsigned i=0;i<count;++i){auto &s=g.node_states_[i];s.value_seen[1]=s.value_seen[2]=0;s.poll_step=ON;physical_on[i]=1;}
    unsigned writes=0,reads=0;uint32_t seen[16]{},max_gap=0;unsigned n=0;
    for(unsigned i=0;i<count;++i)seen[i]=1000;
    while(test_now<601000) {
      assert(g.queue_node_command(1,Command::ONOFF,(n++&1)));
      const auto before=g.mesh_tx_attempts_.load();g.dispatch_(test_now);
      if(before==g.mesh_tx_attempts_){test_now+=20;continue;}
      const auto request=g.active_node_request_;
      if(request.kind==SET_ON)++writes;
      if(request.kind==ON)++reads;
      test_now+=latency;reply(g);
      // Acknowledged writes also refresh the confirmed output value.
      for(unsigned i=0;i<count;++i) {
        const auto confirmed=g.node_states_[i].value_seen[0];
        if(confirmed>seen[i]){max_gap=std::max(max_gap,confirmed-seen[i]);seen[i]=confirmed;}
        assert(test_now-seen[i]<g.node_states_[i].stale_after);
      }
      test_now+=200;
    }
    assert(writes>0 && reads>=count && max_gap<g.node_states_[0].stale_after);
    for(unsigned i=1;i<count;++i)assert(g.node_states_[i].on==1 && g.node_states_[i].value_seen[0]>1000);
    std::printf("Continuous writes: nodes=%u latency=%ums writes=%u reads=%u max read gap=%.2fs\n",count,latency,writes,reads,max_gap/1000.0);
  }
}
void diagnostic_workflow() {
  for(uint8_t kind : {uint8_t(ON),uint8_t(AUTO),uint8_t(SENSOR),uint8_t(COMPOSITION)}) {
    SteinelMesh g;setup(g,2);g.catalog_.nodes[1].selected=false;
    g.mesh_tx_accepted_=10;g.mesh_timeouts_=10;
    auto &s=g.node_states_[1];s.last_seen=0;s.dirty=false;s.supported=0;s.composition_checked=false;
    g.device_diagnostics_.start(test_now,1,1,g.catalog_.nodes[1].address,3);
    const uint8_t element=kind==AUTO?1:kind==SENSOR?2:0;
    assert(g.send_node_request_({1,element,kind,0,0,0,0,true}));
    auto p=captured;p.ctx.recv_op=g.node_expected_status_;
    if(kind==COMPOSITION) {
      uint8_t data[]={0x63,5,0xCE,0x1D,1,0,0,0,0,0,0,0,1,0,0,0x10};Buffer b{};b.data=data;b.len=sizeof(data);
      esp_ble_mesh_cfg_client_cb_param_t cb{};cb.params=&p;cb.status_cb.comp_data_status.composition_data=&b;
      assert(g.node_composition_event_(ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT,&cb));
    }else reply(g,false);
    assert(g.mesh_rx_messages_==1 && g.rssi_samples==1);
    assert(s.last_seen==0 && !s.dirty && s.supported==0 && !s.composition_checked && !g.catalog_.nodes[1].selected);
    g.advance_address_recovery_(60001);assert(g.rotations==0);
    std::printf("Read-only diagnostic: kind=%u authentic receive=%u recovery rotations=%u\n",kind,g.mesh_rx_messages_.load(),g.rotations);
  }
  SteinelMesh g;setup(g,2);g.catalog_.nodes[1].selected=false;
  g.device_diagnostics_.start(test_now,1,1,g.catalog_.nodes[1].address,3);
  assert(g.send_node_request_({1,0,ON,0,0,0,0,true}));
  auto p=captured;p.ctx.recv_op=g.node_expected_status_;p.ctx.addr+=1;
  esp_ble_mesh_generic_client_cb_param_t cb{};cb.params=&p;cb.status_cb.onoff_status.present_onoff=1;
  g.node_generic_event_(0,&cb);assert(g.mesh_rx_messages_==0 && !g.node_completion_pending_);
  p.ctx.addr-=1;p.ctx.recv_op=0;g.node_generic_event_(0,&cb);assert(g.mesh_rx_messages_==0);
  p.ctx.recv_op=g.node_expected_status_;cb.status_cb.onoff_status.present_onoff=2;
  g.node_generic_event_(0,&cb);assert(g.mesh_rx_messages_==0 && g.rssi_samples==0);
  SteinelMesh timeout;setup(timeout,2);timeout.catalog_.nodes[1].selected=false;
  timeout.device_diagnostics_.start(test_now,1,1,timeout.catalog_.nodes[1].address,3);
  assert(timeout.send_node_request_({1,0,ON,0,0,0,0,true}));
  test_now=timeout.access_deadline_;timeout.expire_access_operation_(test_now);
  assert(timeout.mesh_rx_messages_==0 && timeout.mesh_timeouts_==0 && timeout.node_completion_pending_);
}
void sensor_correlation_workflow() {
  uint8_t firmware[]={9,0x0E,0,'1','.','2','.','3'};
  uint8_t lux[]={0xC4,0x09,0xC4,0x09,0};
  auto receive=[&](SteinelMesh &g,uint8_t *data,size_t length) {
    auto p=captured;p.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS;
    Buffer b{};b.data=data;b.len=length;
    esp_ble_mesh_sensor_client_cb_param_t cb{};cb.params=&p;cb.status_cb.sensor_status.marshalled_sensor_data=&b;
    g.node_sensor_event_(0,&cb);
  };
  for(unsigned scenario=0;scenario<3;++scenario) {
    SteinelMesh g;setup(g,1);
    const uint16_t requested=scenario==1?0x000E:0x004E;
    assert(g.send_node_request_({0,2,SENSOR_PROPERTY,0,0,requested}));
    receive(g,scenario==0?firmware:scenario==1?lux:nullptr,
            scenario==0?sizeof(firmware):scenario==1?sizeof(lux):0);
    assert(!g.node_completion_pending_ && g.access_operation_==SteinelMesh::AccessOperation::NODE);
    if(scenario==0)assert(g.node_states_[0].lux==protocol::UNKNOWN_24 &&
                          std::strcmp(g.node_states_[0].firmware,"1.2.3")==0);
    if(scenario==1)assert(g.node_states_[0].lux==2500 && !g.node_states_[0].firmware[0]);
    receive(g,scenario==1?firmware:lux,scenario==1?sizeof(firmware):sizeof(lux));
    assert(g.node_completion_pending_ && g.node_completion_success_ &&
           g.access_operation_==SteinelMesh::AccessOperation::NONE);
  }
  SteinelMesh unsupported;setup(unsupported,1);
  assert(unsupported.send_node_request_({0,2,SENSOR_PROPERTY,0,0,0x004E}));
  uint8_t missing[]={0xFF,0x4E,0};
  receive(unsupported,missing,sizeof(missing));
  assert(unsupported.node_completion_pending_ && unsupported.node_completion_success_ &&
         unsupported.node_states_[0].lux==protocol::UNKNOWN_24);
  SteinelMesh mixed;setup(mixed,1);
  assert(mixed.send_node_request_({0,2,SENSOR_PROPERTY,0,0,0x004E}));
  uint8_t records[sizeof(firmware)+sizeof(lux)]{};
  std::memcpy(records,firmware,sizeof(firmware));std::memcpy(records+sizeof(firmware),lux,sizeof(lux));
  receive(mixed,records,sizeof(records));
  assert(mixed.node_completion_pending_ && mixed.node_completion_success_ && mixed.node_states_[0].lux==2500);
  SteinelMesh malformed;setup(malformed,1);
  assert(malformed.send_node_request_({0,2,SENSOR_PROPERTY,0,0,0x004E}));
  uint8_t truncated[sizeof(lux)+1]{};std::memcpy(truncated,lux,sizeof(lux));truncated[sizeof(lux)]=1;
  receive(malformed,truncated,sizeof(truncated));
  assert(malformed.node_completion_pending_ && !malformed.node_completion_success_ &&
         malformed.node_states_[0].lux==protocol::UNKNOWN_24);
  SteinelMesh all;setup(all,1);assert(all.send_node_request_({0,2,SENSOR,0,0,0}));
  receive(all,nullptr,0);assert(all.node_completion_pending_ && all.node_completion_success_);
  SteinelMesh dropped;setup(dropped,1);
  assert(dropped.send_node_request_({0,2,SENSOR_PROPERTY,0,0,0x004E}));
  receive(dropped,firmware,sizeof(firmware));assert(!dropped.node_completion_pending_);
  test_now=dropped.access_deadline_;dropped.expire_access_operation_(test_now);
  assert(dropped.node_completion_pending_ && !dropped.node_completion_success_ &&
         dropped.access_operation_==SteinelMesh::AccessOperation::NONE);
  SteinelMesh diagnostic;setup(diagnostic,2);diagnostic.catalog_.nodes[1].selected=false;
  auto &state=diagnostic.node_states_[1];state.last_seen=0;state.dirty=false;state.supported=0;
  diagnostic.mesh_tx_accepted_=10;diagnostic.mesh_timeouts_=10;
  diagnostic.device_diagnostics_.start(test_now,1,1,diagnostic.catalog_.nodes[1].address,3);
  assert(diagnostic.send_node_request_({1,2,SENSOR_PROPERTY,0,0,0x004E,0,true}));
  auto p=captured;p.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS;
  Buffer b{};b.data=firmware;b.len=sizeof(firmware);
  esp_ble_mesh_sensor_client_cb_param_t cb{};cb.params=&p;cb.status_cb.sensor_status.marshalled_sensor_data=&b;
  ++p.ctx.addr;diagnostic.node_sensor_event_(0,&cb);
  --p.ctx.addr;p.opcode^=0x8000;diagnostic.node_sensor_event_(0,&cb);
  assert(diagnostic.mesh_rx_messages_==0 && !diagnostic.node_completion_pending_);
  receive(diagnostic,firmware,sizeof(firmware));
  assert(!diagnostic.node_completion_pending_ && diagnostic.mesh_rx_messages_==1 && diagnostic.rssi_samples==1);
  diagnostic.advance_address_recovery_(60001);assert(diagnostic.rotations==0);
  receive(diagnostic,lux,sizeof(lux));
  assert(diagnostic.node_completion_pending_ && diagnostic.node_completion_success_ &&
         diagnostic.mesh_rx_messages_==2 && diagnostic.rssi_samples==2);
  assert(state.last_seen==0 && !state.dirty && state.supported==0 &&
         !state.firmware[0] && state.lux==protocol::UNKNOWN_24);
  std::puts("Sensor correlation: unrelated/empty replies wait, matching/unsupported properties complete, malformed data fails, diagnostic proof preserved");
}
void transaction_workflow() {
  for(const auto command : {Command::ONOFF,Command::BRIGHTNESS,Command::AUTO,Command::THRESHOLD,Command::RUN_TIME}) {
    SteinelMesh acknowledged;setup(acknowledged,1);
    acknowledged.catalog_.nodes[0].nightmatiq=false;
    acknowledged.catalog_.nodes[0].product_id=0x1E79;
    const uint32_t requested=command==Command::BRIGHTNESS?32768:
        command==Command::THRESHOLD?500:command==Command::RUN_TIME?120000:1;
    const uint8_t field=uint8_t(command);
    auto &state=acknowledged.node_states_[0];
    assert(acknowledged.queue_node_command(1,command,requested));
    bool received=false;
    for(unsigned tick=0;tick<40&&!received;++tick) {
      const auto before=acknowledged.mesh_tx_attempts_.load();
      acknowledged.dispatch_(test_now);
      if(acknowledged.mesh_tx_attempts_!=before) {
        const auto request=acknowledged.active_node_request_;
        const bool final_write=is_write(request.kind)&&
            !((request.kind==SET_ON||request.kind==SET_LEVEL)&&request.step==0);
        reply(acknowledged,false);
        if(final_write) {
          assert(acknowledged.node_completion_success_&&state.value_seen[field]==test_now);
          const uint32_t reply_at=test_now;
          test_now+=7;acknowledged.dispatch_(test_now);
          assert(state.controls.after[field]==reply_at);
          assert(state.controls.observe(field,requested,state.value_seen[field],test_now));
          assert(!state.controls.has(field));received=true;
        }
      }
      test_now+=200;
    }
    assert(received);
  }
  for(uint8_t kind : {uint8_t(ON),uint8_t(AUTO)}) {
    SteinelMesh verification;setup(verification,1);
    auto &state=verification.node_states_[0];
    state.verify_fields=1U<<(kind-ON);state.verify_next=kind-ON;
    verification.dispatch_(test_now);
    assert(verification.active_node_request_.kind==kind&&verification.active_node_request_.verification);
    const unsigned limit=kind==ON?OUTPUT_VERIFY_ATTEMPTS:3;
    for(unsigned attempt=0;attempt<limit;++attempt) {
      test_now=verification.access_deadline_;verification.expire_access_operation_(test_now);
      verification.dispatch_(test_now);
      assert(verification.node_states_[0].on==0 && verification.node_states_[0].automatic==1);
      assert(verification.node_queue_count_==0 && state.verify_attempts[kind-ON]==attempt+1);
      if(attempt+1==limit){assert(state.verify_fields==0);break;}
      assert(state.verify_fields==(1U<<(kind-ON)));
      test_now+=200;verification.dispatch_(test_now);
      assert(verification.active_node_request_.kind==kind && verification.active_node_request_.verification);
    }
  }
  // A lost final acknowledgement does not prove that the write was lost.
  SteinelMesh missed;setup(missed,1);
  assert(missed.send_node_request_({0,0,SET_ON,1,77,1,1}));
  test_now=missed.access_deadline_;missed.expire_access_operation_(test_now);
  missed.dispatch_(test_now);
  assert(missed.node_states_[0].force_poll && missed.node_states_[0].poll_step==0);
  SteinelMesh g;setup(g,1);assert(g.queue_node_command(1,Command::ONOFF,1));
  sdk_result=-7;g.dispatch_(test_now);const auto tid_sent=g.active_node_request_.tid;
  assert(g.mesh_tx_errors_==1 && g.node_completion_pending_ && !g.node_completion_success_ && g.node_states_[0].on==0);
  sdk_result=0;
  test_now+=200;g.dispatch_(test_now);
  test_now+=200;g.dispatch_(test_now);assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK);
  assert(g.active_node_request_.attempts==1 && g.active_node_request_.tid==tid_sent && g.node_states_[0].failures==1);
  reply(g);test_now+=200;g.dispatch_(test_now);test_now+=200;g.dispatch_(test_now);
  // A fair read may run between preparation and the output action.
  for(unsigned i=0;captured.opcode!=ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET&&i<20;++i) {
    reply(g);test_now+=200;g.dispatch_(test_now);test_now+=200;g.dispatch_(test_now);
  }
  assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET && tid==tid_sent && value==1);
  auto p=captured;p.ctx.recv_op=g.node_expected_status_;esp_ble_mesh_generic_client_cb_param_t cb{};cb.params=&p;
  cb.status_cb.onoff_status.present_onoff=0;g.node_generic_event_(0,&cb);
  assert(g.node_completion_pending_&&!g.node_completion_success_ && g.node_states_[0].on==0);
  const auto sent=g.mesh_tx_attempts_.load();
  for(unsigned i=0;i<20&&g.mesh_tx_attempts_==sent;++i){test_now+=200;g.dispatch_(test_now);}
  assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET && tid==tid_sent && g.active_node_request_.attempts==1);
  reply(g);test_now+=200;g.dispatch_(test_now);
  assert(g.node_states_[0].on==1 && g.node_states_[0].force_poll && g.node_states_[0].failures==0);

  // A newer queued intent supersedes an earlier preparation, without reusing its TID.
  SteinelMesh newer;setup(newer,1);assert(newer.queue_node_command(1,Command::ONOFF,1));
  newer.dispatch_(test_now);const uint8_t old_tid=newer.active_node_request_.tid;reply(newer);
  assert(newer.queue_node_command(1,Command::ONOFF,0));
  bool output=false;
  for(unsigned i=0;i<30&&!output;++i) {
    const auto before=newer.mesh_tx_attempts_.load();test_now+=200;newer.dispatch_(test_now);
    if(newer.mesh_tx_attempts_!=before){
      if(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET){assert(value==0 && tid!=old_tid);output=true;}
      reply(newer);
    }
  }
  assert(output);
  // SDK acceptance of an unacknowledged command is not a confirmed device value.
  SteinelMesh lost;setup(lost,1);assert(lost.queue_node_command(1,Command::ONOFF,1));
  bool read_back=false;
  for(unsigned i=0;i<100&&!read_back;++i) {
    const auto before=lost.mesh_tx_attempts_.load();lost.dispatch_(test_now);
    if(lost.mesh_tx_attempts_!=before && captured.opcode!=ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK){
      reply(lost);read_back=captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET;
    }
    test_now+=200;
  }
  assert(read_back && lost.node_states_[0].automatic==1);
  // The reserved slot admits a retry even when other devices fill the queue.
  SteinelMesh full;setup(full,16,false);assert(full.queue_node_command(1,Command::ONOFF,1));
  full.dispatch_(test_now);const auto retry_tid=full.active_node_request_.tid;
  for(unsigned i=1;i<16;++i)assert(full.queue_node_command(1+8*i,Command::ONOFF,1));
  assert(full.node_queue_count_==15 && !full.queue_node_command(1,Command::ONOFF,0));
  test_now=full.access_deadline_;full.expire_access_operation_(test_now);full.dispatch_(test_now);
  assert(full.node_queue_count_==16 && full.node_queue_[0].attempts==1);
  test_now+=200;full.dispatch_(test_now);
  assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET && full.active_node_request_.node==0 && tid==retry_tid);
  test_now=full.access_deadline_;full.expire_access_operation_(test_now);full.dispatch_(test_now);
  assert(full.node_queue_count_==15 && full.mesh_timeouts_==2);
  for(unsigned i=0;i<full.node_queue_count_;++i)assert(full.node_queue_[i].node!=0);
  std::puts("Transactions: send rejection, bounded retry/TID, newer intent and lost unacknowledged packet passed");
}
void mode_sequence_workflow() {
  SteinelMesh rejected;setup(rejected,1);
  assert(rejected.queue_node_command(1,Command::MODE,2));
  rejected.node_states_[0].composition_matches=false;
  rejected.dispatch_(test_now);
  assert(!rejected.node_states_[0].mode.writing && rejected.node_states_[0].mode.verifying);
  assert(rejected.node_states_[0].on==0 && rejected.node_states_[0].automatic==1);
  for(unsigned mode=0;mode<3;++mode) {
    SteinelMesh g;setup(g,1);assert(g.queue_node_command(1,Command::MODE,mode));
    unsigned packets=0;uint8_t transaction=0;uint32_t first=0,last=0;
    bool applied_action=false;
    for(unsigned tick=0;tick<25 && packets<4;++tick) {
      const auto before=g.mesh_tx_attempts_.load();g.dispatch_(test_now);
      if(g.mesh_tx_attempts_!=before) {
        assert(g.active_node_request_.kind==SET_MODE);
        assert(g.active_node_request_.step==packets);
        if(!packets){first=test_now;transaction=g.active_node_request_.tid;}
        assert(g.active_node_request_.tid==transaction);
        if(packets<2) {
          assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK);
          physical_auto[0]=value;
          if(!value)physical_on[0]=0;
        } else if(mode==0) {
          assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK);
          if(!applied_action)physical_on[0]=1;
          applied_action=true;
        } else {
          assert(captured.opcode==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK);
          if(!applied_action)physical_on[0]=value;
          applied_action=true;
        }
        assert(g.node_states_[0].on==0 && g.node_states_[0].automatic==1);
        assert(g.node_states_[0].mode.writing);
        assert(g.node_completion_pending_ && g.node_completion_success_);
        last=test_now;++packets;
      }
      test_now+=200;
    }
    assert(packets==4 && last-first<2000 && physical_auto[0]==(mode==0));
    if(mode!=0)assert(physical_on[0]==(mode==1));
    g.dispatch_(test_now);
    assert(g.node_states_[0].force_poll);
    assert(!g.node_states_[0].mode.writing && g.node_states_[0].mode.verifying);
    assert(g.node_states_[0].controls.has(5));
    assert(!(g.node_states_[0].controls.writing&32));
    test_now+=200;g.dispatch_(test_now);
    assert(g.active_node_request_.kind==ON && g.active_node_request_.verification);
    reply(g);
    auto &state=g.node_states_[0];
    state.mode.observe(test_now,state.on,state.automatic,state.value_seen[0],state.value_seen[2],state.stale_after);
    assert(state.mode.verifying && state.controls.has(5));
    g.dispatch_(test_now);test_now+=200;g.dispatch_(test_now);
    assert(g.active_node_request_.kind==AUTO && g.active_node_request_.verification);
    reply(g);
    state.mode.observe(test_now,state.on,state.automatic,state.value_seen[0],state.value_seen[2],state.stale_after);
    assert(state.mode.value==int(mode) && !state.mode.verifying);
    assert(state.controls.observe(5,state.mode.value,state.mode.confirmed_at,test_now));
    assert(state.verify_fields==0);
  }
  std::puts("Mode sequence: four unacknowledged stages keep one TID, complete without replies and require readback");
}
void output_confirmation_workflow() {
  for(bool night : {false,true}) for(bool dropped : {false,true}) {
    SteinelMesh g;setup(g,1);g.catalog_.nodes[0].nightmatiq=night;
    if(!night)g.catalog_.nodes[0].product_id=0x1e79;
    assert(g.queue_node_command(1,night?Command::MODE:Command::ONOFF,1));
    unsigned attempts=0;bool confirmed=false;
    const uint32_t started=test_now;
    for(unsigned tick=0;tick<200&&!confirmed;++tick) {
      const auto before=g.mesh_tx_attempts_.load();g.dispatch_(test_now);
      if(g.mesh_tx_attempts_!=before) {
        const auto r=g.active_node_request_;
        if(r.verification) {
          assert(r.kind==ON); // LC timeouts cannot delay the first physical read or its retries.
          ++attempts;
          if(dropped&&attempts<OUTPUT_VERIFY_ATTEMPTS) {
            test_now=g.access_deadline_;g.expire_access_operation_(test_now);
          } else {
            // A valid but early reply still reports the old output.
            physical_on[0]=!dropped&&attempts<3?0:1;
            reply(g,false);confirmed=g.node_states_[0].on==1;
          }
          if(!confirmed)assert(g.node_states_[0].on==0);
        } else if(!night&&r.kind==SET_ON&&r.step==1) {
          physical_on[0]=1;
          auto p=captured;p.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS;
          esp_ble_mesh_generic_client_cb_param_t cb{};cb.params=&p;
          cb.status_cb.onoff_status.op_en=true;
          cb.status_cb.onoff_status.present_onoff=0;cb.status_cb.onoff_status.target_onoff=1;
          assert(g.node_generic_event_(0,&cb));
        } else reply(g,false);
      }
      test_now+=200;
    }
    assert(confirmed&&attempts==(dropped?OUTPUT_VERIFY_ATTEMPTS:3));
    assert(test_now-started<20000); // No 30-second normal-poll fallback after four lost reads.
    std::printf("Physical output verification: night=%u dropped=%u attempts=%u confirmed=%.2fs\n",
                night,dropped,attempts,(test_now-started)/1000.0);
  }
  SteinelMesh automatic;setup(automatic,1);
  automatic.node_states_[0].automatic=physical_auto[0]=0;
  assert(automatic.queue_node_command(1,Command::MODE,0));
  unsigned attempts=0;const uint32_t started=test_now;
  for(unsigned tick=0;tick<200&&automatic.node_states_[0].on!=1;++tick) {
    const auto before=automatic.mesh_tx_attempts_.load();automatic.dispatch_(test_now);
    if(automatic.mesh_tx_attempts_!=before) {
      const auto r=automatic.active_node_request_;
      if(r.verification) {
        assert(r.kind==ON);++attempts;
        if(attempts<OUTPUT_VERIFY_ATTEMPTS) {
          test_now=automatic.access_deadline_;automatic.expire_access_operation_(test_now);
          assert(automatic.node_states_[0].on==0);
        } else reply(automatic,false);
      } else reply(automatic,false);
    }
    test_now+=200;
  }
  assert(attempts==OUTPUT_VERIFY_ATTEMPTS&&automatic.node_states_[0].on==1&&test_now-started<20000);
  std::puts("Return to Auto: lost output replies retry immediately without inventing the relay state");
}
int main(int argc,char **argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);assert(argc==2);
  assert(POLL_INTERVAL==30000&&STALE_INTERVAL==180000&&IDENTITY_INTERVAL==21600000&&POLL_STEPS>0);
  assert(THRESHOLD_PROPERTY==0x2B&&RUN_PROPERTY==0x3C);
  const std::string scenario=argv[1];
  if(scenario=="lc"){lc_workflow();mode_sequence_workflow();output_confirmation_workflow();}else if(scenario=="polling")polling_workflow();
  else if(scenario=="diagnostics")diagnostic_workflow();
  else if(scenario=="sensor")sensor_correlation_workflow();
  else if(scenario=="transactions")transaction_workflow();else return 2;
}
'''


if __name__ == "__main__":
    scenarios = sys.argv[1:] or ["lc", "polling", "diagnostics", "sensor", "transactions"]
    with tempfile.TemporaryDirectory(prefix="steinel-workflows-") as directory:
        binary = str(Path(directory) / "workflows")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=address,undefined", "-I", str(DIRECTORY),
                        "-x", "c++", "-", "-o", binary],
                       input=sdk+definitions+sdk_set+production+MAIN,text=True,check=True)
        failed = False
        for scenario in scenarios:
            result = subprocess.run([binary,scenario],check=False)
            print(f"Workflow {scenario}: {'PASS' if result.returncode == 0 else 'FAIL'}",flush=True)
            failed |= result.returncode != 0
        sys.exit(int(failed))
