"""Packet replay and closed-loop tests using captured three-device Mesh traffic."""
from collections import defaultdict
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/steinel_mesh'
FIXTURES = ROOT / 'tests/fixtures/l840'
source = (COMPONENT / 'steinel_nodes.cpp').read_text()
mesh = (COMPONENT / 'steinel_mesh.cpp').read_text()
header = (COMPONENT / 'steinel_mesh.h').read_text()


def sensor_records(raw):
    data = bytes.fromhex(raw)
    offset = 0
    while offset < len(data):
        extended = data[offset] & 1
        size = 3 if extended else 2
        encoded = data[offset] >> 1 if extended else (data[offset] >> 1) & 15
        length = 0 if extended and encoded == 127 else encoded + 1
        prop = int.from_bytes(data[offset + 1:offset + 3], 'little') if extended else int.from_bytes(data[offset:offset + 2], 'little') >> 5
        value = data[offset + size:offset + size + length]
        assert len(value) == length
        yield prop, value
        offset += size + length
    assert offset == len(data)


def matches(request, response):
    statuses = {0x8201: 0x8204, 0x824b: 0x824e, 0x8291: 0x8294,
                0x829d: 0x64, 0x8230: 0x51, 0x8231: 0x52}
    if request['element'] != response['element'] or statuses.get(request['opcode']) != response['opcode']:
        return False
    if request['opcode'] == 0x829d:
        return request['property'] == response['property']
    if request['opcode'] == 0x8231 and request['property']:
        return any(prop == request['property'] for prop, _ in sensor_records(response['raw']))
    return True


def read_report(name):
    report = json.loads((FIXTURES / 'rc1' / f'{name}.json').read_text())
    assert report['gateway_version'] == '2.0.1-rc.1'
    assert report['elapsed_ms'] == 180000 and report['probe_complete']
    assert report['missed_events'] == 0 and not report['event_limit_reached']
    for sequence, event in enumerate(report['events'], 1):
        assert event['sequence'] == sequence and not event['truncated']
        assert len(bytes.fromhex(event['raw'])) == event['length']
    return report


names = ['sc-near', 'c-near', 'sc-far']
addresses = [0x18, 0x0c, 0x12]
reports = [read_report(name) for name in names]
parts = []
profiles = []
publications = []
replay = []
cycles = []
for node, (name, address, report) in enumerate(zip(names, addresses, reports)):
    device = report['device']
    composition = device['composition']
    assert composition['valid'] and not composition['truncated']
    raw = ','.join(str(byte) for byte in bytes.fromhex(composition['raw']))
    known = 0
    prior = json.loads((FIXTURES / f'{name}.json').read_text())
    for event in prior['events'] + report['events']:
        if event['event'] not in [2, 5]:
            continue
        known |= {0x8204: 1, 0x824e: 3, 0x8294: 4}.get(event['opcode'], 0)
        if event['opcode'] == 0x64 and event['raw'] != 'ffffff':
            known |= {0x2b: 8, 0x3c: 16}.get(event['property'], 0)
        if event['opcode'] == 0x52:
            for prop, _ in sensor_records(event['raw']):
                known |= {0x4e: 32, 0x42: 64, 0x4d: 64}.get(prop, 0)
    parts.append(f'  {{{address}, {device["product"]}, {known}, {{{raw}}}}}')
    markers = {o['condition']: o['elapsed_ms'] for o in report['observations']}
    if 'light_off' in markers:
        assert 115000 <= markers['light_off'] - markers['light_on'] <= 130000
        cycles.append(f'  {{{markers["light_on"]}, {markers["light_off"]}}}')
    outcomes = defaultdict(list)
    latencies = defaultdict(list)
    pending = None
    for event in report['events']:
        if event['event'] == 0:
            pending = event
        elif event['event'] == 3 and pending:
            outcomes[(pending['element'], pending['opcode'], pending['property'])].append(0)
            pending = None
        elif event['event'] == 2:
            if pending and matches(pending, event):
                key = pending['element'], pending['opcode'], pending['property']
                outcomes[key].append(1)
                latencies[key].append(event['elapsed_ms'] - pending['elapsed_ms'])
                pending = None
            else:
                opcode = {0x8204: 0x8201, 0x824e: 0x824b, 0x8294: 0x8291, 0x64: 0x829d, 0x52: 0x8231}.get(event['opcode'])
                if opcode:
                    outcomes[(event['element'], opcode, event['property'] if opcode == 0x829d else 0)].append(1)
        if event['event'] in [2, 5]:
            data = ','.join(str(byte) for byte in bytes.fromhex(event['raw']))
            replay.append(f'  {{{node}, {event["elapsed_ms"]}, {event["element"]}, {event["opcode"]}, {event["property"]}, {event["event"] == 5 and "true" or "false"}, {{{data}}}}}')
            if event['event'] == 5:
                # Align separate captures by their physical-on marker, not by reply arrival.
                relative = event['elapsed_ms'] - markers['light_on']
                publications.append(f'  {{{node}, {relative}, {event["element"]}, {{{data}}}}}')
    rtt = latencies[(0, 0x8201, 0)]
    assert len(rtt) == 1 and 0 < rtt[0] < 200
    for (element, opcode, prop), sequence in sorted(outcomes.items()):
        latency = min(latencies.get((element, opcode, prop), rtt))
        assert 0 < latency < 3000
        profiles.append(f'  {{{node}, {element}, {opcode}, {prop}, {latency}, {{{",".join(map(str, sequence))}}}}}')

fixture_data = '''
struct FixtureNode { uint16_t address,product,known; std::vector<uint8_t> composition; };
struct ReplyProfile { unsigned node,element,opcode,property,latency;std::vector<uint8_t> outcomes; };
struct Publication { unsigned node;int relative;unsigned element;std::vector<uint8_t> raw; };
struct CapturedReply { unsigned node,time,element,opcode,property;bool publication;std::vector<uint8_t> raw; };
struct Cycle { unsigned on,off; };
'''
for kind, variable, data in [('FixtureNode', 'fixture_nodes', parts), ('ReplyProfile', 'reply_profiles', profiles),
                             ('Publication', 'fixture_publications', publications), ('CapturedReply', 'captured_replies', replay),
                             ('Cycle', 'physical_cycles', cycles)]:
    fixture_data += f'const std::vector<{kind}> {variable}{{\n' + ',\n'.join(data) + '\n};\n'

# Only SDK/HA boundaries are doubled. Scheduling, send construction, correlation,
# callbacks, expiry and entity publication are compiled from the production files.
transport = source[source.index('bool SteinelMesh::send_node_request_('):source.index('bool SteinelMesh::node_scene_event_(')]
diagnostics = source[source.index('bool SteinelMesh::diagnostic_request_allowed_('):source.index('bool SteinelMesh::queue_node_command(')]
commands = source[source.index('bool SteinelMesh::queue_node_command('):source.index('bool SteinelMesh::send_node_request_(')]
advance = source[source.index('void SteinelMesh::advance_nodes_('):source.rindex('}  // namespace')]
access = mesh[mesh.index('bool SteinelMesh::begin_access_operation_('):mesh.index('void SteinelMesh::bind_model_(')]
constants = {
    'ESP_BLE_MESH_KEY_DEV': 0xffff,
    'ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET': 0x8008,
    'ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS': 0x02,
    'ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET': 0x8201,
    'ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET': 0x8202,
    'ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK': 0x8203,
    'ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS': 0x8204,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET': 0x824b,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET': 0x824c,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK': 0x824d,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS': 0x824e,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET': 0x8291,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET': 0x8292,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK': 0x8293,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS': 0x8294,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET': 0x829d,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET': 0x62,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET_UNACK': 0x63,
    'ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS': 0x64,
    'ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_GET': 0x8230,
    'ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS': 0x51,
    'ESP_BLE_MESH_MODEL_OP_SENSOR_GET': 0x8231,
    'ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS': 0x52,
}
for index, name in enumerate(sorted(set(re.findall(r'\bESP_BLE_MESH_[A-Z0-9_]+', transport + diagnostics)))):
    constants.setdefault(name, 0x10000 + index)
definitions = '\n'.join(f'constexpr unsigned {name}={value};' for name, value in constants.items())
harness = (ROOT / 'tests/l840_network_harness.h').read_text()
harness = harness.replace('// SDK_CONSTANTS', definitions)
harness = harness.replace('// NODE_STATE_AND_REQUEST', header[header.index('  struct NodeState {'):header.index('  struct StoredFunctions {')])
requests = source[source.index('enum Request :'):source.index('template<class Base>')]
tid_start = mesh.index('uint8_t SteinelMesh::next_tid_()')
transaction_id = mesh[tid_start:mesh.index('\n', tid_start)] + '\n'
production_code = harness + requests + transaction_id + access + diagnostics + commands + transport + advance + fixture_data
code = production_code
code += (ROOT / 'tests/l840_network_model.h').read_text()

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='steinel-l840-network-') as directory:
        binary = str(Path(directory) / 'network')
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-const-variable', '-fsanitize=address,undefined',
                        '-I', str(COMPONENT), '-x', 'c++', '-', '-o', binary],
                       input=code, text=True, check=True)
        subprocess.run([binary], check=True)
