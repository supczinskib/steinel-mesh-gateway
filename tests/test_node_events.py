"""Exercise the production Sensor callback with a small SDK boundary double."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "esphome/components/steinel_mesh/steinel_nodes.cpp").read_text()
START = SOURCE.index("bool SteinelMesh::node_sensor_event_(")
END = SOURCE.index("bool SteinelMesh::node_scene_event_(", START)
REQUEST_ENUM = SOURCE[SOURCE.index("enum Request :"):SOURCE.index("constexpr uint32_t POLL_INTERVAL")]
HARNESS = r'''
#include "mesh_protocol.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
namespace esphome::steinel_mesh {
uint32_t millis() { return 1000; }
constexpr int ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT = 3;
constexpr int ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT = 4;
constexpr uint32_t ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS = 0x51;
constexpr uint32_t ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS = 0x52;
using esp_ble_mesh_sensor_client_cb_event_t = int;
struct Buffer { uint8_t *data; uint16_t len; };
struct Params { struct { uint32_t recv_op; uint16_t addr; } ctx; };
struct esp_ble_mesh_sensor_client_cb_param_t {
  Params *params; int error_code{0};
  struct {
    struct { Buffer *descriptor; } descriptor_status;
    struct { Buffer *marshalled_sensor_data; } sensor_status;
  } status_cb;
};
class SteinelMesh {
 public:
  protocol::DeviceDiagnostics device_diagnostics_{};
  template<class P>bool diagnostic_reply_(const P*,uint32_t,bool,uint16_t=0){return false;}
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0){}
  struct State {
    bool composition_checked{true},composition_matches{true}, dirty{false};
    protocol::SensorReadings sensors{};
    bool lux_property{false};
    uint16_t supported{0}; uint32_t lux{protocol::UNKNOWN_24}, last_seen{0}, retry_at{0};
    uint8_t failures{0};
    uint8_t lux_element{0xFF}, motion_element{0xFF};
    int8_t motion{-1}; std::array<uint32_t, 7> value_seen{};
    char firmware[17]{}, hardware[17]{};
    std::array<uint32_t,protocol::MAX_ELEMENTS> sensor_push_at{};
  };
  bool catalog_valid_{true}, last_success{false}; unsigned events{0};
  enum class AccessOperation { NONE, NODE };
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  struct Request {uint8_t kind{6};uint32_t value{0};} active_node_request_;
  std::atomic<uint16_t> node_destination_{0};
  std::atomic<uint16_t> node_expected_property_{0};
  protocol::Catalog catalog_;
  std::array<State, protocol::MAX_NODES> node_states_{};
  std::mutex node_mutex_;
  std::atomic<unsigned> mesh_rx_messages_{0}, mesh_sensor_rx_{0};
  template<class Context> void record_mesh_rssi_(const Context &) {}
  bool node_event_(Params *, uint32_t, bool success, uint16_t=0) { last_success = success; ++events; return true; }
  bool node_sensor_event_(int, esp_ble_mesh_sensor_client_cb_param_t *);
};
'''
MAIN = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  SteinelMesh gateway;
  gateway.catalog_.count = 2;
  for (size_t i = 0; i < 2; ++i) {
    auto &node = gateway.catalog_.nodes[i];
    node.address = 2 + i * 4; node.element_count = 4; node.selected = true;
    node.elements[2].capabilities = node.elements[3].capabilities = protocol::SENSOR;
  }
  uint8_t lux[] = {0xC4, 0x09, 0xC4, 0x09, 0}; // 25.00 lx, Format A
  Buffer buffer{lux, sizeof(lux)};
  Params params{{ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS, 5}}; // fourth element
  esp_ble_mesh_sensor_client_cb_param_t response{};
  response.params = &params; response.status_cb.sensor_status.marshalled_sensor_data = &buffer;
  assert(gateway.node_sensor_event_(0, &response) && gateway.last_success);
  assert(gateway.node_states_[0].lux == 2500 && gateway.node_states_[0].supported == protocol::F_LUX);
  gateway.access_operation_=SteinelMesh::AccessOperation::NODE;
  gateway.active_node_request_={SENSOR_PROPERTY,0x004E};gateway.node_destination_=5;
  gateway.node_expected_property_=0x004E;
  assert(gateway.node_sensor_event_(0,&response)&&gateway.node_states_[0].lux_property);
  gateway.access_operation_=SteinelMesh::AccessOperation::NONE;
  assert(gateway.node_states_[0].lux_element == 3 && gateway.node_states_[0].motion_element == 0xFF);
  assert(gateway.node_states_[0].sensor_push_at[3] == 0);
  gateway.node_states_[0].retry_at = 60000; gateway.node_states_[0].failures = 4;
  assert(gateway.node_sensor_event_(ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT, &response));
  assert(!gateway.node_states_[0].lux_property);
  assert(gateway.node_states_[0].sensor_push_at[3] == 1000);
  assert(gateway.node_states_[0].retry_at == 0 && gateway.node_states_[0].failures == 0);
  assert(gateway.node_states_[1].supported == 0);
  // A second element of another device must not overwrite the first device.
  params.ctx.addr = 8; lux[2] = 0xE8; lux[3] = 3;
  assert(gateway.node_sensor_event_(0, &response));
  assert(gateway.node_states_[1].lux == 1000 && gateway.node_states_[0].lux == 2500);
  assert(gateway.node_states_[1].lux_element == 2 && gateway.node_states_[0].lux_element == 3);
  // A matching AppKey response works without Composition Data; a mismatch blocks it.
  gateway.node_states_[1].composition_checked = false;
  gateway.node_states_[1].composition_matches = false;
  assert(gateway.node_sensor_event_(0,&response)&&gateway.last_success);
  gateway.node_states_[1].composition_checked = true;
  gateway.node_states_[1].composition_matches = false;
  lux[2] = 1; lux[3] = 0;
  assert(gateway.node_sensor_event_(0, &response) && !gateway.last_success);
  assert(gateway.node_states_[1].lux == 1000);
  gateway.node_states_[1].composition_matches = true;
  assert(gateway.node_states_[0].sensors.count==1);
  assert(gateway.node_states_[0].sensors.values[0].element==3);
  assert(gateway.node_states_[1].sensors.values[0].element==2);
  // The same unknown property on two elements must remain two diagnostic readings.
  uint8_t unknown[]={3,0x23,0x81,0xAA,0xBB};
  buffer={unknown,sizeof(unknown)};params.ctx.addr=4;
  assert(gateway.node_sensor_event_(0,&response)&&gateway.last_success);
  params.ctx.addr=5;
  assert(gateway.node_sensor_event_(0,&response)&&gateway.last_success);
  assert(gateway.node_states_[0].sensors.count==3);
  assert(gateway.node_states_[0].sensors.values[1].property==0x8123);
  assert(gateway.node_states_[0].sensors.values[1].element==2);
  assert(gateway.node_states_[0].sensors.values[2].element==3);
  assert(gateway.node_states_[0].sensors.values[2].raw[1]==0xBB);
  buffer={lux,sizeof(lux)};params.ctx.addr=8;
  // Invalid/truncated MPID payload cannot partially update a state.
  buffer.len = 4;
  assert(gateway.node_sensor_event_(0, &response) && !gateway.last_success);
  assert(gateway.node_states_[1].lux == 1000);
  buffer.len = 5;
  assert(gateway.node_sensor_event_(ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT, &response) && !gateway.last_success);
  assert(gateway.node_states_[1].lux == 1000);
  // Descriptors establish functions, not made-up values.
  uint8_t descriptors[] = {0x42, 0, 0, 0, 0, 0, 0, 0};
  Buffer descriptor_buffer{descriptors, sizeof(descriptors)};
  response.status_cb.descriptor_status.descriptor = &descriptor_buffer;
  params.ctx.recv_op = ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS;
  assert(gateway.node_sensor_event_(0, &response) && gateway.last_success);
  assert(gateway.node_states_[1].supported & protocol::F_MOTION);
  assert(gateway.node_states_[1].motion == -1);
  uint8_t motion[] = {1,0x4D,0,1};
  buffer={motion,sizeof(motion)};params.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS;
  assert(gateway.node_sensor_event_(0,&response) && gateway.last_success);
  assert(gateway.node_states_[1].motion==1 && gateway.node_states_[1].motion_element==2);
  // Firmware Sensor property, Format B, bounded ASCII string.
  uint8_t firmware[] = {9, 0x0E, 0, '1', '.', '2', '.', '3'};
  buffer = {firmware, sizeof(firmware)};
  params.ctx.recv_op = ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS;
  assert(gateway.node_sensor_event_(0, &response) && gateway.last_success);
  assert(std::strcmp(gateway.node_states_[1].firmware, "1.2.3") == 0);
  // Deselected/non-Sensor addresses cannot inject state into selected nodes.
  gateway.catalog_.nodes[1].selected = false;
  assert(!gateway.node_sensor_event_(0, &response));
  params.ctx.addr = 2; assert(!gateway.node_sensor_event_(0, &response));
  std::puts("Actual Sensor callback: all elements, isolation, composition gating, malformed data and identity passed");
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-events-") as directory:
        binary = str(Path(directory) / "events")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=HARNESS + REQUEST_ENUM + SOURCE[START:END] + MAIN, text=True, check=True)
        subprocess.run([binary], check=True)
