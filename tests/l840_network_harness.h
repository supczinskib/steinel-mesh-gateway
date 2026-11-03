// Host SDK and native-entity boundaries for the closed-loop L 840 network test.
#include "mesh_protocol.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

namespace esphome::steinel_mesh {
uint32_t test_now = 200000;
uint32_t millis() { return test_now; }
constexpr uint32_t MESSAGE_TIMEOUT_MS = 3000;

// SDK_CONSTANTS

struct Model { uint16_t keys[1]{}; };
struct Buffer { uint8_t *data{}, *__buf{}; size_t size{}, len{}; };
struct esp_ble_mesh_client_common_param_t {
  struct { uint16_t net_idx{}, app_idx{}, addr{}; uint8_t send_ttl{}; uint32_t recv_op{}; } ctx;
  uint32_t opcode{}, msg_timeout{}; Model *model{};
};
using esp_err_t = int;
using esp_ble_mesh_generic_client_cb_event_t = int;
using esp_ble_mesh_light_client_cb_event_t = int;
using esp_ble_mesh_sensor_client_cb_event_t = int;
struct esp_ble_mesh_generic_client_get_state_t {};
struct esp_ble_mesh_generic_client_set_state_t {
  struct { uint8_t onoff{}, tid{}; bool op_en{}; uint8_t trans_time{}, delay{}; } onoff_set;
};
struct esp_ble_mesh_light_client_get_state_t { struct { uint16_t property_id{}; } lc_property_get; };
struct esp_ble_mesh_light_client_set_state_t {
  struct { uint16_t lightness{}; uint8_t tid{}; bool op_en{}; uint8_t trans_time{}, delay{}; } lightness_set;
  struct { uint8_t mode{}; } lc_mode_set;
  struct { uint16_t property_id{}; Buffer *property_value{}; } lc_property_set;
};
struct esp_ble_mesh_sensor_client_get_state_t {
  struct { bool op_en{}; uint16_t property_id{}; } sensor_get;
  struct { bool op_en{}; } descriptor_get;
};
struct esp_ble_mesh_cfg_client_get_state_t { struct { uint8_t page{}; } comp_data_get; };
struct esp_ble_mesh_time_scene_client_set_state_t {
  struct { uint16_t scene_number{}; uint8_t tid{}; } scene_recall;
};
struct esp_ble_mesh_generic_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params{}; int error_code{};
  struct { struct { bool op_en{}; uint8_t present_onoff{}, target_onoff{}, remain_time{}; } onoff_status; } status_cb;
};
struct esp_ble_mesh_light_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params{}; int error_code{};
  struct {
    struct { uint16_t property_id{}; Buffer *property_value{}; } lc_property_status;
    struct { uint8_t mode{}; } lc_mode_status;
    struct { bool op_en{}; uint8_t present_light_onoff{}, target_light_onoff{}, remain_time{}; } lc_light_onoff_status;
    struct { bool op_en{}; uint16_t present_lightness{}, target_lightness{}; } lightness_status;
  } status_cb;
};
struct esp_ble_mesh_sensor_client_cb_param_t {
  esp_ble_mesh_client_common_param_t *params{}; int error_code{};
  struct {
    struct { Buffer *descriptor{}; } descriptor_status;
    struct { Buffer *marshalled_sensor_data{}; } sensor_status;
  } status_cb;
};

esp_ble_mesh_client_common_param_t wire_request;
std::vector<uint8_t> wire_payload;
unsigned wire_sends = 0;
int capture(esp_ble_mesh_client_common_param_t *params) {
  wire_request = *params; wire_payload.clear(); ++wire_sends; return 0;
}
int esp_ble_mesh_config_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_cfg_client_get_state_t *) { return capture(p); }
int esp_ble_mesh_generic_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_generic_client_get_state_t *) { return capture(p); }
int esp_ble_mesh_generic_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_generic_client_set_state_t *s) {
  capture(p); const auto &v=s->onoff_set; wire_payload={v.onoff,v.tid};
  if(v.op_en) { wire_payload.push_back(v.trans_time); wire_payload.push_back(v.delay); } return 0;
}
int esp_ble_mesh_light_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_light_client_get_state_t *s) {
  capture(p); if(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET)
    wire_payload={uint8_t(s->lc_property_get.property_id),uint8_t(s->lc_property_get.property_id>>8)};
  return 0;
}
int esp_ble_mesh_light_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_light_client_set_state_t *s) {
  capture(p);
  if(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET||p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK)
    wire_payload={s->lc_mode_set.mode};
  else if(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET || p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK) {
    const auto &v=s->lightness_set;wire_payload={uint8_t(v.lightness),uint8_t(v.lightness>>8),v.tid};
    if(v.op_en) { wire_payload.push_back(v.trans_time);wire_payload.push_back(v.delay); }
  } else if(p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET || p->opcode==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET_UNACK) {
    const auto &v=s->lc_property_set;wire_payload={uint8_t(v.property_id),uint8_t(v.property_id>>8)};
    wire_payload.insert(wire_payload.end(),v.property_value->data,v.property_value->data+v.property_value->len);
  }
  return 0;
}
int esp_ble_mesh_sensor_client_get_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_sensor_client_get_state_t *s) {
  capture(p);if(p->opcode==ESP_BLE_MESH_MODEL_OP_SENSOR_GET&&s->sensor_get.op_en)
    wire_payload={uint8_t(s->sensor_get.property_id),uint8_t(s->sensor_get.property_id>>8)};
  return 0;
}
int esp_ble_mesh_time_scene_client_set_state(esp_ble_mesh_client_common_param_t *p, esp_ble_mesh_time_scene_client_set_state_t *) { return capture(p); }

class SteinelMesh {
 public:
  struct TestEntity {
    bool known{}, boolean{}; float number{}; std::string option; unsigned updates{};
    void publish_state(bool value) { known=true; boolean=value; ++updates; }
    void publish_state(float value) { known=true; number=value; ++updates; }
    void publish_state(const char *text) { known=true; option=text; ++updates; }
    void invalidate_state() { known=false; }
  };
  struct NodeEntities {
    TestEntity *available{}, *actual_output{}, *mode{}, *output{}, *automatic{};
    TestEntity *firmware{}, *hardware{}, *product{}, *model{}, *lux{}, *threshold{}, *run{}, *level{}, *motion{}, *diagnostics{};
  };
  // NODE_STATE_AND_REQUEST
  enum class NodeCommand : uint8_t { ONOFF, BRIGHTNESS, AUTO, THRESHOLD, RUN_TIME, MODE };
  enum class AccessOperation { NONE, NODE };
  enum class ControlKind { NONE };
  struct Config { uint16_t net_key_index=0, app_key_index=0, onoff_address=0; } config_;
  static constexpr uint8_t NODE_QUEUE_SIZE = 16;
  protocol::Catalog catalog_{};
  std::array<NodeState, protocol::MAX_NODES> node_states_{};
  std::array<NodeRequest, NODE_QUEUE_SIZE> node_queue_{};
  size_t node_queue_count_{};
  std::mutex node_mutex_;
  protocol::DeviceDiagnostics device_diagnostics_{};
  protocol::StateTrace state_trace_{};
  bool catalog_valid_=true,legacy_profile_=false;
  uint8_t tid_{};
  uint8_t next_tid_();
  std::atomic<bool> mesh_ready_{true}, reboot_pending_{false}, composition_query_in_flight_{false};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE}, access_last_completed_{AccessOperation::NONE};
  std::atomic<bool> access_last_success_{false};
  std::atomic<uint32_t> access_opcode_{0}, access_deadline_{0}, node_expected_status_{0};
  std::atomic<uint16_t> node_destination_{0}, node_expected_property_{0};
  std::atomic<uint32_t> mesh_rx_messages_{0}, mesh_generic_rx_{0}, mesh_sensor_rx_{0}, mesh_timeouts_{0};
  std::atomic<bool> node_completion_pending_{false}, node_completion_success_{false};
  NodeRequest active_node_request_{};
  std::array<uint8_t, 3> node_property_storage_{};
  Buffer node_property_buffer_{};
  uint32_t node_poll_at_{}, node_next_request_at_{};
  uint8_t node_poll_index_{}, node_priority_burst_{}, node_write_burst_{}, poll_stage_{};
  ControlKind control_kind_=ControlKind::NONE;
  Model model_{};
  Model *node_model_(uint8_t) { return &model_; }
  bool control_request_pending_() const { return false; }
  bool record_send_result_(int result) { return result==0; }
  bool sync_node_functions_() { return false; }
  template<class T> void record_mesh_rssi_(const T &) {}
  bool begin_access_operation_(AccessOperation,uint32_t);
  bool record_access_send_result_(AccessOperation,uint32_t,int);
  bool complete_access_operation_(uint32_t,bool);
  void expire_access_operation_(uint32_t);
  bool diagnostic_request_allowed_(const NodeRequest &);
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0);
  bool diagnostic_reply_(const esp_ble_mesh_client_common_param_t *,uint32_t,bool,uint16_t=0);
  bool next_diagnostic_request_(uint32_t,NodeRequest &);
  bool queue_node_command(uint16_t,NodeCommand,uint32_t);
  bool send_node_request_(const NodeRequest &);
  void finish_node_request_(bool);
  bool node_event_(const esp_ble_mesh_client_common_param_t *,uint32_t,bool,uint16_t=0);
  bool node_response_value_matches_(const esp_ble_mesh_client_common_param_t *,uint32_t,uint32_t) const;
  bool node_generic_event_(int,esp_ble_mesh_generic_client_cb_param_t *);
  bool node_light_event_(int,esp_ble_mesh_light_client_cb_param_t *);
  bool node_sensor_event_(int,esp_ble_mesh_sensor_client_cb_param_t *);
  void advance_nodes_(uint32_t);
  bool poll_priority_(uint32_t);
  void dispatch_node_request_(const NodeRequest &,uint32_t);
  void poll_nodes_(uint32_t);
};
