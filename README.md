# Steinel Mesh Gateway for ESP32-C3

[Polski](README_PL.md) · [Deutsch](README_DE.md) · [Français](README_FR.md)

> **Standalone ESP32-C3 Bluetooth Mesh gateway for local Steinel control, diagnostics, firmware updates and Home Assistant integration.**

```text
Steinel devices <-> Bluetooth Mesh <-> ESP32-C3 gateway -> Home Assistant
```

A standalone Bluetooth Mesh gateway for Steinel installations, with a local web interface and Home Assistant integration through the standard ESPHome API. Version **2.0.0** supports multiple devices from one network.

Author and maintainer: **Bartosz Supcziński** — <bartek@env.pl>

## Why this project exists

Steinel Connect Mesh devices communicate through Bluetooth Mesh, while Home Assistant uses an IP network. The ESP32-C3 joins the existing Mesh installation, exchanges commands and status messages directly with its devices, and publishes them through ESPHome.

The gateway imports the existing network from Steinel Cloud or an app-exported JSON backup. You select devices with checkboxes in its web interface. On the next boot, supported entities are registered with ESPHome and discovered by Home Assistant. No key extraction, per-lamp YAML editing, MQTT broker or custom HA integration is required.

Cloud access is a setup operation, not a dependency of everyday control. In normal use, commands and readings travel locally over Bluetooth Mesh and Wi-Fi.

## Screenshots

### ESP32-C3 Super Mini

![ESP32-C3 Super Mini](docs/images/esp32-c3-super-mini.jpg)

### Local web interface

The built-in page provides setup, control, diagnostics and browser-based firmware updates.

![Steinel Mesh Gateway local web interface](docs/images/steinel-web-interface.png)

### Home Assistant device

The standard ESPHome integration exposes each selected device separately in Home Assistant. The screenshot below shows NightmatIQ Plus.

![NightmatIQ device in Home Assistant](docs/images/home-assistant-device.png)

### Optional Home Assistant control dialog

An optional frontend module combines sensor state, illuminance, operating mode and twilight threshold in one compact dialog for NightmatIQ Plus.

![NightmatIQ control dialog in Home Assistant](docs/images/home-assistant-control.png)

## What this project provides

### Local Bluetooth Mesh integration

- Imports an existing Steinel network from Steinel Cloud or an app-exported JSON backup.
- Restores network/application keys, device keys, IV Index and the device catalog without manual key extraction.
- Communicates directly with selected devices over Bluetooth Mesh.
- Reads confirmed output state, illuminance, motion/presence and device identity, according to the detected functions.
- Controls operating mode, output, brightness, automatic control, light threshold and run time where supported.

### Reliable address and session handling

- Selects a gateway Mesh address from the unoccupied part of the provisioner range.
- Recovers automatically when Mesh peers reject a reused source address.
- Persists the first confirmed source address, preventing unnecessary changes after later restarts or temporary sensor outages.
- Preserves Mesh settings across normal reboots and OTA updates.
- Uses bounded retries and controlled restarts around cloud and Bluetooth transitions.

### Device web interface

- Device selection with checkboxes; each device has its own details and controls.
- English, Polish, German and French, with the language choice remembered in the browser.
- Live control, state refresh, installed configuration and extended diagnostics.
- Mesh RSSI, response/timeout counters, free RAM and the largest free memory block.
- Read-only device reports that can be downloaded as JSON without keys or private network identifiers.
- Password-protected automatic and manual firmware updates.
- Gateway administration for the administrator password, Wi-Fi settings and complete factory reset.

### Home Assistant integration

- A separate Home Assistant device for each selected Mesh node.
- Standard selects, numbers, sensors and binary sensors for its discovered functions.
- Per-device availability, product identity and firmware/hardware revisions when reported.
- Gateway IP address, Bluetooth Mesh readiness, status and signal strength.
- A manual refresh action and optional raw Sensor diagnostics.

The native ESPHome integration provides the controls without a custom integration, MQTT broker or per-device YAML files.

## Hardware and compatibility

### Required hardware

- ESP32-C3 Super Mini with 4 MB flash;
- native USB/JTAG serial connection for the first installation or recovery;
- 2.4 GHz Wi-Fi network;
- a Steinel Connect Mesh installation and access to its cloud or app-exported network backup;

The USB interface normally appears as an Espressif USB JTAG/serial device (`303a:1001`) and as `/dev/ttyACM*` on Linux.

### Supported target

The firmware is designed for the ESP32-C3 and ESP-IDF. Bluetooth 5 extended features are disabled because Bluetooth Mesh uses the BLE 4.2 advertising path. The configuration is intentionally sized for the limited RAM of the ESP32-C3.

### Supported functions

| Profile / model | Exposed functions |
|---|---|
| Device operating mode | Auto / Always On / Always Off |
| Generic OnOff Server `1000` | Confirmed output, On/Off select |
| Light Lightness Server `1300` | Brightness number, 0–100% |
| Light LC Server `130F` | Automatic control On/Off select, light threshold and run-time properties |
| Sensor Server `1100` | Illuminance and motion/presence, when those properties are reported |
| Scene / Scheduler | Recognized in metadata; no generic scene editor or schedule programming |

The gateway checks **bound SIG models** and authenticated Composition Data: element count, Company/Product IDs and imported capabilities. Missing Composition Data does not block valid replies using the imported AppKey; a confirmed mismatch blocks controls.

Groups, neighbouring groups and Master/Slave remain configured in Steinel Connect. The gateway uses the existing network configuration without changing it.

Output controls use the first eligible model (a Lightness output is preferred over a separate OnOff element). All Sensor elements are polled. Multiple independent outputs in one device are not represented as separate channels. Sensor readings remain unknown until the applicable property is received. A successfully transmitted command is not presented as a confirmed device state.

### Devices and compatibility

| Devices | Support path | Scope / condition |
|---|---|---|
| NightmatIQ Plus, IS 180 (Mesh variant) | SIG models | Controls and readings according to device equipment and firmware |
| L 800 SC, L 810 SC / C, L 820 SC, L 830 SC / C, L 835 SC / C, L 840 SC / C | SIG models | Controls and readings according to device equipment and firmware |
| L 40 SC / C, L 42 SC / C | SIG models | Controls and readings according to device equipment and firmware |
| L 270 digi SC, L 271 digi SC / C | SIG models | Controls and readings according to device equipment and firmware |
| RS 200 SC / C, GL 80 SC / C | SIG models | Controls and readings according to device equipment and firmware |
| Spot One SC, Spot Duo SC, Spot Way SC, Spot Garden SC, XLED home 2 SC | SIG models | Controls and readings according to device equipment and firmware |

Compatibility depends on the actual models and firmware, not the commercial name alone. The gateway checks these automatically. Sensorless C variants do not gain their own sensors through the gateway.

All entries require a valid Connect network backup, Company ID `0x0563`, supported models bound to the imported AppKey, and the limits below. Older proprietary Bluetooth firmware, ordinary non-connected variants, Z-Wave/KNX/DALI-only products and client-only buttons are not covered. Mesh conversion can be irreversible and affect compatibility with older devices; consult [Steinel's Connect FAQ](https://www.steinel-shop.de/service/produktinfos-beratung/faq/faq-connect-app/) before updating a lamp.

See [compatibility details](docs/DEVICE_COMPATIBILITY.md) and the [2.0.0 release description](docs/releases/v2.0.0.md).

## Device identity and discovery

Every selected device has the same web card, expandable identity details and a separate HA device. Functions are discovered from authenticated replies or sensor descriptors; merely having an LC or Sensor model does not create every possible control. Writes require a recent device response. Newly discovered functions are saved and exposed through a controlled restart, because native ESPHome entities must be registered at boot. Discovery can take several polling cycles on slow or unreachable devices.

Details include the model, manufacturer, Company/Product IDs and supported Mesh models, plus firmware and hardware revisions when provided by the device. Mesh addresses and hexadecimal identifiers use lowercase letters. UUID and Composition VID are omitted from the regular web details.

The main device list contains Steinel devices. Entries without a manufacturer identifier appear separately under **Unrecognized devices**; devices identified as another manufacturer are excluded. After import, cloud sign-in and local backup import collapse under **Change or reimport network** and remain available for later configuration changes.

Requested settings appear immediately in the web interface and Home Assistant while confirmation runs in the background. Actual output and sensor readings come from device responses, not inferred values. Output readback has priority after a control change and is retried without waiting for the next regular polling cycle. NightmatIQ Plus exposes its operating mode, twilight threshold, illuminance and actual output, without brightness or motion run-time controls.

## Limits and memory

- One network, one NetKey/AppKey pair, up to **16 imported devices**, **8 elements per device** and **16 distinct sensor publication groups**.
- Models requiring a different AppKey are not exposed for control.
- A bounded 16-entry queue coalesces unsent changes; one slot is reserved for a transaction continuation/retry.
- One acknowledged transaction at a time; addressed replies and property IDs are checked. Writes have a bounded retry using the same TID.
- Background polls are generated incrementally, rather than stored as a full per-device queue.
- A 512-byte flash reader parses backups without keeping the complete JSON in RAM.
- The backup workspace is the **inactive OTA partition**. Its previous image is overwritten by an import and the scratch backup is erased after processing.
- Cloud HTTPS and active Mesh run in separate modes. Disable Mesh before refreshing the network import.
- Entity membership changes take effect after a controlled restart. Selecting no devices disables Mesh without deleting its keys.
- Device availability and individual values expire after three minutes, with a bounded extension for the number of confirmed functions in larger networks. Polling all functions on a large or partly offline installation can take longer than 30 seconds.

Use each device's **Available** entity in automations. Output and automatic control use On/Off selects so Home Assistant receives an unknown state when their individual readings expire. The gateway rejects writes when the device is unavailable or its live composition contradicts the import.

The web diagnostics show free internal RAM and the largest free block during operation. Build reports list the firmware's static RAM and flash usage separately.

Device details include up to eight raw Sensor properties, separated by element, with data age and truncation indicators. A disabled-by-default **Sensor diagnostics** text entity exposes the same bounded snapshot in HA. These readings are not written to flash.

To report another device, open its details and choose **Prepare device report**. Enter the model from the label, start collection and optionally mark observations such as a covered light sensor or motion. **Download report** stops collection and saves one JSON file for attaching to an issue. Collection lasts up to three minutes, uses read-only probes and also works for imported devices not selected in HA. The report omits keys, credentials, private names, UUIDs and network addresses. It includes Composition Data, SIG/vendor model IDs, raw replies, failures and explicit truncation/missing-event indicators. The gateway keeps one bounded RAM session; the browser holds the history, so leave the page open. If the gateway disconnects, the collected data can still be downloaded and the report marks the unconfirmed stop or failed final read.

## Security

- The factory administrator account is `admin` with password `12345678`; change it on the device page immediately after joining Wi-Fi.
- The administrator password protects both the local page and firmware updates and is stored in ESP32 NVS.
- The provisioning access point uses the factory password `12345678`.
- Use setup, local backup uploads and firmware updates only on a trusted LAN. HTTP Digest authenticates the web interface but does **not** encrypt HTTP contents.
- Steinel credentials are used for HTTPS import and are not saved by the gateway.
- Network/device keys and configuration are retained in ESP32 NVS; the board must be treated as a device holding your network credentials.
- The default native ESPHome API is not encrypted. Configure API encryption in your own build if required.
- Never publish your backup, real credentials, keys or Bluetooth captures.
- Factory reset erases Wi-Fi, administrator and Mesh configuration, but keeps the installed firmware.

## Repository layout

| Path | Purpose |
|---|---|
| `esphome/steinel-c3.yaml` | Main ESPHome firmware configuration |
| `esphome/components/steinel_mesh/` | Bluetooth Mesh, device catalog, Steinel Cloud import and local web component |
| `esphome/components/steinel_mesh/mesh_protocol.h` | Portable wire-format and validation helpers |
| `esphome/components/steinel_mesh/steinel_nodes.cpp` | Device entities and transaction scheduler |
| `scripts/` | Installation, validation, USB, OTA and release helpers |
| `tests/` | Protocol, parser, transport and browser regression tests |
| `home-assistant/` | Optional Home Assistant package and compact control dialog |
| `docs/images/` | Public README images |
| `docs/DEVICE_COMPATIBILITY.md` | Supported devices and compatibility details |
| `docs/releases/` | Release descriptions |
| `demo/` | Local web interface demo |

## Ready-made installation

The recommended first installation does not require compiling ESPHome:

1. Download the latest `steinel-mesh-esp32-c3-gateway-vX.Y.Z-factory.bin` from [GitHub Releases](https://github.com/supczinskib/steinel-mesh-gateway/releases/latest).
2. Open [ESPHome Web](https://web.esphome.io/) in a WebSerial-capable browser and connect the ESP32-C3 by USB.
3. Select the board, choose **Install**, and select the downloaded `-factory.bin` file.
4. After installation, continue with **Connect Wi-Fi** and **Connect Steinel Mesh** below.

The file is processed locally by ESPHome Web. The `-factory.bin` image is for a new board or USB recovery; later browser updates use the `-ota.bin` image.

## Building from source

## Requirements

- Linux or macOS host;
- Python 3.12–3.14 and a supported ESPHome environment;
- USB access for the first installation;
- network access to the ESP32-C3; internet access when using Steinel Cloud during setup;
- Home Assistant is optional.

The supplied installer creates an isolated, reproducible environment using unmodified ESPHome `2026.7.3`. No patch is applied to the installed ESPHome package.

The supplied installer uses apt on Debian/Ubuntu. On macOS, use a Python 3.12–3.14 virtual environment with ESPHome `2026.7.3`. The helper scripts also accept an `ESPHOME` environment variable pointing to its executable.

## 1. Download and prepare the project

Clone or download this repository, enter its directory and install the pinned toolchain:

```bash
sudo bash scripts/01_install_esphome.sh
```

## 2. Validate

```bash
bash scripts/03_validate_all.sh
```

This runs repository checks and validates the ESPHome configuration.

```sh
bash scripts/00_self_test.sh
bash scripts/11_test_mesh.sh
esphome compile esphome/steinel-c3.yaml
bash scripts/10_prepare_release.sh
```

Protocol and backup-parser tests run with AddressSanitizer/UndefinedBehaviorSanitizer and synthetic data. They cover truncated packets/JSON, composition parsing, address overlap, group limits, key bindings and response correlation.

Browser tests: `cd tests && npm install && npm test`. The test-only jsdom dependency is not included in the firmware. They check all four languages, saved preferences, dynamic device cards, checkbox preservation and unchanged API command values.

If another ESP-IDF is exported in your shell, compile in a clean shell or use `env -u IDF_PATH -u IDF_TOOLS_PATH esphome compile esphome/steinel-c3.yaml`.

## 3. First installation or USB recovery

Connect the ESP32-C3 and use its stable `/dev/serial/by-id/` path when available:

```bash
sudo bash scripts/09_upload_usb.sh /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00
```

If the board has no `by-id` link, use the detected ACM port:

```bash
sudo bash scripts/09_upload_usb.sh /dev/ttyACM0
```

The same compiled image can be installed on every supported ESP32-C3 board. The first USB installation also prepares the device for subsequent browser updates, so the boot button is normally not required again.

## 4. Connect Wi-Fi

1. Connect to the access point named `nightmatiq-gateway-XXXXXX` using password `12345678`.
2. Select the target 2.4 GHz Wi-Fi network in the captive portal and enter its password.
3. Wait for the gateway to restart and connect to the selected network.
4. Open the address assigned by the router or the device hostname ending in `.local`.

The Wi-Fi configuration is stored by the device and survives firmware updates.

## 5. Connect Steinel Mesh

1. Open the gateway address in a browser.
2. Sign in as `admin` with factory password `12345678`.
3. In **Gateway administration**, change the password in **Administrator access**. The same password authorizes future firmware updates.
4. Sign in again after the automatic restart.
5. Enter your Steinel Cloud account and download the network list.
6. Select the installation, install its configuration and allow the gateway to restart. Alternatively, use **Import a local backup** to upload the network JSON exported by the Steinel app; disable Mesh before importing.
7. Select the devices to expose in Home Assistant and choose **Save selection and restart**.

The initial device address is optional; leave it automatic unless a specific address is required. The IV Index normally synchronizes with the network. Steinel credentials remain only in the browser form for the setup requests.

You can change the selection later in the same web panel. If devices or keys change in the Steinel app, disable Mesh, import a fresh backup and select again. Existing selections are retained by device identity where possible.

The panel supports **English, Polish, German and French**. English is the default; the language selector remembers your choice in this browser. Lamp names, account data and API command values are not translated.

## 6. Updating over Wi-Fi

The gateway checks the latest stable GitHub release when its page opens; **CHECK FOR UPDATES** repeats the check manually. If a newer version is available, **DOWNLOAD AND INSTALL** downloads it over HTTPS, verifies its size and SHA-256 digest, installs it and restarts the gateway. A failed update leaves the current firmware active.

Manual installation remains available under **Manual firmware file**. Use only the release file ending in `-ota.bin`; `-factory.bin` is intended exclusively for the first USB installation. No separate OTA password is used or shown to the user.

**Factory reset** removes Wi-Fi, administrator and Bluetooth Mesh settings, then restores the factory account and configuration access point without changing the installed firmware version.

From the command line, enter the administrator password when requested:

```bash
bash scripts/05_upload_ota.sh DEVICE_IP_OR_HOSTNAME
```

Use an **OTA** image through the web updater; do not upload a factory image as OTA. Wi-Fi, administrator credentials and saved Mesh configuration are retained.

## 7. Home Assistant integration

Use **Home Assistant 2025.7 or newer** for [ESPHome sub-device discovery](https://www.home-assistant.io/blog/2025/07/02/release-20257/#noteworthy-improvements-to-existing-integrations).

Home Assistant usually discovers the gateway automatically through ESPHome. If it does not:

1. Open **Settings → Devices & services**.
2. Add the **ESPHome** integration.
3. Enter the gateway IP address or hostname.
4. Assign the selected Steinel devices to the required areas.

Each selected Mesh node appears as a separate device with its own controls and diagnostics. Gateway diagnostics include the current IP address.

Disabling Mesh or entering firmware-update mode keeps selected devices and their entities registered in Home Assistant, preserving their area assignments. Readings become unavailable when communication stops; the devices are not removed.

Home Assistant may retain registry entries for deselected devices; remove unused entries if needed.

## 8. Optional compact Home Assistant dialog

The standard ESPHome integration provides all entities and controls. The files in `home-assistant/` add the compact area tile and control dialog shown above.

1. Copy `steinel-nightmatiq-package.yaml` to the Home Assistant packages directory.
2. Copy `steinel-nightmatiq-popup.js` to `/config/www/`.
3. Add `/local/steinel-nightmatiq-popup.js?v=200` as a JavaScript module in dashboard resources.
4. Reload the package configuration and refresh the browser cache.

The module detects the target automatically when exactly one matching device is present. With several matching devices, set `window.steinelNightmatiqEntities` in the JavaScript file and the `target_entities` mapping in the YAML package to the chosen device's entity IDs. Check these mappings if Home Assistant added `_2` or another suffix. This optional dialog is for NightmatIQ Plus; other devices use the standard Home Assistant controls.

The module customizes the generated area tile and Home Assistant's more-info dialog. Because that area strategy is part of the Home Assistant frontend, a future frontend release may require an update to the optional module.

The area view shows one sensor-state tile that opens the control dialog. Additional tiles for the same sensor are omitted from this view without disabling their entities or automations. Automatic target detection checks gateway identity, not just similar entity names from other integrations.

## Multiple gateways

During network import, each gateway derives a Mesh address policy from the selected installation and its own hardware identity. The same firmware can therefore be configured for different ESP32-C3 boards and Steinel installations.

The MAC suffix in the device name is enabled by default, so multiple gateways receive unique hostnames and access-point names. Configure a unique administrator password on each gateway.

## Fallback access point

If the configured Wi-Fi network is unavailable for 60 seconds, the gateway starts its password-protected access point again. Connect using factory access-point password `12345678` and update the Wi-Fi configuration through the captive portal. The local page remains protected by the administrator password selected on the device.

## Troubleshooting

### The gateway does not appear in Home Assistant

- Check that Home Assistant can reach the gateway on the IoT network.
- Add the ESPHome integration manually by IP address if discovery is filtered between VLANs.
- Confirm that the gateway is online and restart the ESPHome integration if the connection remains unavailable.

### Mesh is ready but values remain unavailable

- Move the ESP32-C3 closer to the selected Steinel devices and check **Last Mesh RSSI** in diagnostics.
- Wait for IV Index synchronization after importing a network backup.
- Use **Refresh devices** to request the current state.

### Steinel network download fails

- Confirm that the account can access the installation in the official Steinel application.
- Check internet access, DNS and system time on the gateway network.
- Wait for the gateway to restart after a failed setup request, then try again.

### OTA update fails

- Confirm the target address and gateway administrator password.
- Use the browser updater from a trusted LAN.
- Recover through native USB if the device no longer reaches Wi-Fi.

## Related project

The same Steinel NightmatIQ Plus functionality is also available as an optional integration in the [AR01V3 RF/IR, ESP-RC01 & Steinel NightmatIQ Plus Gateway](https://github.com/supczinskib/athom-ar01v3-esp-rc01-gateway). Choose that project when NightmatIQ should be added to an existing multifunction AR01V3 gateway; choose this repository for a small, dedicated ESP32-C3 installation.

## License

Copyright (C) 2026 Bartosz Supcziński.

This project is licensed under the GNU General Public License version 3 only (`GPL-3.0-only`). See [LICENSE](LICENSE).

## Credits and support

- Author and maintainer: **Bartosz Supcziński**, <bartek@env.pl>.
- ESPHome project identifier: `envpl.steinel_mesh_gateway`.

When reporting a problem, include the firmware version, ESPHome version, reset reason and relevant logs. Remove passwords, keys, authorization headers, private backups and network identifiers before sharing diagnostics.

This is an independent community project and is not an official Steinel, ESPHome, or Home Assistant product.
