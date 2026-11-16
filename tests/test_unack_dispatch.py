"""Verify command dispatch and read ownership while a GET is pending."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from test_l840_installation import ROOT, build_code

MAIN = r'''
int main() {
  using namespace installation;
  unsigned failures = 0;
  auto check = [&](bool ok, const char *label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
  };
  {
    test_now = BASE + 14789;
    SteinelMesh g; NativeEntities entities; initialize_network(g, entities, 0);
    for (auto &s : g.node_states_) s.poll_at = test_now + 30000;
    check(g.send_node_request_({0,1,RUN,0,0,0}), "start a 1200ms GET before commands arrive");
    const auto destination = g.node_destination_.load(), property = g.node_expected_property_.load();
    const auto opcode = g.access_opcode_.load(), status = g.node_expected_status_.load();
    const uint32_t received = BASE + 15452, timeout = BASE + 15997;
    test_now = received;
    check(g.queue_node_command(g.catalog_.nodes[1].address, SteinelMesh::NodeCommand::ONOFF, 1),
          "queue On while the earlier GET is unanswered");
    uint32_t first = UNSEEN;
    unsigned sends = wire_sends;
    for (; test_now < timeout; test_now += 10) {
      g.advance_nodes_(test_now);
      if (wire_sends != sends) {
        if (first == UNSEEN) first = test_now;
        sends = wire_sends;
      }
    }
    check(first != UNSEEN && first < timeout, "dispatch before the unrelated GET times out");
    check(g.access_operation_.load() == SteinelMesh::AccessOperation::NODE &&
          g.active_node_request_.kind == RUN && g.active_node_request_.node == 0 &&
          g.node_destination_.load() == destination && g.node_expected_property_.load() == property &&
          g.access_opcode_.load() == opcode && g.node_expected_status_.load() == status,
          "UNACK progression preserves the original GET context");
    if (first != UNSEEN) std::printf("First SDK dispatch after receipt: %ums; GET still pending\n", first - received);
  }
  for (uint8_t kind : {uint8_t(ON),uint8_t(AUTO),uint8_t(THRESHOLD),uint8_t(RUN),uint8_t(SENSOR)}) {
    test_now = BASE + 1000;
    SteinelMesh g; NativeEntities entities; initialize_network(g, entities, 0);
    const uint8_t element = kind == ON ? 0 : kind == SENSOR ? 2 : 1;
    check(g.send_node_request_({0,element,kind,0,0,0}), "admit the single acknowledged read");
    const auto before = g.active_node_request_;
    const auto destination = g.node_destination_.load(), property = g.node_expected_property_.load();
    const auto opcode = g.access_opcode_.load(), status = g.node_expected_status_.load();
    SteinelMesh::NodeRequest write{1,0,SET_ON,0,43,1}; write.step = 1;
    ++test_now;
    const bool accepted = g.send_node_request_(write);
    check(accepted, "SDK accepts UNACK during a pending read");
    check(g.active_node_request_.kind == before.kind && g.active_node_request_.node == before.node &&
          g.access_operation_.load() == SteinelMesh::AccessOperation::NODE &&
          g.node_destination_.load() == destination && g.node_expected_property_.load() == property &&
          g.access_opcode_.load() == opcode && g.node_expected_status_.load() == status,
          "write cannot overwrite the read destination, opcode, property or lifetime");
    check(!g.node_completion_pending_.load(), "UNACK completion cannot masquerade as GET completion");
    if (accepted) {
      esp_ble_mesh_client_common_param_t p{}; p.ctx.addr = destination; p.opcode = opcode;
      check(g.node_event_(&p, 0, false), "the original GET timeout still completes its own request");
      g.advance_nodes_(++test_now);
      check(g.node_states_[0].on == 0 && g.node_states_[1].on == 0,
            "SDK acceptance and GET timeout do not invent physical output");
    }
  }
  {
    test_now = BASE + 1000;
    SteinelMesh g; NativeEntities entities; initialize_network(g, entities, 0);
    check(g.send_node_request_({0,0,ON,0,0,0}), "start same-device output read");
    g.state_trace_.start(test_now, 17);
    auto read = wire_request;
    check(g.queue_node_command(g.catalog_.nodes[0].address, SteinelMesh::NodeCommand::ONOFF, 1),
          "register a newer output intent");
    SteinelMesh::NodeRequest write{0,0,SET_ON,0,44,1}; write.step = 1;
    if (g.send_node_request_(write)) {
      const auto deadline = g.access_deadline_.load();
      esp_ble_mesh_client_common_param_t failure{};
      failure.ctx.addr = g.catalog_.nodes[0].address;
      failure.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK;
      esp_ble_mesh_generic_client_cb_param_t error{}; error.params = &failure; error.error_code = -12;
      g.node_generic_event_(0, &error);
      check(!g.node_completion_pending_.load() && g.access_deadline_.load() == deadline,
            "asynchronous UNACK error cannot finish or retime the GET");
      read.ctx.recv_op = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS;
      esp_ble_mesh_generic_client_cb_param_t response{}; response.params = &read;
      response.status_cb.onoff_status = {false,1,0,0};
      ++test_now;
      g.node_generic_event_(0, &response);
      check(g.node_completion_pending_.load() && g.node_completion_success_.load(),
            "a pre-write GET reply still releases its acknowledged request");
      check(g.node_states_[0].on == 0,
            "a pre-write GET reply cannot confirm the newer On command");
      bool recorded = false;
      for (size_t i = 0; i < g.state_trace_.count; ++i) {
        const auto &e = g.state_trace_.entry(i);
        recorded |= e.event == protocol::StateTrace::RESPONSE &&
            (e.flags & 2);
      }
      check(recorded, "trace identifies a pre-write reply excluded from state confirmation");
      g.advance_nodes_(test_now);
      check(g.node_states_[0].on == 0,
            "read and write completions in one loop preserve confirmed state");
    } else check(false, "same-device read and write use separate completion state");
  }
  {
    test_now = BASE + 1000;
    SteinelMesh g; NativeEntities entities; initialize_network(g, entities, 0);
    check(g.send_node_request_({0,0,ON,0,0,0}), "start GET before checking dispatch spacing");
    const unsigned before = wire_sends;
    g.queue_node_command(g.catalog_.nodes[1].address, SteinelMesh::NodeCommand::ONOFF, 1);
    for (test_now = BASE + 1001; test_now < BASE + 1200; ++test_now) g.advance_nodes_(test_now);
    check(wire_sends == before, "preserve the 200ms send spacing during a pending GET");
    g.advance_nodes_(test_now);
    check(wire_sends == before + 1, "send when spacing expires without waiting for GET timeout");
  }
  {
    test_now = BASE + 1000;
    SteinelMesh g; NativeEntities entities; initialize_network(g, entities, 0);
    check(g.send_node_request_({0,0,COMPOSITION,0,0,0}), "start Composition Data read");
    const unsigned before = wire_sends;
    g.queue_node_command(g.catalog_.nodes[1].address, SteinelMesh::NodeCommand::ONOFF, 1);
    test_now += 500; g.advance_nodes_(test_now);
    check(wire_sends == before && g.active_node_request_.kind == COMPOSITION,
          "retain the configuration-read guard");
  }
  {
    test_now = BASE + 1000;
    SteinelMesh g; NativeEntities entities; initialize_network(g, entities, 0);
    g.node_states_[0].supported |= protocol::F_THRESHOLD;
    check(g.send_node_request_({0,1,SET_THRESHOLD,0,45,500}), "prepare a persistent LC property payload");
    check(g.send_node_request_({0,1,THRESHOLD,0,0,0}), "construct a property GET independently of its write");
    check(protocol::le24(g.node_property_buffer_.data) == 500,
          "GET construction cannot overwrite the SDK's retained property bytes");
    check(!g.send_node_request_({0,1,SET_THRESHOLD,0,46,600}) &&
          protocol::le24(g.node_property_buffer_.data) == 500,
          "one write owner protects payload bytes until its completion is handled");
  }
  std::printf("UNACK dispatch contract: %u failure(s)\n", failures);
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    code = build_code(source_dir=args.source_dir).rsplit('\nint main(int argc,char **argv) {', 1)[0] + MAIN
    with tempfile.TemporaryDirectory(prefix='steinel-unack-dispatch-') as directory:
        output = args.output or Path(directory)
        output.mkdir(parents=True, exist_ok=True)
        if args.output:
            (output / 'compiled.cpp').write_text(code)
        binary = str(Path(directory) / 'dispatch')
        subprocess.run([os.environ.get('CXX','c++'), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-const-variable', '-fsanitize=address,undefined',
                        '-I', str(ROOT / 'tests'), '-I', str(args.source_dir or ROOT / 'esphome/components/steinel_mesh'),
                        '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True)
        print(result.stdout + result.stderr, end='')
        if args.output:
            (output / 'run.log').write_text(result.stdout + result.stderr)
            (output / 'result.json').write_text(json.dumps({'passed':result.returncode == 0,
                'scope':'Production dispatch and callback ownership with a manually held GET; no physical radio/lamp timing claim.'},indent=2)+'\n')
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
