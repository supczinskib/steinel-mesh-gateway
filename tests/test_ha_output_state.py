"""Exercise HA's captured Output guard against production entity publication.

Transport faults are declared independently of the reporter's timings. These
checks cover false HA state and skipped actions, without modeling radio delays.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from test_l840_installation import ROOT, build_code


MAIN = r'''
int main(int argc, char **argv) {
  using namespace installation;
  using Command = SteinelMesh::NodeCommand;
  assert(argc == 2);
  const std::string directory = argv[1];
  unsigned failures = 0;
  auto check = [&](bool passed, const char *description) {
    std::printf("%s: %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) ++failures;
  };
  auto action = [](Network &n, bool on) {
    for (unsigned i : ACTION_NODES) n.command(i, Command::ONOFF, on);
  };
  auto condition = [](const Network &n) {
    const auto &entity = n.entities.output[GUARD_NODE];
    return entity.known && entity.option == GUARD_STATE;
  };
  {
    transport_fault = 1;
    Network n({Input::SCRIPTED, 0, 1, 0, -1});
    bool eligible = false;
    n.schedule(BASE + 10000, [&] { action(n, true); });
    n.schedule(BASE + 20000, [&] {
      eligible = condition(n);
      transport_fault = 0;
      if (eligible) action(n, true);
    });
    n.run(35000);
    check(eligible, "fresh Off replies allow the captured HA condition to retry a lost On");
    for (unsigned i = 0; i < 3; ++i) {
      check(n.physical_on[i] != UNSEEN && n.physical_on[i] >= 20000,
            "retry reaches a lamp after the injected write outage ends");
      check(n.entities.output[i].known && n.entities.output[i].option == "On" &&
            n.gateway.node_states_[i].on == 1,
            "On is published after a device status confirms it");
    }
    std::ofstream(directory + "/lost-on-and-retry.csv") << n.trace.str();
  }
  {
    transport_fault = 1;
    Network n({Input::RESTORE_AUTO, 0, 1, 0, -1});
    n.scenario.input = Input::SCRIPTED;
    bool incorrectly_eligible = false;
    n.schedule(BASE + 10000, [&] { action(n, false); });
    n.schedule(BASE + 20000, [&] { incorrectly_eligible = condition(n); });
    n.run(25000);
    check(!incorrectly_eligible, "a lost Off cannot masquerade as Off in the HA guard");
    for (unsigned i = 0; i < 3; ++i)
      check(n.lamps[i].on() && n.gateway.node_states_[i].on == 1 &&
            n.entities.output[i].known && n.entities.output[i].option == "On",
            "fresh On replies remain visible while an Off request is pending");
    std::ofstream(directory + "/lost-off.csv") << n.trace.str();
  }
  {
    transport_fault = 0;
    Network n({Input::SCRIPTED, 0, 1, 0, -1});
    n.schedule(BASE + 10000, [&] { action(n, true); });
    n.run(25000);
    check(!condition(n), "confirmed On prevents an unnecessary repeat in the HA condition");
    for (unsigned i = 0; i < 3; ++i)
      check(n.ha_on[i] != UNSEEN && n.confirmed_on[i] != UNSEEN &&
            n.ha_on[i] >= n.confirmed_on[i] && n.ha_on[i] >= n.physical_on[i],
            "healthy HA On follows confirmed output rather than queue admission");
    std::ofstream(directory + "/healthy.csv") << n.trace.str();
  }
  {
    transport_fault = 2;
    Network n({Input::SCRIPTED, 0, 1, 0, -1});
    n.schedule(BASE + 10000, [&] { action(n, true); });
    n.run(25000);
    for (unsigned i = 0; i < 3; ++i)
      check(n.physical_on[i] != UNSEEN && n.confirmed_on[i] == UNSEEN &&
            n.ha_on[i] == UNSEEN,
            "missing device replies do not fabricate HA On from a submitted command");
    std::ofstream(directory + "/missing-replies.csv") << n.trace.str();
  }
  {
    SteinelMesh g;
    NativeEntities entities;
    test_now = BASE;
    initialize_network(g, entities, 0);
    g.composition_query_in_flight_ = true;
    auto &state = g.node_states_[GUARD_NODE];
    const auto address = g.catalog_.nodes[GUARD_NODE].address;
    state.on = -1;
    state.value_seen[0] = 0;
    state.dirty = true;
    g.advance_nodes_(test_now);
    check(!entities.output[GUARD_NODE].known, "unknown output is not defaulted to Off");
    check(g.queue_node_command(address, Command::ONOFF, 1), "selected online node admits On");
    g.advance_nodes_(++test_now);
    check(!entities.output[GUARD_NODE].known,
          "a request cannot turn an unknown measured output into a known HA state");
    esp_ble_mesh_client_common_param_t p{};
    p.ctx.addr = address;
    p.ctx.recv_op = 0x8204;
    p.opcode = 0x8201;
    deliver(g, p, true, {0, 1, 0x32});
    g.advance_nodes_(++test_now);
    check(entities.output[GUARD_NODE].known && entities.output[GUARD_NODE].option == "Off",
          "transition target On does not override present Off");
    deliver(g, p, true, {1});
    g.advance_nodes_(++test_now);
    check(entities.output[GUARD_NODE].option == "On", "present On updates HA");
    state.last_seen = test_now + state.stale_after;
    test_now = state.last_seen;
    g.advance_nodes_(test_now);
    check(!entities.output[GUARD_NODE].known,
          "unrelated traffic cannot keep an expired output state known");
  }
  std::printf("HA Output contract: %u failure(s)\n", failures);
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    fixture = json.loads((ROOT / 'tests/fixtures/l840/rc6/ha-output-condition.json').read_text())
    condition, action = fixture['condition'], fixture['action']
    assert condition['condition'] == 'state' and condition['state'] == ['Off']
    assert action['action'] == 'select.select_option' and action['data']['option'] == 'On'
    # Indices follow the three captured Composition Data fixtures (SC, C, SC).
    node_ids = ['select.l_840_sc_181b_output', 'select.l_840_c_e16dfc_output',
                'select.l_840_sc_aca3a1_output']
    main_code = MAIN.replace('ACTION_NODES', '{' + ','.join(
        f'{node_ids.index(entity)}U' for entity in action['target']['entity_id']) + '}')
    main_code = main_code.replace('GUARD_NODE', str(node_ids.index(condition['entity_id'])))
    main_code = main_code.replace('GUARD_STATE', json.dumps(condition['state'][0]))
    code = build_code(source_dir=args.source_dir).rsplit('\nint main(int argc,char **argv) {', 1)[0]
    code = code.replace('class Network {', 'unsigned transport_fault = 0;\nclass Network {', 1)
    marker = 'bool drop(unsigned node,unsigned direction,uint32_t opcode) {'
    assert code.count(marker) == 1
    code = code.replace(marker, marker + '''
    if (transport_fault == 1 && direction == 0 &&
        (opcode == 0x8203 || opcode == 0x8293)) return true;
    if (transport_fault == 2 && direction == 1) return true;
''', 1)
    code = code.replace('const bool lost=scenario.lose_downlink&&drop(i,0,p.opcode);',
                        'const bool lost=drop(i,0,p.opcode);', 1)
    code += main_code
    with tempfile.TemporaryDirectory(prefix='steinel-ha-output-') as directory:
        output = args.output or Path(directory)
        output.mkdir(parents=True, exist_ok=True)
        if args.output:
            (output / 'compiled.cpp').write_text(code)
        binary = str(Path(directory) / 'ha-output')
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-parameter', '-Wno-unused-const-variable',
                        '-fsanitize=address,undefined', '-I', str(ROOT / 'tests'), '-I',
                        str(args.source_dir or ROOT / 'esphome/components/steinel_mesh'),
                        '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
        result = subprocess.run([binary, str(output)], capture_output=True, text=True)
        print(result.stdout + result.stderr, end='')
        if args.output:
            (output / 'run.log').write_text(result.stdout + result.stderr)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
