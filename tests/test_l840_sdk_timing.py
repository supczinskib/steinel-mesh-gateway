"""Execute the installed ESP-IDF client functions with host clock/queue boundaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IDF = Path.home() / 'Library/Caches/esphome/idf/frameworks/5.5.5'


def function(source, name):
    match = re.search(r'^(?:static )?[^\n;{}]*\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.MULTILINE)
    if not match:
        raise ValueError(f'Cannot locate SDK function {name}')
    start, brace, depth = match.start(), source.index('{', match.start()), 1
    at = brace + 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf-root', type=Path, default=DEFAULT_IDF)
    parser.add_argument('--output', type=Path, default=ROOT / 'output/validation/l840-sdk-client')
    parser.add_argument('--installation', action='store_true', help='Connect the real SDK client functions to the three independent lamps and production gateway')
    parser.add_argument('--capture', action='store_true', help='Evaluate held-out recorded output packets using the SDK-connected model')
    parser.add_argument('--require-capture-match', action='store_true', help='Return status 2 unless the evaluated autonomous capture obtains an unconditional match')
    args = parser.parse_args()
    if args.capture and not args.installation:
        parser.error('--capture requires --installation')
    if args.require_capture_match and not args.capture:
        parser.error('--require-capture-match requires --capture')
    component = args.idf_root / 'components/bt/esp_ble_mesh'
    paths = [component / 'models/client/client_common.c', component / 'core/adv_common.c']
    client, adv = [path.read_text() for path in paths]
    mesh_path = ROOT / 'esphome/components/steinel_mesh/steinel_mesh.cpp'
    config_path = ROOT / 'esphome/.esphome/build/nightmatiq-gateway/sdkconfig.nightmatiq-gateway'
    assert 'config_server.net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20);' in mesh_path.read_text()
    config = config_path.read_text()
    assert 'CONFIG_BLE_MESH_HCI_5_0=y\n' in config and 'CONFIG_BLE_MESH_ADV_BUF_COUNT=6\n' in config
    names = ['client_pick_node_unsafe', 'client_pick_node', 'bt_mesh_is_client_recv_publish_msg',
             'client_get_status_op', 'client_get_adv_duration', 'client_calc_timeout', 'msg_send_start']
    extracted = ''.join(function(client, name) for name in names)
    extracted += 'static const struct bt_mesh_send_cb send_cb = {msg_send_start, nullptr};\n'
    extracted += function(client, 'bt_mesh_client_send_msg') + function(client, 'bt_mesh_client_free_node')
    extracted += function(adv, 'bt_mesh_pdu_duration')
    # The SDK is C; only its implicit void-pointer conversion needs adapting for this C++ host.
    extracted = extracted.replace('bt_mesh_client_node_t *node = cb_data;',
                                  'bt_mesh_client_node_t *node = static_cast<bt_mesh_client_node_t *>(cb_data);')
    source = (ROOT / 'tests/l840_sdk_contract.h').read_text().replace('// IDF_CLIENT_FUNCTIONS', extracted)
    lamp = None
    if args.installation:
        from test_l840_installation import build_code
        source = source[:source.index('\nint main() {')]
        source += (ROOT / 'tests/l840_sdk_bridge.h').read_text()
        source = build_code().replace('#include "l840_lamp_model.h"', source + '\n#include "l840_lamp_model.h"')
        if not args.capture:
            source = source.rsplit('\nint main(int argc,char **argv) {', 1)[0]
            source += (ROOT / 'tests/l840_control_sequences.h').read_text()
        lamp = (ROOT / 'tests/l840_lamp_model.h').read_text()
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output)).resolve()
    (output / 'compiled.cpp').write_text(source)
    record = {'idf_root': str(args.idf_root), 'sdk_source_sha256': {
        str(path.relative_to(args.idf_root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},
        'host_source_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'installation': args.installation,
        'captured_input': args.capture,
        'gateway_config_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in (mesh_path, config_path)},
        'run_state': 'COMPILING',
        'scope': 'Actual SDK client admission, timeout calculation/start and reply classification; clock, memory, queues and radio calls are host boundaries.',
        'not_emulated': ['BTC task contention', 'physical RF channel', 'controller/Wi-Fi coexistence',
                         'incoming segmented reply transport', 'proprietary lamp firmware internals']}
    print('Run directory:', output, flush=True)
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    with tempfile.TemporaryDirectory(prefix='steinel-sdk-client-') as directory:
        binary = str(Path(directory) / 'client')
        options = []
        if args.installation:
            (Path(directory) / 'l840_lamp_model.h').write_text(lamp)
            (output / 'l840_lamp_model.h').write_text(lamp)
            record['lamp_model_sha256'] = hashlib.sha256(lamp.encode()).hexdigest()
            options = ['-DL840_REAL_SDK=1', '-I', directory, '-I', str(ROOT / 'esphome/components/steinel_mesh')]
            if args.capture:
                options += ['-DL840_CAPTURE_ONLY=1']
        build = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-const-variable', '-fsanitize=address,undefined', *options,
                        '-x', 'c++', '-', '-o', binary],
                       input=source, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / 'build.log').write_text(build.stdout)
        if build.returncode:
            record.update(run_state='BUILD_FAILED', checks_passed=False)
            (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
            print(build.stdout, end='')
            raise SystemExit(build.returncode)
        record['run_state'] = 'RUNNING'
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        result = subprocess.run([binary, str(output)] if args.installation else [binary],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='')
    (output / 'run.log').write_text(result.stdout)
    record['checks_passed'] = result.returncode == 0
    record['run_state'] = 'COMPLETE' if result.returncode == 0 else 'FAILED'
    if args.capture and result.returncode == 0:
        import csv
        from test_l840_installation import compare_capture, evidence
        with (output / 'captured-input.csv').open() as handle:
            predictions = list(csv.DictReader(handle))
        record['comparison_with_report'] = compare_capture(evidence(), predictions, output)
        record['convergence'] = record['comparison_with_report']['assessment']['status']
        print('Captured-cycle compatibility:', record['convergence'])
    elif args.installation and result.returncode == 0:
        from test_l840_controls import summarize
        record['simulation'] = summarize(output / 'timing.csv')
        print(json.dumps(record['simulation'], indent=2))
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    if args.require_capture_match and result.returncode == 0 and record['convergence'] != 'MATCH':
        raise SystemExit(2)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
