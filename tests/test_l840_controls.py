"""Drive real HA queue/SDK/callback paths through complete local control sequences."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from test_l840_installation import ROOT, build_code


def summarize(path):
    with path.open() as handle:
        rows = list(csv.DictReader(handle))
    outcomes = {'node', 'lc_mode', 'physical_on', 'gateway_on', 'ha_output', 'ha_known',
                'first_physical_on_ms', 'first_confirmed_on_ms', 'tx_dropped', 'rx_dropped'}
    fields = [field for field in rows[0] if field not in outcomes]
    cases = defaultdict(list)
    for row in rows:
        for field in ('lc_mode', 'physical_on', 'ha_known'):
            assert row[field] in ('0', '1')
        assert row['gateway_on'] in ('-1', '0', '1')
        cases[tuple(row[field] for field in fields)].append(row)
    counts = Counter()
    reliable = Counter()
    for key, lamps in cases.items():
        assert len(lamps) == 3 and len({lamp['node'] for lamp in lamps}) == 3
        description = dict(zip(fields, key))
        on = sum(int(lamp['physical_on']) for lamp in lamps)
        outcome = 'all_on' if on == 3 else 'all_off' if on == 0 else 'mixed'
        counts[outcome] += 1
        if description['loss_percent'] == '0':
            reliable[outcome] += 1
        counts['physical_confirmed_disagreements_at_end'] += sum(
            lamp['physical_on'] != lamp['gateway_on'] for lamp in lamps)
        counts['physical_ha_output_disagreements_at_end'] += sum(
            lamp['ha_known'] != '1' or lamp['ha_output'] != ('On' if lamp['physical_on'] == '1' else 'Off')
            for lamp in lamps)
    return {'timing_scenarios': len(cases), 'outcomes': dict(counts), 'reliable_link_outcomes': dict(reliable),
            'interpretation': 'On outcomes are measurements, not pass criteria. A motion pulse during manual control need not light every lamp.',
            'scope': 'Production command queue and callbacks connected to independent lamps; not a calibrated RF or physical-installation acceptance test.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'output/validation/l840-control-sequences')
    parser.add_argument('--revision', help='Substitute committed gateway command/scheduler code; keep the same independent lamps')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output)).resolve()
    source = build_code(args.revision).rsplit('\nint main(int argc,char **argv) {', 1)[0]
    source += (ROOT / 'tests/l840_control_sequences.h').read_text()
    lamp = (ROOT / 'tests/l840_lamp_model.h').read_text()
    (output / 'compiled.cpp').write_text(source)
    (output / 'l840_lamp_model.h').write_text(lamp)
    print('Run directory:', output, flush=True)
    with tempfile.TemporaryDirectory(prefix='steinel-control-sequences-') as directory:
        binary = str(Path(directory) / 'controls')
        (Path(directory) / 'l840_lamp_model.h').write_text(lamp)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-const-variable', '-fsanitize=address,undefined',
                        '-I', directory, '-I', str(ROOT / 'esphome/components/steinel_mesh'),
                        '-x', 'c++', '-', '-o', binary], input=source, text=True, check=True)
        result = subprocess.run([binary, str(output)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='')
    (output / 'run.log').write_text(result.stdout)
    record = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'lamp_sha256': hashlib.sha256(lamp.encode()).hexdigest(),
              'gateway_revision': args.revision or 'working tree', 'checks_passed': result.returncode == 0}
    if result.returncode == 0:
        record['simulation'] = summarize(output / 'timing.csv')
        print(json.dumps(record['simulation'], indent=2))
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
