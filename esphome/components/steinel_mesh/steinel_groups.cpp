#include "steinel_mesh.h"

#include <algorithm>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::steinel_mesh {
namespace {
// Request kinds shared with the node scheduler and node_model_.
constexpr uint8_t COMPOSITION = 0, SET_ON = 7, SET_LEVEL = 8, SET_AUTO = 9, SET_THRESHOLD = 10;
}

bool SteinelMesh::queue_group_command(uint16_t address, NodeCommand command, uint32_t value) {
  if (!protocol::group(address) || !this->catalog_valid_ || !this->mesh_ready_.load() ||
      this->reboot_pending_.load()) return false;
  switch (command) {
    case NodeCommand::ONOFF:
    case NodeCommand::AUTO:
      if (value > 1) return false;
      break;
    case NodeCommand::BRIGHTNESS:
      if (value > 65535) return false;
      break;
    case NodeCommand::THRESHOLD:
    case NodeCommand::RUN_TIME:
      if (value >= protocol::UNKNOWN_24) return false;
      break;
    default:
      return false;
  }
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  if (this->group_queue_count_ == this->group_queue_.size()) return false;
  this->group_queue_[this->group_queue_count_++] = {address, command, value};
  return true;
}

void SteinelMesh::advance_group_commands_(uint32_t now) {
  const auto operation = this->access_operation_.load();
  if ((operation != AccessOperation::NONE &&
       (operation != AccessOperation::NODE || this->active_node_request_.kind == COMPOSITION)) ||
      this->node_write_pending_ ||
      this->node_completion_pending_.load() || this->composition_query_in_flight_.load() ||
      this->control_kind_ != ControlKind::NONE || this->control_request_pending_() ||
      this->poll_stage_ != 0 || protocol::deadline_pending(now, this->node_send_at_) ||
      (operation == AccessOperation::NONE && protocol::deadline_pending(now, this->node_next_request_at_))) return;

  GroupRequest request{};
  uint8_t tid = 0;
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    if (this->group_queue_count_ == 0) return;
    request = this->group_queue_[0];
    std::move(this->group_queue_.begin() + 1, this->group_queue_.begin() + this->group_queue_count_,
              this->group_queue_.begin());
    --this->group_queue_count_;
    if (request.command == NodeCommand::ONOFF || request.command == NodeCommand::BRIGHTNESS)
      tid = this->next_tid_();
    if (operation == AccessOperation::NODE) {
      const uint32_t opcode = this->access_opcode_.load();
      if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET ||
          opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET ||
          opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET ||
          opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET)
        this->node_read_obsolete_.store(true);
    }
  }
  esp_ble_mesh_client_common_param_t params{};
  params.ctx.net_idx = this->config_.net_key_index;
  params.ctx.app_idx = this->config_.app_key_index;
  params.ctx.addr = request.address;
  params.ctx.send_ttl = 7;
  params.msg_timeout = 3000;
  esp_ble_mesh_generic_client_set_state_t generic{};
  esp_ble_mesh_light_client_set_state_t light{};
  switch (request.command) {
    case NodeCommand::ONOFF:
      params.model = this->node_model_(SET_ON);
      params.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK;
      generic.onoff_set.onoff = request.value;
      generic.onoff_set.tid = tid;
      break;
    case NodeCommand::BRIGHTNESS:
      params.model = this->node_model_(SET_LEVEL);
      params.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK;
      light.lightness_set.lightness = request.value;
      light.lightness_set.tid = tid;
      break;
    case NodeCommand::AUTO:
      params.model = this->node_model_(SET_AUTO);
      params.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK;
      light.lc_mode_set.mode = request.value;
      break;
    case NodeCommand::THRESHOLD:
    case NodeCommand::RUN_TIME:
      params.model = this->node_model_(SET_THRESHOLD);
      params.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET_UNACK;
      light.lc_property_set.property_id = request.command == NodeCommand::THRESHOLD ? 0x002B : 0x003C;
      this->group_property_storage_ = {uint8_t(request.value), uint8_t(request.value >> 8), uint8_t(request.value >> 16)};
      this->group_property_buffer_.data = this->group_property_storage_.data();
      this->group_property_buffer_.__buf = this->group_property_storage_.data();
      this->group_property_buffer_.size = 3;
      this->group_property_buffer_.len = 3;
      light.lc_property_set.property_value = &this->group_property_buffer_;
      break;
    default:
      ESP_LOGE("steinel_groups", "Invalid queued group command");
      return;
  }
  const esp_err_t result = request.command == NodeCommand::ONOFF ?
      esp_ble_mesh_generic_client_set_state(&params, &generic) :
      esp_ble_mesh_light_client_set_state(&params, &light);
  this->node_send_at_ = millis() + 200;
  this->node_next_request_at_ = this->node_send_at_;
  if (!this->record_send_result_(result))
    ESP_LOGW("steinel_groups", "Group command to 0x%04x failed: %d", request.address, int(result));
  return;
}

}  // namespace esphome::steinel_mesh
