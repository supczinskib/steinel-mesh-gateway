"""Independent lamps + production gateway; exit status distinguishes tests from evidence."""
import argparse
import csv
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import uuid
from l840_compare import assess, compare_packets

ROOT = Path(__file__).resolve().parents[1]


def build_code(revision=None, source_dir=None):
    legacy = runpy.run_path(str(ROOT / 'tests/test_l840_network.py'))
    # Share the SDK boundary, not the old scripted output generator.
    helpers = (ROOT / 'tests/l840_network_model.h').read_text()
    helpers = helpers[:helpers.index('void replay_captured_packets()')]
    fixtures = ('struct FixtureNode { uint16_t address,product,known; std::vector<uint8_t> composition; };\n'
                'const std::vector<FixtureNode> fixture_nodes{\n' + ',\n'.join(legacy['parts']) + '\n};\n')
    report = json.loads((ROOT / 'tests/fixtures/l840/rc3/sc-0018-motion.json').read_text())
    stimuli = [event for event in report['events'] if event['event'] == 5 and event['opcode'] == 0x52 and
               event['element'] == 2 and event['raw'].startswith('4008')]
    assert len(stimuli) == 4
    fixtures += ('struct MotionStimulus { uint32_t at;uint8_t value; };\nconst std::vector<MotionStimulus> captured_motion{\n' +
                 ',\n'.join('  {%d,%d}' % (event['elapsed_ms'], bytes.fromhex(event['raw'])[2]) for event in stimuli) + '\n};\n')
    # Recorded output cycles/outcome sequences are not linked into this model.
    production = legacy['production_code'].replace(legacy['fixture_data'], fixtures)
    if revision or source_dir:
        def source(name):
            if source_dir:
                return (Path(source_dir) / name).read_text()
            return subprocess.run(['git', 'show', f'{revision}:esphome/components/steinel_mesh/{name}'],
                                  cwd=ROOT, capture_output=True, text=True, check=True).stdout
        nodes, mesh = source('steinel_nodes.cpp'), source('steinel_mesh.cpp')
        requests = nodes[nodes.index('enum Request :'):nodes.index('template<class Base>')]
        access = mesh[mesh.index('bool SteinelMesh::begin_access_operation_('):mesh.index('void SteinelMesh::bind_model_(')]
        diagnostics = nodes[nodes.index('bool SteinelMesh::diagnostic_request_allowed_('):nodes.index('bool SteinelMesh::queue_node_command(')]
        commands = nodes[nodes.index('bool SteinelMesh::queue_node_command('):nodes.index('bool SteinelMesh::send_node_request_(')]
        transport = nodes[nodes.index('bool SteinelMesh::send_node_request_('):nodes.index('bool SteinelMesh::node_scene_event_(')]
        advance = nodes[nodes.index('void SteinelMesh::advance_nodes_('):nodes.rindex('}  // namespace')]
        tid_start = mesh.index('uint8_t SteinelMesh::next_tid_()')
        transaction_id = mesh[tid_start:mesh.index('\n', tid_start)] + '\n'
        production = legacy['harness'] + requests + transaction_id + access + diagnostics + commands + transport + advance + fixtures
    return (production + helpers + '\n}\n' +
            (ROOT / 'tests/l840_installation_model.h').read_text())


def evidence():
    directory = ROOT / 'tests/fixtures/l840/rc3'
    reports = {}
    for path in sorted(directory.glob('*.json')):
        report = json.loads(path.read_text())
        assert report['gateway_version'] == '2.0.1-rc.3'
        events = report['events']
        gaps = sum(max(0, b['sequence'] - a['sequence'] - 1) for a, b in zip(events, events[1:]))
        assert gaps == report['missed_events']
        observations = []
        for marker in report['observations']:
            if marker['condition'] not in ('light_on', 'light_off'):
                continue
            expected = marker['condition'] == 'light_on'
            candidates = []
            for event in events:
                if event['event'] not in (2, 5) or event['element'] != 0 or event['elapsed_ms'] < marker['elapsed_ms']:
                    continue
                if event['opcode'] == 0x8204:
                    value = bytes.fromhex(event['raw'])[0] != 0
                elif event['opcode'] == 0x824e:
                    value = int.from_bytes(bytes.fromhex(event['raw'])[:2], 'little') != 0
                else:
                    continue
                if value == expected:
                    candidates.append(event['elapsed_ms'] - marker['elapsed_ms'])
            observations.append({'marker_ms': marker['elapsed_ms'], 'condition': marker['condition'],
                                 'first_matching_reply_ms': min(candidates) if candidates else None})
        reports[path.name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                              'diagnostic_buffer_gaps': gaps, 'markers': observations}
    assert len(reports) == 3, 'All three rc.3 captures must be available for comparison'
    return {'reports': reports,
            'not_identified': ['actual turn-on automation (the pasted sequence is Output Off -> LC Mode On)',
                               'HA command arrival/send/ack timestamps', 'group subscription/publication addresses and messages',
                               'default transition time and LC fade/occupancy settings',
                               'separate ordinary-request and reply losses', 'meaning of repeated light_on markers'],
            'reason': 'Missing inputs are varied explicitly; no parameter is fitted to the reported 15/30-second outcome.'}


def summarize(directory):
    with (directory / 'scenarios.csv').open() as handle:
        rows = list(csv.DictReader(handle))
    grouped = defaultdict(list)
    for row in rows:
        key = tuple((name, value) for name, value in row.items() if name not in (
            'node', 'first_write_ms', 'physical_on_ms', 'confirmed_on_ms', 'ha_on_ms',
            'physical_off_ms', 'confirmed_off_ms', 'tx_dropped', 'rx_dropped'))
        grouped[key].append(row)
    counts = defaultdict(int)
    ranges = defaultdict(list)
    examples = []
    for key, lamps in grouped.items():
        assert len(lamps) == 3 and len({r['node'] for r in lamps}) == 3
        description = dict(key)
        counts['scenarios'] += 1
        for row in lamps:
            for direction in ('on', 'off'):
                physical, confirmed = row[f'physical_{direction}_ms'], row[f'confirmed_{direction}_ms']
                if physical == 'not_observed':
                    continue
                if confirmed == 'not_observed':
                    counts['unconfirmed_physical_transitions'] += 1
                    continue
                lag = int(confirmed) - int(physical)
                assert lag >= 0
                ranges[f'{description["input"]}_report_{direction}_lag_ms'].append(lag)
        if description['input'] == 'manual_on':
            times = [int(r['physical_on_ms']) - 10000 for r in lamps if r['physical_on_ms'] != 'not_observed']
            if len(times) != 3:
                counts['manual_on_incomplete'] += 1
            if times:
                ranges['manual_on_first_ms'].append(min(times))
                ranges['manual_on_last_ms'].append(max(times))
                if max(times) >= 10000:
                    counts['manual_on_over_10s'] += 1
                    if len(examples) < 8:
                        examples.append({'conditions': description, 'physical_after_input_ms': {
                            r['node']: (int(r['physical_on_ms']) - 10000 if r['physical_on_ms'] != 'not_observed' else None)
                            for r in lamps}})
            if description['loss_percent'] == '0':
                assert len(times) == 3
                ranges['reliable_manual_on_first_ms'].append(min(times))
                ranges['reliable_manual_on_last_ms'].append(max(times))
        if description['input'] == 'posted_off_auto':
            assert all(r['physical_on_ms'] == 'not_observed' for r in lamps)
            counts['literal_posted_sequence_did_not_turn_on'] += 1
    return {'counts': dict(counts), 'ranges': {key: {'min': min(values), 'max': max(values)} for key, values in ranges.items()},
            'slow_examples': examples, 'latency_origin': 'produced by packet delivery, device timers and production command queue; not assigned to outputs'}


def compare_capture(record, predictions, directory):
    report = json.loads((ROOT / 'tests/fixtures/l840/rc3/sc-0018-motion.json').read_text())
    samples = [{'at': e['elapsed_ms'], 'opcode': e['opcode'],
                'value': int.from_bytes(bytes.fromhex(e['raw']), 'little')}
               for e in report['events'] if e['event'] in (2, 5) and e['element'] == 0 and e['opcode'] in (0x8204, 0x824e)]
    rtts, pending = [], {}
    status_for = {0x8201: 0x8204, 0x824b: 0x824e}
    previous_sequence = None
    for event in report['events']:
        if previous_sequence is not None and event['sequence'] != previous_sequence + 1:
            pending.clear()
        previous_sequence = event['sequence']
        if event['event'] == 0:
            # There is only one acknowledged access operation in this firmware.
            # A newer diagnostic send cannot be paired to an old missing reply.
            pending.clear()
            if event['opcode'] in status_for:
                pending[(event['element'], status_for[event['opcode']])] = event['elapsed_ms']
        elif event['event'] == 3:
            pending.clear()
        elif event['event'] == 2:
            sent = pending.pop((event['element'], event['opcode']), None)
            if sent is not None:
                rtts.append(event['elapsed_ms'] - sent)
    bound = max(rtts) if rtts else None
    markers = record['reports']['sc-0018-motion.json']['markers']
    observed = {marker['condition']: marker['marker_ms'] for marker in markers}
    received = {marker['condition']: marker['marker_ms'] + marker['first_matching_reply_ms'] for marker in markers
                if marker['first_matching_reply_ms'] is not None}
    comparisons = []
    for row in predictions:
        if row['node'] != '0x18':
            continue
        result = {'conditions': {key: int(row[key]) for key in ('fade_on_ms', 'fade_off_ms', 'occupancy_delay_ms')},
                  'group_mechanism': row['group_mechanism'], 'differences_ms': {}}
        for direction in ('on', 'off'):
            marker = f'light_{direction}'
            result['differences_ms'][f'physical_{direction}_minus_marker'] = (
                int(row[f'physical_{direction}_ms']) - observed[marker] if row[f'physical_{direction}_ms'] != 'not_observed' else None)
            result['differences_ms'][f'confirmed_{direction}_minus_recorded_reply'] = (
                int(row[f'confirmed_{direction}_ms']) - received[marker] if
                row[f'confirmed_{direction}_ms'] != 'not_observed' and marker in received else None)
        with (directory / row['trajectory_file']).open() as handle:
            trajectory = [(int(point['elapsed_ms']), int(point['lightness'])) for point in csv.DictReader(handle) if point['node'] == '0x18']
        result['packet_comparison'] = compare_packets(trajectory, samples, bound)
        comparisons.append(result)
    assert len(comparisons) == 54
    assessed = assess([result['packet_comparison'] for result in comparisons],
                      ['LC timing properties and group configuration not recorded', 'physical observation timing uncertainty unknown'])
    return {'stimuli': 'Only four recorded Motion Sensed publications are simulation inputs; output replies and physical markers are held out.',
            'timing_note': 'Publication arrival is a proxy for sensor stimulus time; browser observation error is unknown.',
            'predictions': comparisons, 'transport_bound_ms': bound,
            'sample_count': len(samples), 'assessment': assessed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'output/validation/l840-installation')
    parser.add_argument('--require-convergence', action='store_true', help='Fail if physical-installation equivalence is not established')
    parser.add_argument('--revision', help='Compare a committed node scheduler in the same environment; device model and protocol helpers stay fixed')
    parser.add_argument('--quick', action='store_true', help='Use one seed, zero fade and no diagnostics for harness development')
    parser.add_argument('--capture-only', action='store_true', help='Compare independent settings against the recorded autonomous cycle')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    run_dir = (args.output / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])).resolve()
    run_dir.mkdir()
    record = evidence()
    code = build_code(args.revision)
    record['gateway_revision'] = args.revision or 'working tree'
    record['host_source_sha256'] = hashlib.sha256(code.encode()).hexdigest()
    record['lamp_model_sha256'] = hashlib.sha256((ROOT / 'tests/l840_lamp_model.h').read_bytes()).hexdigest()
    record['run_state'] = 'RUNNING'
    record['quick'] = args.quick
    record['capture_only'] = args.capture_only
    record['run_directory'] = str(run_dir)
    record['test_sources'] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in (
        ROOT / 'tests/test_l840_installation.py', ROOT / 'tests/l840_installation_model.h',
        ROOT / 'tests/l840_network_harness.h', ROOT / 'tests/l840_network_model.h', ROOT / 'tests/test_l840_network.py')}
    if args.revision:
        record['compared_commit'] = subprocess.run(['git', 'rev-parse', args.revision], cwd=ROOT, text=True,
                                                  capture_output=True, check=True).stdout.strip()
    (run_dir / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
    (run_dir / 'compiled.cpp').write_text(code)
    lamp_source = (ROOT / 'tests/l840_lamp_model.h').read_text()
    (run_dir / 'l840_lamp_model.h').write_text(lamp_source)
    print('Run directory:', run_dir, flush=True)
    with tempfile.TemporaryDirectory(prefix='steinel-l840-installation-') as build_dir:
        binary = str(Path(build_dir) / 'installation')
        # Compile a frozen lamp header, even if a subsequent editing turn begins.
        (Path(build_dir) / 'l840_lamp_model.h').write_text(lamp_source)
        compile_command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-const-variable', '-fsanitize=address,undefined',
                        '-I', build_dir, '-I', str(ROOT / 'esphome/components/steinel_mesh'),
                        '-x', 'c++', '-', '-o', binary]
        if args.quick:
            compile_command.insert(1, '-DL840_QUICK=1')
        if args.capture_only:
            compile_command.insert(1, '-DL840_CAPTURE_ONLY=1')
        subprocess.run(compile_command, input=code, text=True, check=True)
        result = subprocess.run([binary, str(run_dir)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='')
    (run_dir / 'run.log').write_text(result.stdout)
    record['model_checks_passed'] = result.returncode == 0
    record['source_files'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [
        ROOT / 'esphome/components/steinel_mesh' / name for name in ('steinel_nodes.cpp', 'steinel_mesh.cpp', 'steinel_mesh.h', 'mesh_protocol.h')]}
    record['run_state'] = 'COMPLETE' if result.returncode == 0 else 'FAILED'
    if result.returncode == 0:
        record['simulation'] = summarize(run_dir)
        with (run_dir / 'captured-input.csv').open() as handle:
            record['captured_input_prediction'] = list(csv.DictReader(handle))
        record['comparison_with_report'] = compare_capture(record, record['captured_input_prediction'], run_dir)
        record['convergence'] = record['comparison_with_report']['assessment']['status']
        record['convergence_scope'] = 'Held-out output packets for the captured autonomous SC 0x0018 cycle; not the HA camera automation'
    else:
        record['convergence'] = 'MODEL_EXECUTION_FAILED'
    (run_dir / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Captured-cycle compatibility:', record['convergence'])
    if result.returncode:
        raise SystemExit(result.returncode)
    if args.require_convergence and record['convergence'] != 'MATCH':
        raise SystemExit(2)


if __name__ == '__main__':
    main()
