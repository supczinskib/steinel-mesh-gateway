"""Exercise production command admission/coalescing without a radio stack."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "esphome/components/steinel_mesh/steinel_nodes.cpp").read_text()
START = SOURCE.index("bool SteinelMesh::queue_node_command(")
END = SOURCE.index("bool SteinelMesh::send_node_request_(", START)
COMPLETION_START = SOURCE.index("  if (this->node_completion_pending_.exchange(false))")
COMPLETION_END = SOURCE.index("  if (!this->mesh_ready_.load()", COMPLETION_START)
COMPLETION = "\nvoid SteinelMesh::test_completion(uint32_t now) {\n" + SOURCE[COMPLETION_START:COMPLETION_END] + "\n}\n"
REQUESTS = SOURCE[SOURCE.index("enum Request :"):SOURCE.index("template<class Base>")]
HARNESS = r'''
#include "mesh_protocol.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
namespace esphome::steinel_mesh {
uint32_t test_now = 1000;
uint32_t millis() { return test_now; }
class SteinelMesh {
 public:
  protocol::DeviceDiagnostics device_diagnostics_{};
  protocol::StateTrace state_trace_{};
  template<class P>bool diagnostic_reply_(const P*,uint32_t,bool,uint16_t=0){return false;}
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0){}
  enum class NodeCommand : uint8_t { ONOFF, BRIGHTNESS, AUTO, THRESHOLD, RUN_TIME, MODE };
  struct NodeRequest { uint8_t node, element, kind, attempts, tid; uint32_t value; uint8_t step{0}; bool diagnostic{false},verification{false}; };
  std::atomic<uint16_t> node_destination_{0};
  std::atomic<uint32_t> node_expected_status_{0};
  struct NodeState { bool composition_matches{true},composition_checked{true}; uint32_t last_seen{1000},stale_after{180000}; uint16_t supported{255}; uint8_t failures{0}; uint32_t retry_at{0},poll_at{0}; uint8_t poll_step{0}; bool discovery_complete{false},force_poll{false},dirty{false}; int8_t on{-1},automatic{-1}; protocol::ConfirmedMode mode{}; protocol::PendingControls controls{}; uint8_t verify_fields{0},verify_next{0}; std::array<uint8_t,5> verify_attempts{}; std::array<uint32_t,7> value_seen{}; };
  struct Config { uint16_t onoff_address{1}; } config_;
  static constexpr size_t NODE_QUEUE_SIZE = 16;
  bool catalog_valid_{true}, legacy_profile_{false};
  std::atomic<bool> mesh_ready_{true}, reboot_pending_{false};
  protocol::Catalog catalog_;
  std::array<NodeState, protocol::MAX_NODES> node_states_{};
  std::array<NodeRequest, NODE_QUEUE_SIZE> node_queue_{};
  size_t node_queue_count_{0};
  uint8_t tid_{0}, node_poll_index_{0}, node_poll_step_{0};
  uint8_t node_priority_burst_{0};
  uint8_t node_write_burst_{0};
  uint32_t node_next_request_at_{0};
  std::atomic<bool> node_completion_pending_{false}, node_completion_success_{false};
  NodeRequest active_node_request_{};
  std::mutex node_mutex_;
  uint8_t next_tid_() { return tid_++; }
  bool queue_node_command(uint16_t, NodeCommand, uint32_t);
  void test_completion(uint32_t now);
};
'''
MAIN = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  using Command = SteinelMesh::NodeCommand;
  SteinelMesh gateway;
  gateway.catalog_.count = 4;
  for (size_t i = 0; i < 4; ++i) {
    auto &node = gateway.catalog_.nodes[i];
    node.address = i * 2 + 1; node.element_count = 1; node.selected = true;
    node.elements[0].capabilities = protocol::ONOFF | protocol::LIGHTNESS | protocol::LC | protocol::SENSOR;
  }
  assert(!gateway.queue_node_command(999, Command::ONOFF, 1));
  assert(!gateway.queue_node_command(1, Command(255), 1));
  assert(!gateway.queue_node_command(1, Command::ONOFF, 2));
  assert(!gateway.queue_node_command(1, Command::AUTO, 2));
  assert(!gateway.queue_node_command(1, Command::BRIGHTNESS, 65536));
  assert(!gateway.queue_node_command(1, Command::THRESHOLD, protocol::UNKNOWN_24));
  assert(!gateway.queue_node_command(1, Command::RUN_TIME, protocol::UNKNOWN_24));
  gateway.catalog_.nodes[0].nightmatiq=true;
  assert(!gateway.queue_node_command(1, Command::RUN_TIME, 1000));
  assert(gateway.node_queue_count_==0);
  gateway.catalog_.nodes[0].nightmatiq=false;
  assert(gateway.queue_node_command(1, Command::RUN_TIME, 1000));
  gateway.node_queue_count_=0;
  gateway.node_states_[0].composition_matches = false;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.node_states_[0].composition_matches = true;
  gateway.node_states_[0].composition_checked = false;
  gateway.node_states_[0].composition_matches = false;
  assert(gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.node_queue_count_ = 0;
  gateway.node_states_[0].composition_checked = gateway.node_states_[0].composition_matches = true;
  test_now = 181001;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  test_now = 1000;
  gateway.legacy_profile_ = true;
  assert(gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.node_queue_count_ = 0;
  gateway.legacy_profile_ = false; gateway.mesh_ready_ = false;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.mesh_ready_ = true; gateway.reboot_pending_ = true;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.reboot_pending_ = false; gateway.catalog_.nodes[0].selected = false;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.catalog_.nodes[0].selected = true;
  gateway.catalog_.nodes[0].elements[0].capabilities &= ~protocol::ONOFF;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.catalog_.nodes[0].elements[0].capabilities |= protocol::ONOFF;
  assert(gateway.queue_node_command(1, Command::BRIGHTNESS, 100));
  const auto original_tid = gateway.node_queue_[0].tid;
  gateway.node_queue_[0].attempts = 1; // transmitted retry waiting in queue
  assert(gateway.queue_node_command(1, Command::BRIGHTNESS, 200));
  assert(gateway.node_queue_count_ == 1 && gateway.node_queue_[0].value == 200);
  assert(gateway.node_queue_[0].tid != original_tid && gateway.node_queue_[0].attempts == 0);
  for (uint32_t value = 0; value < 65536; ++value)
    assert(gateway.queue_node_command(1, Command::BRIGHTNESS, value));
  assert(gateway.node_queue_count_ == 1 && gateway.node_queue_[0].value == 65535);
  gateway.node_queue_count_ = 0;
  for (unsigned i = 0; i < 15; ++i)
    assert(gateway.queue_node_command((i / 5) * 2 + 1, Command(i % 5), 1));
  assert(gateway.node_queue_count_ == 15);
  assert(!gateway.queue_node_command(7, Command::ONOFF, 1));
  assert(gateway.queue_node_command(1, Command::BRIGHTNESS, 200));
  assert(gateway.node_queue_count_ == 15);
  gateway.node_queue_count_ = 0;
  gateway.node_states_[0].supported = 0;
  assert(!gateway.queue_node_command(1, Command::ONOFF, 1));
  gateway.node_states_[0].supported = 255;
  assert(!gateway.queue_node_command(1, Command::MODE, 0));
  gateway.catalog_.nodes[0].nightmatiq = true;
  gateway.catalog_.nodes[0].elements[0].capabilities |= protocol::SCENE;
  assert(!gateway.queue_node_command(1, Command::MODE, 3));
  assert(gateway.queue_node_command(1, Command::MODE, 0));
  gateway.node_queue_[0].step = 3;
  assert(gateway.queue_node_command(1, Command::MODE, 1));
  assert(gateway.node_queue_[0].step == 0 && gateway.node_queue_[0].value == 1);
  // Test the actual production completion block: continuation is priority,
  // bounded retry reuses TID, four stages terminate, newer intent supersedes.
  gateway.node_queue_count_ = 0;
  for (uint8_t step = 0; step < 4; ++step) {
    gateway.active_node_request_ = {0, 0, SET_MODE, 0, 42, 0, step};
    gateway.node_completion_pending_ = true; gateway.node_completion_success_ = true;
    gateway.test_completion(test_now);
    assert(gateway.node_queue_count_ == (step < 3 ? 1 : 0));
    if (step < 3) {
      assert(gateway.node_queue_[0].step == step + 1 && gateway.node_queue_[0].tid == 42);
      gateway.node_queue_count_ = 0;
    }
  }
  // Fill admission capacity; the reserved final slot still fits a retry.
  for (unsigned i = 0; i < 15; ++i)
    assert(gateway.queue_node_command((i / 5) * 2 + 3, Command(i % 5), 1));
  gateway.active_node_request_ = {0, 0, SET_MODE, 0, 42, 0, 1};
  gateway.node_completion_pending_ = true; gateway.node_completion_success_ = false;
  gateway.test_completion(test_now);
  assert(gateway.node_queue_count_ == 16 && gateway.node_queue_[0].kind == SET_MODE);
  assert(gateway.node_queue_[0].tid == 42 && gateway.node_queue_[0].attempts == 1);
  gateway.node_queue_count_ = 0;
  gateway.active_node_request_.attempts = 1;
  gateway.node_completion_pending_ = true; gateway.test_completion(test_now);
  assert(gateway.node_queue_count_ == 0); // no endless retries
  assert(gateway.queue_node_command(1, Command::MODE, 2));
  gateway.active_node_request_.attempts = 0;
  gateway.node_completion_success_ = true; gateway.node_completion_pending_ = true;
  gateway.test_completion(test_now);
  assert(gateway.node_queue_count_ == 1 && gateway.node_queue_[0].value == 2 && gateway.node_queue_[0].step == 0);
  // Manual writes disable LC before setting the output.
  gateway.node_queue_count_ = 0;
  assert(gateway.queue_node_command(1, Command::ONOFF, 1));
  assert(gateway.node_queue_[0].kind == SET_ON && gateway.node_queue_[0].step == 0);
  gateway.active_node_request_ = gateway.node_queue_[0]; gateway.node_queue_count_ = 0;
  gateway.node_completion_pending_ = true; gateway.node_completion_success_ = true;
  gateway.test_completion(test_now);
  assert(gateway.node_queue_count_ == 1 && gateway.node_queue_[0].step == 1);
  gateway.active_node_request_ = gateway.node_queue_[0]; gateway.node_queue_count_ = 0;
  gateway.node_completion_pending_ = true; gateway.test_completion(test_now);
  assert(gateway.node_queue_count_ == 0 && gateway.node_states_[0].force_poll);
  gateway.catalog_.nodes[0].elements[0].capabilities &= ~protocol::LC;
  assert(!gateway.queue_node_command(1, Command::BRIGHTNESS, 100));
  gateway.catalog_.nodes[0].nightmatiq = false;
  assert(gateway.queue_node_command(1, Command::BRIGHTNESS, 100));
  assert(gateway.node_queue_[0].step == 1);
  std::puts("Actual command queue: validation, stale/function gating, bounds and coalesced TIDs passed");
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix="steinel-queue-") as directory:
        binary = str(Path(directory) / "queue")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-fsanitize=address,undefined",
                        "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=HARNESS + REQUESTS + SOURCE[START:END] + COMPLETION + MAIN, text=True, check=True)
        subprocess.run([binary], check=True)


if __name__ == "__main__":
    run()
