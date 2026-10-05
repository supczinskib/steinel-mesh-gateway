#include "steinel_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "esp_ble_mesh_networking_api.h"
#include "esphome/core/application.h"
#include "esphome/core/controller_registry.h"
#include "esphome/core/device.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::steinel_mesh {
namespace {
enum Request : uint8_t { COMPOSITION, ON, LEVEL, AUTO, THRESHOLD, RUN, SENSOR,
                         SET_ON, SET_LEVEL, SET_AUTO, SET_THRESHOLD, SET_RUN,
                         DESCRIPTORS, SENSOR_PROPERTY, SET_MODE };
constexpr uint32_t POLL_INTERVAL = 30000, STALE_INTERVAL = 180000;
constexpr uint32_t IDENTITY_INTERVAL = 6UL * 60UL * 60UL * 1000UL;
constexpr uint16_t THRESHOLD_PROPERTY = 0x002B, RUN_PROPERTY = 0x003C;
constexpr uint8_t POLL_STEPS = 6 + protocol::MAX_ELEMENTS * 5;
inline constexpr uint8_t OUTPUT_VERIFY_ATTEMPTS = 5;
inline constexpr uint32_t NIGHTMATIQ_STATE_TIMEOUT = 1200;
constexpr bool is_write(uint8_t kind) {
  return (kind >= SET_ON && kind <= SET_RUN) || kind == SET_MODE;
}
constexpr size_t command_field(uint8_t kind) { return kind == SET_MODE ? 5 : kind - SET_ON; }
constexpr uint16_t required_function(uint8_t kind) {
  switch (kind) {
    case SET_ON: return protocol::F_OUTPUT;
    case SET_LEVEL: return protocol::F_BRIGHTNESS;
    case SET_AUTO: return protocol::F_AUTO;
    case SET_THRESHOLD: return protocol::F_THRESHOLD;
    case SET_RUN: return protocol::F_RUN_TIME;
    case SET_MODE: return protocol::F_MODE;
    default: return 0;
  }
}
constexpr uint16_t required_cap(uint8_t kind) {
  switch (kind) {
    case ON: case SET_ON: return protocol::ONOFF;
    case LEVEL: case SET_LEVEL: return protocol::LIGHTNESS;
    case SENSOR: case DESCRIPTORS: case SENSOR_PROPERTY: return protocol::SENSOR;
    default: return protocol::LC;
  }
}
template<class Base> class AttachedEntity : public Base {
 public:
  void attach(Device *device) { this->set_device_(device); }
};
class MeshSelect : public AttachedEntity<select::Select> {
 public:
  void invalidate_state() {
    if (!this->has_state()) return;
    this->set_has_state(false);
    this->state_callback_.call(this->active_index_);
#if defined(USE_SELECT) && defined(USE_CONTROLLER_REGISTRY)
    ControllerRegistry::notify_select_update(this);
#endif
  }
};
class MeshDiagnostic final : public AttachedEntity<text_sensor::TextSensor> {
 public:
  void invalidate_state() {
    if (!this->has_state()) return;
    this->set_has_state(false);
    this->state.clear();
    this->callback_.call(this->state);
#if defined(USE_TEXT_SENSOR) && defined(USE_CONTROLLER_REGISTRY)
    ControllerRegistry::notify_text_sensor_update(this);
#endif
  }
};
class MeshToggle final : public MeshSelect {
 public:
  SteinelMesh *gateway{};
  uint16_t address{};
  SteinelMesh::NodeCommand command{};
 protected:
  void control(const std::string &value) override {
    if (value != "Off" && value != "On") return;
    if (!this->gateway->queue_node_command(this->address, this->command, value == "On"))
      ESP_LOGW("steinel_nodes", "Command rejected for 0x%04x", this->address);
  }
};
class MeshNumber final : public AttachedEntity<number::Number> {
 public:
  SteinelMesh *gateway{};
  uint16_t address{};
  SteinelMesh::NodeCommand command{};
  float scale{1};
 protected:
  void control(float value) override {
    if (!std::isfinite(value) || value < this->traits.get_min_value() ||
        value > this->traits.get_max_value()) return;
    if (!this->gateway->queue_node_command(this->address, this->command,
                                          uint32_t(std::lround(value * this->scale))))
      ESP_LOGW("steinel_nodes", "Command rejected for 0x%04x", this->address);
  }
};
class MeshMode final : public MeshSelect {
 public:
  SteinelMesh *gateway{};
  uint16_t address{};
 protected:
  void control(const std::string &value) override {
    const uint32_t mode = value == "Auto" ? 0 : value == "Always On" ? 1 : 2;
    if (value != "Auto" && value != "Always On" && value != "Always Off") return;
    this->gateway->queue_node_command(this->address, SteinelMesh::NodeCommand::MODE, mode);
  }
};
}  // namespace

struct SteinelMesh::NodeEntities {
  Device device;
  MeshToggle *output{}, *automatic{};
  MeshNumber *level{}, *threshold{}, *run{};
  AttachedEntity<sensor::Sensor> *lux{};
  AttachedEntity<binary_sensor::BinarySensor> *motion{}, *available{};
  AttachedEntity<binary_sensor::BinarySensor> *actual_output{};
  MeshMode *mode{};
  AttachedEntity<text_sensor::TextSensor> *firmware{}, *hardware{};
  AttachedEntity<text_sensor::TextSensor> *model{}, *product{};
  MeshDiagnostic *diagnostics{};
  uint16_t functions{0};
};

bool SteinelMesh::load_catalog_() {
  this->catalog_valid_ = this->catalog_preference_.load(&this->catalog_) &&
                        protocol::valid_catalog(this->catalog_) &&
                        this->catalog_.mesh_uuid == this->config_.mesh_uuid;
  // Migrate the 1.x configuration into the device catalog.
  if (!this->catalog_valid_ && this->configured_ && this->device_key_valid_ &&
      !(this->config_.flags & FLAG_DEVICE_CATALOG) &&
      this->config_.lc_address == this->config_.onoff_address + 1 &&
      this->config_.sensor_address == this->config_.onoff_address + 2) {
    this->catalog_ = protocol::Catalog{};
    this->catalog_.mesh_uuid = this->config_.mesh_uuid; this->catalog_.count = 1;
    auto &node = this->catalog_.nodes[0];
    node.address = this->config_.onoff_address; node.device_key = this->device_key_;
    node.company_id = 0x0563; node.product_id = 0x1DCE; node.element_count = 3;
    node.nightmatiq = true; node.scene = this->config_.scene_number; node.selected = true;
    std::strncpy(node.name, this->config_.node_name, sizeof(node.name) - 1);
    node.elements[0] = {uint16_t(protocol::ONOFF | protocol::SCENE), this->config_.app_key_index, 0};
    node.elements[1] = {protocol::LC, this->config_.app_key_index, 0};
    node.elements[2] = {protocol::SENSOR, this->config_.app_key_index, 0};
    this->catalog_valid_ = protocol::valid_catalog(this->catalog_) && this->save_catalog_(this->catalog_);
  }
  this->legacy_profile_ = !this->catalog_valid_;
  if (!this->functions_preference_.load(&this->stored_functions_) ||
      this->stored_functions_.magic != 0x534D4631 || this->stored_functions_.mesh_uuid != this->config_.mesh_uuid)
    this->stored_functions_ = StoredFunctions{};
  return this->catalog_valid_;
}

bool SteinelMesh::sync_node_functions_() {
  StoredFunctions next{}; next.mesh_uuid = this->catalog_.mesh_uuid;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    for (size_t i = 0; i < this->catalog_.count; ++i) {
      const auto &node = this->catalog_.nodes[i];
      auto &entry = next.entries[i];
      entry.identity = protocol::node_identity(node, this->catalog_.mesh_uuid);
      entry.product = node.product_id;
      // Retain functions only for the same device identity, including while offline.
      for (const auto &old : this->stored_functions_.entries)
        if (old.identity == entry.identity && old.product == entry.product) entry.functions = old.functions;
      entry.functions |= this->node_states_[i].supported;
      entry.functions &= protocol::possible_functions(node);
      if (node.selected && this->node_states_[i].entities != nullptr &&
          entry.functions != this->node_states_[i].entities->functions) changed = true;
    }
  }
  if (!changed) return true;
  if (!this->functions_preference_.save(&next) || !global_preferences->sync()) return false;
  this->stored_functions_ = next;
  this->set_status_("Device functions discovered. Restarting to synchronize Home Assistant entities.");
  this->reboot_at_ = millis() + 1500; this->reboot_pending_.store(true);
  return true;
}

bool SteinelMesh::save_catalog_(const protocol::Catalog &catalog) {
  return protocol::valid_catalog(catalog) && this->catalog_preference_.save(&catalog) &&
         global_preferences->sync();
}

void SteinelMesh::setup_node_entities_() {
  if (!this->catalog_valid_) return;
  // Register before API discovery; persist membership changes and restart.
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    const auto &node = this->catalog_.nodes[i];
    if (!node.selected) continue;
    auto *entities = new NodeEntities{};
    this->node_states_[i].entities = entities;
    const uint32_t identity = protocol::node_identity(node, this->catalog_.mesh_uuid);
    for (const auto &entry : this->stored_functions_.entries)
      if (entry.identity == identity && entry.product == node.product_id) entities->functions = entry.functions;
    entities->functions = protocol::initial_functions(node, entities->functions);
    this->node_states_[i].supported = entities->functions;
    entities->device.set_device_id(identity);
    entities->device.set_name(node.name[0] == '\0' ? "Steinel device" : node.name);
    App.register_device(&entities->device);
    auto add_text = [&](const char *name, const char *value) {
      auto *entity = new AttachedEntity<text_sensor::TextSensor>{};
      entity->attach(&entities->device);
      App.register_text_sensor(entity, name, fnv1_hash(name), uint32_t(ENTITY_CATEGORY_DIAGNOSTIC) << ENTITY_FIELD_ENTITY_CATEGORY_SHIFT);
      if (value != nullptr) entity->publish_state(value);
      return entity;
    };
    char identifier[40];
    std::snprintf(identifier, sizeof(identifier), "0x%04x", node.address);
    add_text("Mesh address", identifier);
    std::snprintf(identifier, sizeof(identifier), "0x%04x / 0x%04x", node.company_id, node.product_id);
    entities->product = add_text("Company / Product ID", identifier);
    entities->model = add_text("Model", protocol::product_name(node.company_id, node.product_id));
    const std::array<uint8_t, 16> empty{};
    if (node.uuid != empty) {
      for (size_t byte = 0; byte < 16; ++byte) std::snprintf(identifier + byte * 2, 3, "%02x", node.uuid[byte]);
      add_text("Device UUID", identifier);
    }
    entities->firmware = add_text("Firmware revision", nullptr);
    entities->hardware = add_text("Hardware revision", nullptr);
    entities->diagnostics = new MeshDiagnostic{};
    entities->diagnostics->attach(&entities->device);
    App.register_text_sensor(entities->diagnostics, "Sensor diagnostics", fnv1_hash("Sensor diagnostics"),
        (uint32_t(ENTITY_CATEGORY_DIAGNOSTIC) << ENTITY_FIELD_ENTITY_CATEGORY_SHIFT) |
        (1UL << ENTITY_FIELD_DISABLED_BY_DEFAULT_SHIFT));
    auto add_toggle = [&](const char *name, NodeCommand command) {
      auto *entity = new MeshToggle{};
      entity->gateway = this; entity->address = node.address; entity->command = command;
      entity->attach(&entities->device);
      entity->traits.set_options({"Off", "On"});
      App.register_select(entity, name, fnv1_hash(name), 0);
      return entity;
    };
    auto add_number = [&](const char *name, NodeCommand command, float maximum,
                          float scale, uint32_t fields) {
      auto *entity = new MeshNumber{};
      entity->gateway = this; entity->address = node.address; entity->command = command;
      entity->scale = scale; entity->attach(&entities->device);
      entity->traits.set_min_value(0); entity->traits.set_max_value(maximum);
      entity->traits.set_step(1); entity->traits.set_mode(number::NUMBER_MODE_BOX);
      App.register_number(entity, name, fnv1_hash(name), fields);
      return entity;
    };
    if (!node.nightmatiq && (entities->functions & protocol::F_OUTPUT))
      entities->output = add_toggle("Output", NodeCommand::ONOFF);
    if (node.nightmatiq && (entities->functions & protocol::F_OUTPUT)) {
      entities->actual_output = new AttachedEntity<binary_sensor::BinarySensor>{};
      entities->actual_output->attach(&entities->device);
      App.register_binary_sensor(entities->actual_output, "Output", fnv1_hash("Output"), 0);
    }
    if (entities->functions & protocol::F_BRIGHTNESS)
      entities->level = add_number("Brightness", NodeCommand::BRIGHTNESS, 100, 655.35f, STEINEL_PERCENT_FIELDS);
    if (!node.nightmatiq && (entities->functions & protocol::F_AUTO))
      entities->automatic = add_toggle("Automatic control", NodeCommand::AUTO);
    if (entities->functions & protocol::F_THRESHOLD)
      entities->threshold = add_number("Light threshold", NodeCommand::THRESHOLD, 167772, 100,
                                       STEINEL_LUX_FIELDS);
    if (entities->functions & protocol::F_RUN_TIME)
      entities->run = add_number("Run time", NodeCommand::RUN_TIME, 16777, 1000, STEINEL_SECONDS_FIELDS);
    if (entities->functions & protocol::F_MODE) {
      entities->mode = new MeshMode{}; entities->mode->gateway = this; entities->mode->address = node.address;
      entities->mode->attach(&entities->device);
      entities->mode->traits.set_options({"Auto", "Always On", "Always Off"});
      App.register_select(entities->mode, "Mode", fnv1_hash("Mode"), 0);
    }
    if (entities->functions & protocol::F_LUX) {
      entities->lux = new AttachedEntity<sensor::Sensor>{};
      entities->lux->attach(&entities->device); entities->lux->set_accuracy_decimals(2);
      App.register_sensor(entities->lux, "Illuminance", fnv1_hash("Illuminance"), STEINEL_LUX_FIELDS);
    }
    if (entities->functions & protocol::F_MOTION) {
      entities->motion = new AttachedEntity<binary_sensor::BinarySensor>{};
      entities->motion->attach(&entities->device);
      App.register_binary_sensor(entities->motion, "Motion", fnv1_hash("Motion"), STEINEL_MOTION_FIELDS);
    }
    entities->available = new AttachedEntity<binary_sensor::BinarySensor>{};
    entities->available->attach(&entities->device);
    App.register_binary_sensor(entities->available, "Available", fnv1_hash("Available"), 0);
    entities->available->publish_state(false);
  }
}

bool SteinelMesh::diagnostic_request_allowed_(const NodeRequest &request) {
  if (!request.diagnostic || is_write(request.kind)) return false;
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  return this->device_diagnostics_.active(millis()) && this->device_diagnostics_.node == request.node;
}

void SteinelMesh::record_diagnostic_(uint16_t source, protocol::DeviceDiagnostics::Event event,
                                    uint32_t opcode, uint16_t property, const uint8_t *data,
                                    size_t length, int32_t error) {
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  this->device_diagnostics_.record(millis(), source, event, opcode, property, data, length, error);
}

bool SteinelMesh::diagnostic_reply_(const esp_ble_mesh_client_common_param_t *params,
                                   uint32_t received, bool valid, uint16_t property) {
  if (params == nullptr || this->access_operation_.load() != AccessOperation::NODE ||
      !this->active_node_request_.diagnostic) return false;
  const bool expected_response = valid &&
      protocol::matching_reply(this->node_destination_.load(), this->access_opcode_.load(),
                               this->node_expected_status_.load(), 0,
                               params->ctx.addr, params->opcode, received, 0, true);
  return this->node_event_(params, received, valid, property) || expected_response;
}

bool SteinelMesh::next_diagnostic_request_(uint32_t now, NodeRequest &request) {
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  auto &session = this->device_diagnostics_;
  if (!session.active(now)) { session.running = false; return false; }
  if (protocol::deadline_pending(now, session.next_probe) || session.node >= this->catalog_.count) return false;
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    const auto &state = this->node_states_[i];
    if (!this->catalog_.nodes[i].selected ||
        !protocol::composition_allows(state.composition_checked, state.composition_matches) ||
        protocol::deadline_pending(now, state.retry_at)) continue;
    for (uint32_t seen : state.value_seen)
      if (seen != 0 && now - seen >= state.stale_after - 2 * POLL_INTERVAL) return false;
  }
  const auto &node = this->catalog_.nodes[session.node];
  constexpr uint8_t steps = 6 + protocol::MAX_ELEMENTS * 5;
  for (uint8_t tried = 0; tried < steps; ++tried) {
    const bool repeated = session.probe_complete;
    const uint8_t step = session.probe++;
    if (session.probe == steps) { session.probe = 1; session.probe_complete = true; }
    request = NodeRequest{}; request.node = session.node; request.diagnostic = true;
    if (step == 0) request.kind = COMPOSITION;
    else if (step < 6) {
      request.kind = step;
      const int element = request.kind == ON ? protocol::output_element(node) :
          protocol::element_with(node, required_cap(request.kind));
      if (element < 0) continue;
      request.element = element;
    } else {
      request.element = (step - 6) / 5;
      if (request.element >= node.element_count ||
          !(node.elements[request.element].capabilities & protocol::SENSOR)) continue;
      const uint8_t probe = (step - 6) % 5;
      request.kind = probe == 0 ? DESCRIPTORS : probe == 1 ? SENSOR : SENSOR_PROPERTY;
      request.value = probe == 2 ? 0x004E : probe == 3 ? 0x000E : 0x0010;
      if (repeated && probe != 1) continue;
    }
    if (repeated && step < 6) continue;
    session.next_probe = now + 3000;
    return true;
  }
  session.probe_complete = true; session.next_probe = now + 3000;
  return false;
}

bool SteinelMesh::queue_node_command(uint16_t address, NodeCommand command, uint32_t value) {
  if (!this->catalog_valid_ || !this->mesh_ready_.load() || this->reboot_pending_.load()) return false;
  const uint8_t kinds[] = {SET_ON, SET_LEVEL, SET_AUTO, SET_THRESHOLD, SET_RUN, SET_MODE};
  const size_t command_index = size_t(command);
  if (command_index >= sizeof(kinds)) return false;
  if (command == NodeCommand::MODE && value > 2) return false;
  if ((command == NodeCommand::ONOFF || command == NodeCommand::AUTO) && value > 1) return false;
  if (command == NodeCommand::BRIGHTNESS && value > 65535) return false;
  if ((command == NodeCommand::THRESHOLD || command == NodeCommand::RUN_TIME) && value >= protocol::UNKNOWN_24)
    return false;
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    const auto &node = this->catalog_.nodes[i];
    if (node.address != address || !node.selected) continue;
    auto &state = this->node_states_[i];
    if (!protocol::composition_allows(state.composition_checked, state.composition_matches) ||
        state.last_seen == 0 || millis() - state.last_seen >= state.stale_after) return false;
    if ((state.supported & protocol::possible_functions(node) & required_function(kinds[command_index])) == 0)
      return false;
    if (command == NodeCommand::MODE && (!node.nightmatiq || protocol::element_with(node, protocol::SCENE) < 0)) return false;
    const int element = command == NodeCommand::ONOFF ? protocol::output_element(node) :
                        protocol::element_with(node, required_cap(kinds[command_index]));
    if (element < 0) return false;
    const auto record_command = [&](const NodeRequest &request) {
      if (request.kind == SET_MODE) { state.controls.cancel(0); state.controls.cancel(2); }
      else if (request.kind == SET_ON || request.kind == SET_LEVEL || request.kind == SET_AUTO)
        state.controls.cancel(5);
      state.controls.begin(command_index, value, millis()); state.dirty = true;
      if ((state.supported & protocol::F_MODE) &&
          (request.kind == SET_MODE || request.kind == SET_ON || request.kind == SET_LEVEL || request.kind == SET_AUTO))
        state.mode.begin_write();
      const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16)};
      this->state_trace_.record(millis(), node.address, node.address + element,
          protocol::StateTrace::QUEUED, 0, 0, 0, request.kind, request.step, request.tid,
          state.on, state.automatic, state.on, state.automatic, bytes, sizeof(bytes));
    };
    // Coalesce an unsent slider command, but never overwrite an in-flight TID.
    for (size_t j = 0; j < this->node_queue_count_; ++j)
      if (this->node_queue_[j].node == i && this->node_queue_[j].kind == kinds[command_index]) {
        this->node_queue_[j].value = value;
        // A replacement value needs a new TID, including when replacing a queued retry.
        this->node_queue_[j].tid = this->next_tid_();
        this->node_queue_[j].attempts = 0;
        this->node_queue_[j].step = (command == NodeCommand::ONOFF || command == NodeCommand::BRIGHTNESS) &&
            (protocol::element_with(node, protocol::LC) < 0 || !(state.supported & protocol::F_AUTO)) ? 1 : 0;
        record_command(this->node_queue_[j]);
        return true;
      }
    // Reserve a slot for continuation/retry of an active transaction.
    if (this->node_queue_count_ >= NODE_QUEUE_SIZE - 1) return false;
    this->node_queue_[this->node_queue_count_++] = NodeRequest{
        uint8_t(i), uint8_t(element), kinds[command_index], 0, this->next_tid_(), value};
    if ((command == NodeCommand::ONOFF || command == NodeCommand::BRIGHTNESS) &&
        (protocol::element_with(node, protocol::LC) < 0 || !(state.supported & protocol::F_AUTO)))
      this->node_queue_[this->node_queue_count_ - 1].step = 1;
    record_command(this->node_queue_[this->node_queue_count_ - 1]);
    return true;
  }
  return false;
}

bool SteinelMesh::send_node_request_(const NodeRequest &request) {
  if (request.node >= this->catalog_.count || !this->mesh_ready_.load() || this->reboot_pending_.load()) return false;
  const auto &node = this->catalog_.nodes[request.node];
  if (request.element >= node.element_count ||
      (request.diagnostic ? !this->diagnostic_request_allowed_(request) : !node.selected)) return false;
  if (is_write(request.kind)) {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    const auto &state = this->node_states_[request.node];
    if (!protocol::composition_allows(state.composition_checked, state.composition_matches) ||
        state.last_seen == 0 || millis() - state.last_seen >= state.stale_after ||
        (node.elements[request.element].capabilities & required_cap(request.kind)) == 0 ||
        (state.supported & protocol::possible_functions(node) & required_function(request.kind)) == 0) return false;
  }
  esp_ble_mesh_client_common_param_t params{};
  params.ctx.net_idx = this->config_.net_key_index;
  params.ctx.app_idx = this->config_.app_key_index;
  params.ctx.addr = node.address + request.element;
  params.ctx.send_ttl = 7;
  params.msg_timeout = request.kind == COMPOSITION ? 4000 : 3000;
  if (node.nightmatiq && (request.kind == ON || request.kind == AUTO))
    params.msg_timeout = NIGHTMATIQ_STATE_TIMEOUT;
  params.model = node_model_(request.kind);
  uint32_t status = 0;
  uint16_t property = 0;
  esp_ble_mesh_generic_client_get_state_t generic_get{};
  esp_ble_mesh_generic_client_set_state_t generic_set{};
  esp_ble_mesh_light_client_get_state_t light_get{};
  esp_ble_mesh_light_client_set_state_t light_set{};
  esp_ble_mesh_sensor_client_get_state_t sensor_get{};
  esp_ble_mesh_cfg_client_get_state_t composition_get{};
  esp_ble_mesh_time_scene_client_set_state_t scene_set{};
  const bool manual_prepare = (request.kind == SET_ON || request.kind == SET_LEVEL) && request.step == 0;
  if (manual_prepare) {
    const int lc = protocol::element_with(node, protocol::LC);
    if (lc < 0) return false;
    params.ctx.addr = node.address + lc;
    params.model = node_model_(AUTO);
    // LC preparation does not require a Mode Status; follow-up reads verify state.
    params.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK;
    light_set.lc_mode_set.mode = 0;
    status = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS;
  } else {
  switch (request.kind) {
    case COMPOSITION:
      params.ctx.app_idx = ESP_BLE_MESH_KEY_DEV;
      params.model->keys[0] = ESP_BLE_MESH_KEY_DEV;
      params.opcode = ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET;
      composition_get.comp_data_get.page = 0;
      status = ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS;
      break;
    case ON: case SET_ON:
      params.opcode = request.kind == ON ? ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET : ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET;
      generic_set.onoff_set.onoff = request.value; generic_set.onoff_set.tid = request.tid;
      status = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS; break;
    case LEVEL: case SET_LEVEL:
      params.opcode = request.kind == LEVEL ? ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET : ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET;
      light_set.lightness_set.lightness = request.value; light_set.lightness_set.tid = request.tid;
      status = ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS; break;
    case AUTO: case SET_AUTO:
      params.opcode = request.kind == AUTO ? ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET : ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET;
      light_set.lc_mode_set.mode = request.value;
      status = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS; break;
    case DESCRIPTORS:
      params.opcode = ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_GET;
      sensor_get.descriptor_get.op_en = false;
      status = ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS; break;
    case SENSOR: case SENSOR_PROPERTY:
      params.opcode = ESP_BLE_MESH_MODEL_OP_SENSOR_GET;
      sensor_get.sensor_get.op_en = request.kind == SENSOR_PROPERTY;
      sensor_get.sensor_get.property_id = request.value;
      if (request.kind == SENSOR_PROPERTY) property = request.value;
      status = ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS; break;
    case SET_MODE:
      if (request.step < 2) {
        params.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK;
        light_set.lc_mode_set.mode = request.value == 0;
        status = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS;
      } else if (request.value == 0) {
        params.ctx.addr = node.address + protocol::element_with(node, protocol::SCENE);
        params.model = node_model_(255);
        params.opcode = ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK;
        scene_set.scene_recall.scene_number = node.scene;
        scene_set.scene_recall.tid = request.tid;
        status = ESP_BLE_MESH_MODEL_OP_SCENE_STATUS;
      } else {
        params.ctx.addr = node.address + protocol::output_element(node);
        params.model = node_model_(ON);
        params.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK;
        generic_set.onoff_set.onoff = request.value == 1;
        generic_set.onoff_set.tid = request.tid;
        status = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS;
      }
      break;
    default:
      property = (request.kind == THRESHOLD || request.kind == SET_THRESHOLD) ? THRESHOLD_PROPERTY : RUN_PROPERTY;
      params.opcode = is_write(request.kind) ? ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET : ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET;
      light_get.lc_property_get.property_id = property;
      this->node_property_storage_ = {uint8_t(request.value), uint8_t(request.value >> 8), uint8_t(request.value >> 16)};
      this->node_property_buffer_.data = this->node_property_storage_.data();
      this->node_property_buffer_.__buf = this->node_property_storage_.data();
      this->node_property_buffer_.size = 3; this->node_property_buffer_.len = 3;
      light_set.lc_property_set.property_id = property;
      light_set.lc_property_set.property_value = &this->node_property_buffer_;
      status = ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS; break;
  }
  }
  if (!this->begin_access_operation_(AccessOperation::NODE, params.opcode)) return false;
  this->access_deadline_.store(millis() + params.msg_timeout + 750);
  this->active_node_request_ = request;
  this->node_destination_.store(params.ctx.addr);
  this->node_expected_status_.store(status);
  this->node_expected_property_.store(property);
  if (!request.diagnostic && (request.kind == ON || request.kind == AUTO || is_write(request.kind))) {
    uint8_t bytes[3]{}; size_t length = 0;
    if (params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
        params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
      bytes[0] = generic_set.onoff_set.onoff; bytes[1] = request.tid; length = 2;
    } else if (params.opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET ||
               params.opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK) {
      bytes[0] = light_set.lc_mode_set.mode; length = 1;
    } else if (params.opcode == ESP_BLE_MESH_MODEL_OP_SCENE_RECALL ||
               params.opcode == ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK) {
      bytes[0] = node.scene; bytes[1] = node.scene >> 8; bytes[2] = request.tid; length = 3;
    } else if (is_write(request.kind)) {
      bytes[0] = request.value; bytes[1] = request.value >> 8; bytes[2] = request.value >> 16; length = 3;
    }
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    const auto &state = this->node_states_[request.node];
    this->state_trace_.record(millis(), node.address, params.ctx.addr, protocol::StateTrace::SEND,
        params.opcode, params.opcode, 0, request.kind, request.step, request.tid,
        state.on, state.automatic, state.on, state.automatic, bytes, length);
  }
  esp_err_t result;
  if (request.kind == COMPOSITION)
    result = esp_ble_mesh_config_client_get_state(&params, &composition_get);
  else if (params.opcode == ESP_BLE_MESH_MODEL_OP_SCENE_RECALL ||
           params.opcode == ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK)
    result = esp_ble_mesh_time_scene_client_set_state(&params, &scene_set);
  else if (params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET ||
           params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
           params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK)
    result = params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET ? esp_ble_mesh_generic_client_get_state(&params, &generic_get) :
                                 esp_ble_mesh_generic_client_set_state(&params, &generic_set);
  else if (request.kind == SENSOR || request.kind == SENSOR_PROPERTY || request.kind == DESCRIPTORS)
    result = esp_ble_mesh_sensor_client_get_state(&params, &sensor_get);
  else
    result = !is_write(request.kind) ? esp_ble_mesh_light_client_get_state(&params, &light_get) :
                                    esp_ble_mesh_light_client_set_state(&params, &light_set);
  const bool accepted = this->record_access_send_result_(AccessOperation::NODE, params.opcode, result);
  if (!accepted && !request.diagnostic) {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    const auto &state = this->node_states_[request.node];
    this->state_trace_.record(millis(), node.address, params.ctx.addr, protocol::StateTrace::SEND_ERROR,
        params.opcode, params.opcode, 0, request.kind, request.step, request.tid,
        state.on, state.automatic, state.on, state.automatic, nullptr, 0, result);
  }
  if (request.diagnostic)
    this->record_diagnostic_(params.ctx.addr, accepted ? protocol::DeviceDiagnostics::SENT :
                            protocol::DeviceDiagnostics::SEND_ERROR, params.opcode,
                            request.kind == SENSOR_PROPERTY ? request.value : property, nullptr, 0, result);
  if (!accepted) this->finish_node_request_(false);
  else if (params.opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK ||
           params.opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK ||
           params.opcode == ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK) {
    // Complete only the send stage. No device state is inferred from SDK acceptance.
    this->finish_node_request_(true);
    this->complete_access_operation_(params.opcode, true);
  }
  return accepted;
}

void SteinelMesh::finish_node_request_(bool success) {
  this->node_completion_success_.store(success);
  this->node_completion_pending_.store(true);
}

bool SteinelMesh::node_event_(const esp_ble_mesh_client_common_param_t *params, uint32_t received,
                                bool success, uint16_t property) {
  if (params == nullptr || this->access_operation_.load() != AccessOperation::NODE) return false;
  // An authenticated diagnostic reply proves transport even for another property.
  if (success && this->active_node_request_.diagnostic &&
      protocol::matching_reply(this->node_destination_.load(), this->access_opcode_.load(),
                               this->node_expected_status_.load(), 0,
                               params->ctx.addr, params->opcode, received, 0, true)) {
    this->mesh_rx_messages_.fetch_add(1);
    this->record_mesh_rssi_(params->ctx);
  }
  if (!protocol::matching_reply(this->node_destination_.load(), this->access_opcode_.load(),
                                this->node_expected_status_.load(), this->node_expected_property_.load(),
                                params->ctx.addr, params->opcode, received, property, success)) return false;
  // Publish completion before releasing the slot to prevent callback/loop races.
  this->finish_node_request_(success);
  if (!success && !this->active_node_request_.diagnostic) this->mesh_timeouts_.fetch_add(1);
  this->complete_access_operation_(params->opcode, success);
  return true;
}

bool SteinelMesh::node_response_value_matches_(const esp_ble_mesh_client_common_param_t *params,
                                               uint32_t received, uint32_t value) const {
  if (params == nullptr || this->access_operation_.load() != AccessOperation::NODE ||
      params->ctx.addr != this->node_destination_.load() || params->opcode != this->access_opcode_.load() ||
      received != this->node_expected_status_.load()) return true;
  const auto &request = this->active_node_request_;
  if (!is_write(request.kind)) return true;
  if ((request.kind == SET_ON || request.kind == SET_LEVEL) && request.step == 0) return value == 0;
  if (request.kind == SET_MODE)
    return request.step < 2 ? value == uint32_t(request.value == 0) : value == uint32_t(request.value == 1);
  return value == request.value;
}

bool SteinelMesh::node_generic_event_(esp_ble_mesh_generic_client_cb_event_t event,
                                        esp_ble_mesh_generic_client_cb_param_t *param) {
  if (!this->catalog_valid_ || param->params == nullptr) return false;
  const bool success = event != ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT && param->error_code == 0 &&
                       param->params->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS &&
                       param->status_cb.onoff_status.present_onoff <= 1;
  if (success) {
    const uint8_t value = param->status_cb.onoff_status.present_onoff;
    this->record_diagnostic_(param->params->ctx.addr, protocol::DeviceDiagnostics::RESPONSE,
                            param->params->ctx.recv_op, 0, &value, 1);
  }
  if (this->diagnostic_reply_(param->params, param->params->ctx.recv_op, success)) return true;
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    const auto &node = this->catalog_.nodes[i];
    if (!node.selected) continue;
    const int element = protocol::output_element(node);
    if (element < 0 || node.address + element != param->params->ctx.addr) continue;
    const int8_t before_on = this->node_states_[i].on, before_auto = this->node_states_[i].automatic;
    if (success && protocol::composition_allows(this->node_states_[i].composition_checked,
        this->node_states_[i].composition_matches)) {
      auto &state = this->node_states_[i];
      const auto value = param->status_cb.onoff_status.present_onoff;
      if (state.on != value)
        ESP_LOGI("steinel_nodes", "Output 0x%04x: %d -> %u (OnOff, event %u)",
                 node.address, state.on, value, unsigned(event));
      state.on = value;
      state.supported |= protocol::F_OUTPUT;
      if (node.nightmatiq && (state.supported & protocol::F_AUTO) && protocol::element_with(node, protocol::SCENE) >= 0)
        state.supported |= protocol::F_MODE;
      state.value_seen[0] = millis();
      state.last_seen = millis(); state.dirty = true;
      this->mesh_rx_messages_.fetch_add(1);
      this->mesh_generic_rx_.fetch_add(1); this->record_mesh_rssi_(param->params->ctx);
    }
    uint8_t bytes[3]{}; size_t length = 0;
    if (success) {
      const auto &status = param->status_cb.onoff_status;
      bytes[0] = status.present_onoff; length = 1;
      if (status.op_en) { bytes[1] = status.target_onoff; bytes[2] = status.remain_time; length = 3; }
    }
    const auto &state = this->node_states_[i];
    this->state_trace_.record(millis(), node.address, param->params->ctx.addr,
        event == ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT ? protocol::StateTrace::TIMEOUT : protocol::StateTrace::RESPONSE,
        param->params->ctx.recv_op, param->params->opcode, uint8_t(event), 0, 0, 0,
        before_on, before_auto, state.on, state.automatic, bytes, length, param->error_code);
    const auto &status = param->status_cb.onoff_status;
    this->node_event_(param->params, param->params->ctx.recv_op, success &&
        this->node_response_value_matches_(param->params, param->params->ctx.recv_op,
                                           status.op_en ? status.target_onoff : status.present_onoff));
    return true;
  }
  this->node_event_(param->params, param->params->ctx.recv_op, success);
  return false;
}

bool SteinelMesh::node_light_event_(esp_ble_mesh_light_client_cb_event_t event,
                                      esp_ble_mesh_light_client_cb_param_t *param) {
  if (!this->catalog_valid_ || param->params == nullptr) return false;
  const uint32_t received = param->params->ctx.recv_op;
  bool valid = event != ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT && param->error_code == 0;
  uint16_t property = 0;
  if (valid && received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS) {
    property = param->status_cb.lc_property_status.property_id;
    const auto *buffer = param->status_cb.lc_property_status.property_value;
    valid = buffer != nullptr && buffer->data != nullptr && buffer->len == 3;
  }
  if (valid && received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS)
    valid = param->status_cb.lc_mode_status.mode <= 1;
  if (valid && received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS)
    valid = param->status_cb.lc_light_onoff_status.present_light_onoff <= 1;
  if (valid) {
    uint8_t bytes[3]{}; size_t length = 0;
    if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
      const auto value = param->status_cb.lightness_status.present_lightness;
      bytes[0] = value; bytes[1] = value >> 8; length = 2;
    } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS) {
      bytes[0] = param->status_cb.lc_mode_status.mode; length = 1;
    } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS) {
      bytes[0] = param->status_cb.lc_light_onoff_status.present_light_onoff; length = 1;
    } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS) {
      std::memcpy(bytes, param->status_cb.lc_property_status.property_value->data, 3); length = 3;
    }
    if (length) this->record_diagnostic_(param->params->ctx.addr, protocol::DeviceDiagnostics::RESPONSE,
                                       received, property, bytes, length);
  }
  if (this->diagnostic_reply_(param->params, received, valid, property)) return true;
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    const auto &node = this->catalog_.nodes[i];
    if (!node.selected) continue;
    const uint16_t cap = received == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS ? protocol::LIGHTNESS : protocol::LC;
    const int element = protocol::element_with(node, cap);
    if (element < 0 || node.address + element != param->params->ctx.addr) continue;
    if (!valid || !protocol::composition_allows(this->node_states_[i].composition_checked,
        this->node_states_[i].composition_matches)) {
      if (event == ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT) {
        const auto &state = this->node_states_[i];
        this->state_trace_.record(millis(), node.address, param->params->ctx.addr, protocol::StateTrace::TIMEOUT,
            received, param->params->opcode, uint8_t(event), 0, 0, 0,
            state.on, state.automatic, state.on, state.automatic, nullptr, 0, param->error_code);
      }
      this->node_event_(param->params, received, false, property); return true;
    }
    auto &state = this->node_states_[i];
    const int8_t before_on = state.on, before_auto = state.automatic;
    if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
      state.brightness = param->status_cb.lightness_status.present_lightness;
      state.brightness_known = true;
      state.supported |= protocol::F_BRIGHTNESS;
      state.value_seen[1] = millis();
    } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS) {
      const auto value = param->status_cb.lc_mode_status.mode;
      if (state.automatic != value)
        ESP_LOGI("steinel_nodes", "Automatic control 0x%04x: %d -> %u (event %u)",
                 node.address, state.automatic, value, unsigned(event));
      state.automatic = value;
      state.supported |= protocol::F_AUTO;
      if (node.nightmatiq && (state.supported & protocol::F_OUTPUT) && protocol::element_with(node, protocol::SCENE) >= 0)
        state.supported |= protocol::F_MODE;
      state.value_seen[2] = millis();
    } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS &&
               protocol::output_element(node) >= 0) {
      const auto value = param->status_cb.lc_light_onoff_status.present_light_onoff;
      if (state.on != value)
        ESP_LOGI("steinel_nodes", "Output 0x%04x: %d -> %u (Light LC, event %u)",
                 node.address, state.on, value, unsigned(event));
      state.on = value;
      state.supported |= protocol::F_OUTPUT;
      state.value_seen[0] = millis();
    }
    else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS) {
      const uint32_t value = protocol::le24(param->status_cb.lc_property_status.property_value->data);
      if (property == THRESHOLD_PROPERTY) { state.threshold = value; state.value_seen[3] = millis(); if (value != protocol::UNKNOWN_24) state.supported |= protocol::F_THRESHOLD; }
      else if (property == RUN_PROPERTY) {
        if (protocol::possible_functions(node) & protocol::F_RUN_TIME) {
          state.run_time = value; state.value_seen[4] = millis();
          if (value != protocol::UNKNOWN_24) state.supported |= protocol::F_RUN_TIME;
        }
      }
      else return true;
    } else return true;
    if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS ||
        received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS) {
      uint8_t bytes[3]{}; size_t length = 1;
      if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS) bytes[0] = param->status_cb.lc_mode_status.mode;
      else {
        const auto &status = param->status_cb.lc_light_onoff_status;
        bytes[0] = status.present_light_onoff;
        if (status.op_en) { bytes[1] = status.target_light_onoff; bytes[2] = status.remain_time; length = 3; }
      }
      this->state_trace_.record(millis(), node.address, param->params->ctx.addr, protocol::StateTrace::RESPONSE,
          received, param->params->opcode, uint8_t(event), 0, 0, 0,
          before_on, before_auto, state.on, state.automatic, bytes, length);
    }
    state.last_seen = millis(); state.dirty = true; this->mesh_rx_messages_.fetch_add(1);
    this->record_mesh_rssi_(param->params->ctx);
    uint32_t confirmed = 0;
    if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
      const auto &status = param->status_cb.lightness_status;
      confirmed = status.op_en ? status.target_lightness : status.present_lightness;
    } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS) confirmed = state.automatic;
    else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS) {
      const auto &status = param->status_cb.lc_light_onoff_status;
      confirmed = status.op_en ? status.target_light_onoff : status.present_light_onoff;
    }
    else confirmed = protocol::le24(param->status_cb.lc_property_status.property_value->data);
    this->node_event_(param->params, received, valid &&
        this->node_response_value_matches_(param->params, received, confirmed), property);
    return true;
  }
  this->node_event_(param->params, received, valid, property);
  return false;
}

bool SteinelMesh::node_sensor_event_(esp_ble_mesh_sensor_client_cb_event_t event,
                                       esp_ble_mesh_sensor_client_cb_param_t *param) {
  if (!this->catalog_valid_ || param->params == nullptr) return false;
  const auto received = param->params->ctx.recv_op;
  const bool descriptors = received == ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS;
  const auto *buffer = descriptors ? param->status_cb.descriptor_status.descriptor :
                                    param->status_cb.sensor_status.marshalled_sensor_data;
  bool valid = event != ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT && param->error_code == 0 &&
               (descriptors || received == ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS) && buffer != nullptr;
  const uint16_t requested_property = !descriptors && this->access_operation_.load() == AccessOperation::NODE &&
      param->params->ctx.addr == this->node_destination_.load() ? this->node_expected_property_.load() : 0;
  uint16_t response_property = 0;
  if (valid) valid = descriptors ? protocol::sensor_descriptors(buffer->data, buffer->len, [](uint16_t) {}) :
                                 protocol::sensor_records(buffer->data, buffer->len, [&](const auto &record) {
                                   if (record.property == requested_property) response_property = requested_property;
                                 });
  // A malformed response fails the request; a valid unrelated response leaves it pending.
  if (!valid) response_property = requested_property;
  if (event != ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT && param->error_code == 0 && buffer != nullptr &&
      (descriptors || received == ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS))
    this->record_diagnostic_(param->params->ctx.addr, !valid ? protocol::DeviceDiagnostics::INVALID :
                            event == ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT ? protocol::DeviceDiagnostics::PUBLICATION :
                            protocol::DeviceDiagnostics::RESPONSE, received, 0, buffer->data, buffer->len);
  if (this->diagnostic_reply_(param->params, received, valid, response_property)) return true;
  std::lock_guard<std::mutex> lock(this->node_mutex_);
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    const auto &node = this->catalog_.nodes[i];
    if (!node.selected || param->params->ctx.addr < node.address ||
        param->params->ctx.addr >= node.address + node.element_count) continue;
    const size_t element = param->params->ctx.addr - node.address;
    if (!(node.elements[element].capabilities & protocol::SENSOR)) continue;
    valid = valid && protocol::composition_allows(this->node_states_[i].composition_checked,
                                                this->node_states_[i].composition_matches);
    if (valid) {
      auto &state = this->node_states_[i];
      if (event == ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT && !descriptors) {
        state.sensor_push_at[element] = millis();
        state.retry_at = 0;
        state.failures = 0;
      }
      if (descriptors) {
        protocol::sensor_descriptors(buffer->data, buffer->len, [&](uint16_t property) {
          if (property == 0x004E) state.supported |= protocol::F_LUX;
          if (property == 0x0042 || property == 0x004D) state.supported |= protocol::F_MOTION;
        });
      } else protocol::sensor_records(buffer->data, buffer->len, [&](const protocol::SensorRecord &record) {
        state.sensors.store(element, record, millis());
        if (record.property == 0x004E && record.length == 3) {
          state.lux = protocol::le24(record.value); state.value_seen[5] = millis();
          state.lux_element = element;
          state.lux_property = event != ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT &&
              this->access_operation_.load() == AccessOperation::NODE &&
              this->active_node_request_.kind == SENSOR_PROPERTY && this->active_node_request_.value == 0x004E &&
              param->params->ctx.addr == this->node_destination_.load();
          state.supported |= protocol::F_LUX;
        }
        if (record.property == 0x0042 && record.length == 1) {
          state.motion = record.value[0] <= 200 ? (record.value[0] != 0) : -1;
          state.value_seen[6] = millis();
          state.motion_element = element;
          state.supported |= protocol::F_MOTION;
        }
        if (record.property == 0x004D && record.length == 1) {
          state.motion = record.value[0] <= 1 ? record.value[0] : -1;
          state.value_seen[6] = millis();
          state.motion_element = element;
          state.supported |= protocol::F_MOTION;
        }
        if (record.property == 0x000E) protocol::revision_string(record.value, record.length, state.firmware);
        if (record.property == 0x0010) protocol::revision_string(record.value, record.length, state.hardware);
      });
      state.last_seen = millis(); state.dirty = true; this->mesh_rx_messages_.fetch_add(1);
      this->mesh_sensor_rx_.fetch_add(1); this->record_mesh_rssi_(param->params->ctx);
    }
    this->node_event_(param->params, received, valid, response_property);
    return true;
  }
  this->node_event_(param->params, received, valid, response_property);
  return false;
}

bool SteinelMesh::node_scene_event_(esp_ble_mesh_time_scene_client_cb_event_t event,
                                      esp_ble_mesh_time_scene_client_cb_param_t *param) {
  if (!this->catalog_valid_ || param->params == nullptr || this->access_operation_.load() != AccessOperation::NODE ||
      this->active_node_request_.kind != SET_MODE) return false;
  const bool valid = event != ESP_BLE_MESH_TIME_SCENE_CLIENT_TIMEOUT_EVT && param->error_code == 0 &&
                     param->params->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_SCENE_STATUS &&
                     param->status_cb.scene_status.status_code == 0 &&
                     (param->status_cb.scene_status.current_scene == this->catalog_.nodes[this->active_node_request_.node].scene ||
                      (param->status_cb.scene_status.op_en && param->status_cb.scene_status.target_scene ==
                       this->catalog_.nodes[this->active_node_request_.node].scene));
  if (valid) {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    auto &state = this->node_states_[this->active_node_request_.node];
    if (param->params->ctx.addr == this->node_destination_.load()) {
      state.last_seen = millis(); state.dirty = true;
      this->mesh_rx_messages_.fetch_add(1); this->record_mesh_rssi_(param->params->ctx);
    }
  }
  return this->node_event_(param->params, param->params->ctx.recv_op, valid);
}

bool SteinelMesh::node_composition_event_(esp_ble_mesh_cfg_client_cb_event_t event,
                                            esp_ble_mesh_cfg_client_cb_param_t *param) {
  if (!this->catalog_valid_ || param->params == nullptr || this->access_operation_.load() != AccessOperation::NODE ||
      this->active_node_request_.kind != COMPOSITION || param->params->ctx.addr != this->node_destination_.load() ||
      param->params->opcode != ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) return false;
  const auto &status = param->status_cb.comp_data_status;
  const auto *buffer = status.composition_data;
  std::array<uint16_t, protocol::MAX_ELEMENTS> caps{};
  uint8_t count = 0;
  const bool valid = event == ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT && param->error_code == 0 &&
                     status.page == 0 && buffer != nullptr && protocol::composition(buffer->data, buffer->len, caps, count);
  if (event == ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT && param->error_code == 0 && status.page == 0 && buffer != nullptr) {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    this->device_diagnostics_.composition(millis(), param->params->ctx.addr, buffer->data, buffer->len, valid);
  }
  if (this->diagnostic_reply_(param->params, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS, valid)) return true;
  if (valid) {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    const auto &node = this->catalog_.nodes[this->active_node_request_.node];
    auto &state = this->node_states_[this->active_node_request_.node];
    const bool identity_changed = state.composition_checked &&
        (state.company != protocol::le16(buffer->data) || state.product != protocol::le16(buffer->data + 2) ||
         state.composition_version != protocol::le16(buffer->data + 4));
    if (identity_changed) { state.firmware[0] = '\0'; state.hardware[0] = '\0'; }
    state.company = protocol::le16(buffer->data); state.product = protocol::le16(buffer->data + 2);
    state.composition_version = protocol::le16(buffer->data + 4);
    state.composition_checked = true;
    state.composition_at = millis() + IDENTITY_INTERVAL;
    state.composition_matches = count == node.element_count && state.company == 0x0563 &&
                                (node.product_id == 0 || node.product_id == state.product);
    for (size_t i = 0; i < node.element_count; ++i)
      if ((caps[i] & node.elements[i].capabilities) != node.elements[i].capabilities) state.composition_matches = false;
    uint8_t major, minor, patch;
    if (state.composition_matches && protocol::firmware_from_vid(state.company, state.product,
        state.composition_version, major, minor, patch))
      std::snprintf(state.firmware, sizeof(state.firmware), "%u.%u.%u", major, minor, patch);
    // Retain authenticated 1.x metadata only for its original device.
    if (node.nightmatiq && state.composition_matches &&
        this->advertised_identity_valid_.load() && this->advertised_product_id_.load() == state.product &&
        ((node.address == this->identity_node_address_ && this->identity_found_this_boot_.load() &&
          ((!identity_changed && this->advertised_composition_version_id_.load() == 0) ||
           this->advertised_composition_version_id_.load() == state.composition_version)) ||
         (node.address == this->config_.onoff_address && state.composition_version != 0 &&
          this->advertised_composition_version_id_.load() == state.composition_version))) {
      std::snprintf(state.firmware, sizeof(state.firmware), "%u.%u.%u",
          this->advertised_firmware_major_.load(), this->advertised_firmware_minor_.load(),
          this->advertised_firmware_patch_.load());
      std::snprintf(state.hardware, sizeof(state.hardware), "%u", this->advertised_hardware_version_.load());
      if (node.address == this->config_.onoff_address) {
        this->advertised_composition_version_id_.store(state.composition_version);
        this->advertised_identity_save_pending_.store(true);
      }
    }
    state.last_seen = millis(); state.dirty = true;
    this->mesh_rx_messages_.fetch_add(1); this->record_mesh_rssi_(param->params->ctx);
  }
  this->node_event_(param->params, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS, valid);
  return true;
}

void SteinelMesh::advance_nodes_(uint32_t now) {
  if (!this->catalog_valid_) return;
  // Publish in the ESPHome task, outside the mutex: callbacks may queue commands.
  for (size_t i = 0; i < this->catalog_.count; ++i) {
    NodeState state;
    bool publish = false;
    {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      // Bluetooth callbacks may have refreshed timestamps since loop() sampled now.
      now = millis();
      auto &stored = this->node_states_[i];
      for (size_t field = 0; field < stored.value_seen.size(); ++field) {
        if (field == 3 || field == 4) continue;
        if (stored.value_seen[field] == 0 || now - stored.value_seen[field] < stored.stale_after) continue;
        stored.value_seen[field] = 0; stored.dirty = true;
        switch (field) {
          case 0: stored.on = -1; break;
          case 1: stored.brightness_known = false; break;
          case 2: stored.automatic = -1; break;
          case 5: stored.lux = protocol::UNKNOWN_24; break;
          case 6: stored.motion = -1; break;
        }
      }
      const bool available = protocol::composition_allows(stored.composition_checked, stored.composition_matches) &&
                             stored.last_seen != 0 && now - stored.last_seen < stored.stale_after;
      const int8_t previous_mode = stored.mode.value;
      stored.mode.observe(now, stored.on, stored.automatic, stored.value_seen[0], stored.value_seen[2], stored.stale_after);
      stored.dirty |= previous_mode != stored.mode.value;
      const int64_t confirmed[] = {stored.on, stored.brightness_known ? int64_t(stored.brightness) : -1,
          stored.automatic, stored.threshold == protocol::UNKNOWN_24 ? -1 : int64_t(stored.threshold),
          stored.run_time == protocol::UNKNOWN_24 ? -1 : int64_t(stored.run_time), stored.mode.value};
      for (size_t field = 0; field < stored.controls.values.size(); ++field)
        stored.dirty |= stored.controls.observe(field, confirmed[field],
            field == 5 ? stored.mode.confirmed_at : stored.value_seen[field], now);
      publish = stored.dirty || stored.was_available != available;
      stored.was_available = available; stored.dirty = false; state = stored;
    }
    auto *entities = state.entities;
    if (!publish || entities == nullptr) continue;
    entities->available->publish_state(state.was_available);
    if (entities->actual_output) {
      if (state.was_available && state.on >= 0) entities->actual_output->publish_state(state.on != 0);
      else entities->actual_output->invalidate_state();
    }
    if (entities->mode) {
      const int mode = state.controls.has(5) ? state.controls.values[5] : state.mode.value;
      if (state.was_available && mode >= 0)
        entities->mode->publish_state(mode == 0 ? "Auto" : mode == 1 ? "Always On" : "Always Off");
      else entities->mode->invalidate_state();
    }
    entities->firmware->publish_state(state.firmware);
    entities->hardware->publish_state(state.hardware);
    if (state.composition_matches) {
      char identifier[24];
      std::snprintf(identifier, sizeof(identifier), "0x%04x / 0x%04x", state.company, state.product);
      entities->product->publish_state(identifier);
      entities->model->publish_state(protocol::product_name(state.company, state.product));
    }
    if (entities->output) {
      const int on = state.controls.has(0) ? state.controls.values[0] : state.on;
      if (state.was_available && on >= 0) entities->output->publish_state(on ? "On" : "Off");
      else entities->output->invalidate_state();
    }
    if (entities->automatic) {
      const int automatic = state.controls.has(2) ? state.controls.values[2] : state.automatic;
      if (state.was_available && automatic >= 0) entities->automatic->publish_state(automatic ? "On" : "Off");
      else entities->automatic->invalidate_state();
    }
    if (entities->diagnostics) {
      if (!state.was_available || state.sensors.count == 0) entities->diagnostics->invalidate_state();
      else {
        char text[256]{};
        size_t used = 0;
        for (size_t j = 0; j < state.sensors.count; ++j) {
          const auto &value = state.sensors.values[j];
          used += std::snprintf(text + used, sizeof(text) - used, "%s%04x/%04x=", j ? ";" : "",
                               this->catalog_.nodes[i].address + value.element, value.property);
          if (now - value.seen >= state.stale_after || value.length == 0)
            text[used++] = '?';
          else {
            for (size_t k = 0; k < std::min<size_t>(value.length, value.raw.size()); ++k)
              used += std::snprintf(text + used, sizeof(text) - used, "%02x", value.raw[k]);
            if (value.length > value.raw.size()) text[used++] = '+';
          }
        }
        if (state.sensors.overflow) text[used++] = '+';
        text[used] = '\0';
        entities->diagnostics->publish_state(text);
      }
    }
    if (entities->lux) entities->lux->publish_state(state.was_available && state.lux != protocol::UNKNOWN_24 ? state.lux / 100.0f : NAN);
    const uint32_t threshold = state.controls.has(3) ? state.controls.values[3] : state.threshold;
    const uint32_t run_time = state.controls.has(4) ? state.controls.values[4] : state.run_time;
    const int level = state.controls.has(1) ? state.controls.values[1] : state.brightness_known ? state.brightness : -1;
    if (entities->threshold) entities->threshold->publish_state(state.was_available && threshold != protocol::UNKNOWN_24 ? threshold / 100.0f : NAN);
    if (entities->run) entities->run->publish_state(state.was_available && run_time != protocol::UNKNOWN_24 ? run_time / 1000.0f : NAN);
    if (entities->level) entities->level->publish_state(state.was_available && level >= 0 ? level / 655.35f : NAN);
    if (entities->motion) {
      if (state.was_available && state.motion >= 0) entities->motion->publish_state(state.motion != 0);
      else entities->motion->invalidate_state();
    }
  }
  if (this->node_completion_pending_.exchange(false)) {
    const bool success = this->node_completion_success_.load();
    auto request = this->active_node_request_;
    if (request.diagnostic) {
      if (!success) this->record_diagnostic_(this->node_destination_.load(), protocol::DeviceDiagnostics::TIMEOUT,
                                            this->node_expected_status_.load());
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      this->device_diagnostics_.next_probe = now + 1500;
    } else {
    {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      auto &state = this->node_states_[request.node];
      now = millis();
      const bool optional = request.kind == DESCRIPTORS || request.kind == SENSOR_PROPERTY ||
          request.kind == THRESHOLD || request.kind == RUN ||
          request.kind == COMPOSITION;
      const bool lc_prepare = (request.kind == SET_MODE && request.step < 2) ||
          ((request.kind == SET_ON || request.kind == SET_LEVEL) && request.step == 0);
      if (success && !lc_prepare) state.failures = 0;
      else if (!optional) state.failures = std::min<unsigned>(state.failures + 1, 8);
      if (success && !lc_prepare) state.retry_at = 0;
      if (!is_write(request.kind) && !optional) {
        // Probe each model before declaring the node offline. A missing model
        // reply must not pause the other functions of a responding device.
        const bool responding = state.last_seen != 0 && now - state.last_seen < state.stale_after;
        state.retry_at = success || responding || !state.discovery_complete ? 0 :
            now + (60000U << std::min<uint8_t>(state.failures - 1, 2));
      }
      const bool newer_queued = std::any_of(this->node_queue_.begin(),
          this->node_queue_.begin() + this->node_queue_count_, [&](const auto &item) {
            return item.node == request.node && item.kind == request.kind;
          });
      const bool continuation = (request.kind == SET_MODE && request.step < 3) ||
          ((request.kind == SET_ON || request.kind == SET_LEVEL) && request.step == 0);
      if (success && !newer_queued && continuation && this->node_queue_count_ < NODE_QUEUE_SIZE) {
        ++request.step; request.attempts = 0;
        std::move_backward(this->node_queue_.begin(), this->node_queue_.begin() + this->node_queue_count_,
                           this->node_queue_.begin() + this->node_queue_count_ + 1);
        this->node_queue_[0] = request; ++this->node_queue_count_;
      }
      if (is_write(request.kind) && !newer_queued &&
          ((success && !continuation) || (!success && request.attempts != 0))) {
        // The device may have applied a write whose acknowledgement was lost.
        state.poll_step = 0; state.poll_at = now + 300; state.retry_at = 0; state.force_poll = true;
        const size_t control_field = command_field(request.kind);
        const uint32_t confirmation_at = success && request.kind != SET_MODE ?
            state.value_seen[control_field] : now;
        state.controls.ready(control_field, confirmation_at);
        uint8_t fields = request.kind == SET_MODE ? 5 :
            uint8_t(1U << command_field(request.kind));
        if (state.supported & protocol::F_OUTPUT) fields |= 1;
        if ((request.kind == SET_ON || request.kind == SET_LEVEL) && (state.supported & protocol::F_AUTO)) fields |= 4;
        if (request.kind == SET_AUTO && request.value == 0 && (state.supported & protocol::F_MODE)) fields |= 1;
        state.verify_fields |= fields; state.verify_next = 0;
        for (size_t field = 0; field < state.verify_attempts.size(); ++field)
          if (fields & (1U << field)) state.verify_attempts[field] = 0;
        const bool mode_write = request.kind == SET_MODE || request.kind == SET_ON ||
            request.kind == SET_LEVEL || request.kind == SET_AUTO;
        const bool mode_write_queued = std::any_of(this->node_queue_.begin(),
            this->node_queue_.begin() + this->node_queue_count_, [&](const auto &item) {
              return item.node == request.node && (item.kind == SET_MODE || item.kind == SET_ON ||
                  item.kind == SET_LEVEL || item.kind == SET_AUTO);
            });
        if (mode_write && state.mode.writing && !mode_write_queued) state.mode.finish_write(now);
      }
      if (request.verification && request.kind >= ON && request.kind <= RUN) {
        const size_t field = request.kind - ON;
        const bool output_unsettled = request.kind == ON &&
            ((state.controls.has(0) && state.on != int(state.controls.values[0])) ||
             (state.controls.has(5) && state.controls.values[5] != 0 &&
              state.on != int(state.controls.values[5] == 1)));
        if ((!success || output_unsettled) &&
            ++state.verify_attempts[field] < (field == 0 ? OUTPUT_VERIFY_ATTEMPTS : 3)) {
          state.verify_fields |= 1U << field;
          // A missing LC reply must not defer physical output to the next polling cycle.
          if (field == 0) state.verify_next = 0;
        }
      }
      if (!success && !newer_queued &&
          is_write(request.kind) && request.attempts == 0 &&
          this->node_queue_count_ < NODE_QUEUE_SIZE) {
        request.attempts++;
        std::move_backward(this->node_queue_.begin(), this->node_queue_.begin() + this->node_queue_count_,
                           this->node_queue_.begin() + this->node_queue_count_ + 1);
        this->node_queue_[0] = request; ++this->node_queue_count_;
      }
    }
    }
    this->node_next_request_at_ = now + 200;
  }
  if (!this->mesh_ready_.load() || this->reboot_pending_.load() || this->node_completion_pending_.load() ||
      this->access_operation_.load() != AccessOperation::NONE ||
      this->composition_query_in_flight_.load() || this->control_kind_ != ControlKind::NONE ||
      this->control_request_pending_() || this->poll_stage_ != 0 ||
      protocol::deadline_pending(now, this->node_next_request_at_)) return;
  NodeRequest request{};
  bool queued = false;
  bool mode_continuation;
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    mode_continuation = this->node_queue_count_ != 0 &&
        this->node_queue_[0].kind == SET_MODE && this->node_queue_[0].step != 0;
  }
  // Keep duplicated mode actions inside the Mesh transaction window.
  if (this->node_write_burst_ >= 2 && !mode_continuation) {
    this->node_write_burst_ = 0;
    this->poll_nodes_(now);
    if (this->access_operation_.load() != AccessOperation::NONE || this->node_completion_pending_.load() ||
        this->reboot_pending_.load()) return;
  }
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    if (this->node_queue_count_ != 0) {
      request = this->node_queue_[0];
      std::move(this->node_queue_.begin() + 1, this->node_queue_.begin() + this->node_queue_count_, this->node_queue_.begin());
      --this->node_queue_count_; queued = true;
    }
  }
  if (queued) {
    ++this->node_write_burst_;
    if (!this->send_node_request_(request)) {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      auto &state = this->node_states_[request.node];
      const bool mode_write_queued = std::any_of(this->node_queue_.begin(),
          this->node_queue_.begin() + this->node_queue_count_, [&](const auto &item) {
            return item.node == request.node && (item.kind == SET_MODE || item.kind == SET_ON ||
                item.kind == SET_LEVEL || item.kind == SET_AUTO);
          });
      if (!this->node_completion_pending_.load() && state.mode.writing && !mode_write_queued) {
        // Admission can change before dispatch. A rejected command must not
        // leave future authenticated readings blocked as an unfinished write.
        state.mode.finish_write(millis());
        state.poll_step = 0; state.poll_at = millis() + 300; state.force_poll = true;
      }
      if (!this->node_completion_pending_.load() && is_write(request.kind)) {
        const size_t field = command_field(request.kind);
        const bool newer_queued = std::any_of(this->node_queue_.begin(),
            this->node_queue_.begin() + this->node_queue_count_, [&](const auto &item) {
              return item.node == request.node && item.kind == request.kind;
            });
        if (!newer_queued && state.controls.has(field) && state.controls.values[field] == request.value) {
          state.controls.cancel(field); state.dirty = true;
        }
      }
      this->node_next_request_at_ = millis() + 200;
    }
    return;
  }
  this->node_write_burst_ = 0;
  bool verifying = false;
  { std::lock_guard<std::mutex> lock(this->node_mutex_);
    for (size_t i = 0; i < this->catalog_.count; ++i) verifying |= this->node_states_[i].verify_fields != 0;
  }
  if (verifying) { this->poll_nodes_(now); return; }
  if (this->next_diagnostic_request_(now, request)) {
    this->send_node_request_(request); this->node_next_request_at_ = now + 200; return;
  }
  this->poll_nodes_(now);
}

void SteinelMesh::poll_nodes_(uint32_t now) {
  if (protocol::deadline_pending(now, this->node_poll_at_)) return;
  bool discovered = true;
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    size_t fields = 0;
    for (size_t i = 0; i < this->catalog_.count; ++i) {
      if (!this->catalog_.nodes[i].selected) continue;
      const auto &state = this->node_states_[i];
      for (size_t f = 0; f < state.value_seen.size(); ++f)
        if (state.value_seen[f] || (state.supported & (1U << f))) ++fields;
    }
    const uint32_t stale_after = protocol::stale_interval(fields);
    for (auto &state : this->node_states_) state.stale_after = stale_after;
    for (size_t i = 0; i < this->catalog_.count; ++i)
      if (this->catalog_.nodes[i].selected && this->node_states_[i].supported != 0 &&
          !this->node_states_[i].discovery_complete) discovered = false;
  }
  bool diagnosing;
  { std::lock_guard<std::mutex> lock(this->node_mutex_); now = millis(); diagnosing = this->device_diagnostics_.active(now); }
  if (!diagnosing && discovered && this->sync_node_functions_() && this->reboot_pending_.load()) return;
  if (this->catalog_.count == 0) return;
  const auto dispatch = [&](const NodeRequest &request) {
    {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      now = millis();
      auto &state = this->node_states_[request.node];
      const auto &node = this->catalog_.nodes[request.node];
      if (request.kind >= ON && request.kind <= RUN) state.value_poll_at[request.kind - ON] = now;
      if (request.kind == COMPOSITION)
        state.composition_at = now + (state.composition_checked ? IDENTITY_INTERVAL : 60000);
      if (request.kind == SENSOR || (request.kind == SENSOR_PROPERTY && request.value == 0x004E)) {
        const int fallback = protocol::element_with(node, protocol::SENSOR);
        if (request.element == (state.lux_element < node.element_count ? state.lux_element : fallback))
          state.value_poll_at[5] = now;
        if (request.kind == SENSOR &&
            request.element == (state.motion_element < node.element_count ? state.motion_element : fallback))
          state.value_poll_at[6] = now;
      }
    }
    this->send_node_request_(request);
    this->node_next_request_at_ = now + 200;
  };
  NodeRequest verification{};
  bool verification_found = false;
  if (this->node_priority_burst_ < 8) {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    for (size_t i = 0; i < this->catalog_.count && !verification_found; ++i) {
      const size_t index = (this->node_poll_index_ + i) % this->catalog_.count;
      const auto &node = this->catalog_.nodes[index];
      auto &state = this->node_states_[index];
      if (!node.selected || state.mode.writing ||
          !protocol::composition_allows(state.composition_checked, state.composition_matches)) continue;
      for (size_t offset = 0; offset < 5; ++offset) {
        const uint8_t field = (state.verify_next + offset) % 5;
        if (!(state.verify_fields & (1U << field))) continue;
        const uint8_t kind = ON + field;
        const int element = field == 0 ? protocol::output_element(node) : protocol::element_with(node, required_cap(kind));
        state.verify_fields &= ~(1U << field); state.verify_next = (field + 1) % 5;
        if (!(protocol::possible_functions(node) & (1U << field))) continue;
        if (element < 0) continue;
        verification = NodeRequest{uint8_t(index), uint8_t(element), kind, 0, 0, 0};
        verification.verification = true; verification_found = true;
        this->node_poll_index_ = (index + 1) % this->catalog_.count;
        break;
      }
    }
  }
  if (verification_found) { ++this->node_priority_burst_; dispatch(verification); return; }
  // Refresh the oldest known value before background probes can make it expire.
  NodeRequest priority{};
  bool priority_found = false, optional_timeout = false;
  uint32_t oldest = STALE_INTERVAL - POLL_INTERVAL;
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    now = millis();
    for (size_t i = 0; i < this->catalog_.count; ++i) {
      const auto &state = this->node_states_[i];
      if (!this->catalog_.nodes[i].selected || !protocol::composition_allows(state.composition_checked, state.composition_matches) ||
          protocol::deadline_pending(now, state.retry_at)) continue;
      for (size_t field : {size_t(3), size_t(4)})
        if (state.value_seen[field] != 0 && state.value_poll_at[field] != 0 &&
            static_cast<int32_t>(state.value_poll_at[field] - state.value_seen[field]) > 0) optional_timeout = true;
    }
    // Leave more time for working values when previously supported properties time out.
    if (optional_timeout) oldest = STALE_INTERVAL - 2 * POLL_INTERVAL;
    for (size_t i = 0; i < this->catalog_.count; ++i) {
      const auto &node = this->catalog_.nodes[i];
      const auto &state = this->node_states_[i];
      if (!node.selected || !protocol::composition_allows(state.composition_checked, state.composition_matches) ||
          protocol::deadline_pending(now, state.retry_at)) continue;
      for (size_t field = 0; field < state.value_seen.size(); ++field) {
        if (!(protocol::possible_functions(node) & (1U << field))) continue;
        if ((state.value_seen[field] == 0 && !(state.supported & (1U << field))) ||
            (state.value_poll_at[field] != 0 && now - state.value_poll_at[field] < POLL_INTERVAL)) continue;
        if ((field == 3 || field == 4) && state.value_poll_at[field] != 0 &&
            static_cast<int32_t>(state.value_poll_at[field] - state.value_seen[field]) > 0 &&
            now - state.value_poll_at[field] <
                ((state.supported & (1U << field)) ? 2 * POLL_INTERVAL : STALE_INTERVAL)) continue;
        uint32_t refreshed_at = state.value_seen[field] ? state.value_seen[field] : state.value_poll_at[field];
        if ((field == 3 || field == 4) && state.value_poll_at[field] != 0 &&
            static_cast<int32_t>(state.value_poll_at[field] - refreshed_at) > 0)
          refreshed_at = state.value_poll_at[field];
        const uint32_t elapsed = now - refreshed_at;
        const uint32_t age = optional_timeout && (field == 3 || field == 4) ?
            elapsed - std::min<uint32_t>(elapsed, POLL_INTERVAL) : elapsed;
        if (age < oldest) continue;
        const uint8_t kind = field < 5 ? uint8_t(ON + field) :
            field == 5 && state.lux_property ? uint8_t(SENSOR_PROPERTY) : uint8_t(SENSOR);
        int element = field == 0 ? protocol::output_element(node) : protocol::element_with(node, required_cap(kind));
        if (field == 5 && state.lux_element < node.element_count) element = state.lux_element;
        if (field == 6 && state.motion_element < node.element_count) element = state.motion_element;
        if (element < 0) continue;
        priority = NodeRequest{uint8_t(i), uint8_t(element), kind, 0, 0,
                               kind == SENSOR_PROPERTY ? 0x004EU : 0U};
        priority_found = true; oldest = age;
      }
    }
  }
  if (priority_found && this->node_priority_burst_ < 8) {
    ++this->node_priority_burst_; dispatch(priority); return;
  }
  for (size_t attempts = 0; attempts < protocol::MAX_NODES * POLL_STEPS; ++attempts) {
    const uint8_t index = this->node_poll_index_++ % this->catalog_.count;
    this->node_poll_index_ %= this->catalog_.count;
    const auto &node = this->catalog_.nodes[index];
    if (!node.selected) continue;
    uint8_t step;
    bool composition_checked, composition_matches, composition_due;
    {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      now = millis();
      auto &state = this->node_states_[index];
      if (protocol::deadline_pending(now, state.retry_at) || protocol::deadline_pending(now, state.poll_at)) continue;
      composition_checked = state.composition_checked; composition_matches = state.composition_matches;
      composition_due = !protocol::deadline_pending(now, state.composition_at);
      if (state.poll_step == 0) state.metadata_sent = false;
      step = state.poll_step++;
      if (state.poll_step >= POLL_STEPS) {
        state.poll_step = 0; state.poll_at = now + POLL_INTERVAL;
        state.discovery_complete = true; state.force_poll = false;
      }
    }
    uint8_t kind = step;
    uint32_t value = 0;
    if (kind >= ON && kind <= RUN &&
        !(protocol::possible_functions(node) & (1U << (kind - ON)))) continue;
    if (kind == COMPOSITION && composition_checked) {
      if (!composition_due) continue;
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      std::fill(std::begin(this->node_states_[index].sensor_probes),
                std::end(this->node_states_[index].sensor_probes), 0);
    }
    if (kind == COMPOSITION && !composition_due) continue;
    if (kind != COMPOSITION && !protocol::composition_allows(composition_checked, composition_matches)) continue;
    int element;
    if (step >= 6) {
      element = (step - 6) / 5;
      const uint8_t probe = (step - 6) % 5;
      if (element >= node.element_count || !(node.elements[element].capabilities & protocol::SENSOR)) continue;
      kind = probe == 0 ? SENSOR : probe == 2 ? DESCRIPTORS : SENSOR_PROPERTY;
      value = probe == 1 ? 0x004E : probe == 3 ? 0x000E : probe == 4 ? 0x0010 : 0;
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      now = millis();
      auto &state = this->node_states_[index];
      const bool lux_fresh = !(state.supported & protocol::F_LUX) ||
          (state.value_seen[5] != 0 && now - state.value_seen[5] < 45000);
      const bool motion_fresh = !(state.supported & protocol::F_MOTION) ||
          (state.value_seen[6] != 0 && now - state.value_seen[6] < 45000);
      if (probe == 0 && !state.force_poll && lux_fresh && motion_fresh &&
          state.sensor_push_at[element] != 0 && now - state.sensor_push_at[element] < 45000) continue;
      // One optional probe per node/cycle; five attempts per property/element.
      const bool bounded = probe == 2 || probe >= 3;
      if (bounded) {
        if (std::any_of(state.value_seen.begin(), state.value_seen.end(), [&](uint32_t seen) {
              return seen != 0 && now - seen >= state.stale_after - POLL_INTERVAL;
            })) continue;
        auto &attempts = state.sensor_probes[element];
        const uint8_t expected = attempts % 3 == 0 ? 2 : attempts % 3 == 1 ? 3 : 4;
        if (state.metadata_sent || attempts >= 15 || probe != expected) continue;
        ++attempts;
        if (attempts > 3 && ((probe == 3 && state.firmware[0]) || (probe == 4 && state.hardware[0]))) continue;
        state.metadata_sent = true;
      }
      if (probe == 1) {
        if (state.value_seen[5] != 0 && now - state.value_seen[5] < 45000) continue;
        if (!(state.lux_property && state.lux_element == element)) {
          auto &attempts = state.lux_probes[element];
          if (state.metadata_sent || attempts >= 5 ||
              std::any_of(state.value_seen.begin(), state.value_seen.end(), [&](uint32_t seen) {
                return seen != 0 && now - seen >= state.stale_after - POLL_INTERVAL;
              })) continue;
          ++attempts; state.metadata_sent = true;
        }
      }
    } else element = kind == COMPOSITION ? 0 : kind == ON ? protocol::output_element(node) :
                    protocol::element_with(node, required_cap(kind));
    if (element < 0) continue;
    if (kind == THRESHOLD || kind == RUN) {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      now = millis();
      auto &state = this->node_states_[index];
      const size_t field = kind - ON;
      if ((state.supported & (1U << field)) && state.value_poll_at[field] != 0 &&
          static_cast<int32_t>(state.value_poll_at[field] - state.value_seen[field]) > 0 &&
          now - state.value_poll_at[field] < 2 * POLL_INTERVAL) continue;
      if (!(state.supported & (1U << field)) && state.value_seen[field] == 0) {
        auto &attempts = state.lc_probes[kind - THRESHOLD];
        // Five discovery attempts, then one attempt per ten passes.
        if (attempts >= 5 && attempts < 14) { ++attempts; continue; }
        if (state.metadata_sent) continue;
        attempts = attempts < 5 ? attempts + 1 : 5;
        state.metadata_sent = true;
      }
    }
    this->node_priority_burst_ = 0;
    dispatch(NodeRequest{index, uint8_t(element), kind, 0, 0, value}); return;
  }
  if (priority_found) { this->node_priority_burst_ = 0; dispatch(priority); return; }
  this->node_poll_at_ = now + 20;
}
}  // namespace esphome::steinel_mesh
