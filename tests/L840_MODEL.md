# Mesh device regression models

Run the portable suite from the repository root:

```sh
bash scripts/11_test_mesh.sh
```

It requires Python, a C++17 compiler, AddressSanitizer and UndefinedBehaviorSanitizer.
The Home Assistant panel checks also require the Python dependencies used by
ESPHome. Browser tests use `npm ci --prefix tests` and `npm test --prefix tests`.
The tests do not connect to a gateway, Home Assistant or the Internet.

## Coverage

- `test_l840_reports.py`: captured packet replay through production callbacks,
  diagnostic state updates and polling fairness.
- `test_l840_network.py`: polling and expiry regressions with capture-derived
  reply profiles. Its output cycles are prescribed test inputs.
- `test_l840_installation.py`: production command admission, request construction,
  scheduling, callbacks and native entity publication connected to independent
  lamp controllers in `l840_lamp_model.h`.
- `test_l840_controls.py`: manual output, automatic control, concurrent diagnostics,
  replaced commands, transaction identifiers and motion sequences.
- `test_ha_output_state.py`: device-confirmed HA Output and automation conditions
  with failed writes, missing replies, unknown values and state expiry.
- `test_command_trace.py`: trace coverage and unchanged command timing when passive
  recording is enabled, including declared queue waits and missing replies.
- `test_l840_comparison.py`: positive and negative controls for captured-packet
  comparisons.

Composition Data and packet payloads come from the public reports under
`fixtures/l840/`. Fixture README files identify the source and capture limits.
Fault profiles, transitions and unknown configuration alternatives are explicit
inputs. Physical outputs and confirmed HA states are recorded separately.

Detailed host traces and source hashes are written under `output/validation/`.
A successful regression run verifies the declared software contracts; it does
not establish real radio reliability or reproduce an installation's physical
switching delay. The models do not emulate proprietary lamp firmware,
controller/Wi-Fi coexistence or the RF channel.

## Optional installed SDK checks

After compiling the firmware, the installed ESP-IDF 5.5.5 client functions can
be exercised through host clock, queue and radio boundaries:

```sh
python3 tests/test_l840_sdk_timing.py --idf-root /path/to/esp-idf
python3 tests/test_l840_sdk_timing.py --idf-root /path/to/esp-idf --installation
```

These checks require the generated gateway SDK configuration and read the SDK
source without changing it. They distinguish SDK admission, timer callbacks and
response/publication classification. They do not measure on-air transmission.
`--capture` additionally compares an autonomous cycle with recorded output
packets; `--require-capture-match` rejects conditional or missing agreement.
The ordinary regression suite does not require an installed SDK.
