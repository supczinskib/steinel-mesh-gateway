# Changelog

All notable changes to this project are documented in this file.

## 2.0.0 — 2026-10-05

- Rename the project and repository to Steinel Mesh Gateway and document multi-device scope.
- Import a bounded device catalog from Steinel Cloud or an optional app-exported JSON backup.
- Select devices through web checkboxes; change membership later without editing YAML or extracting keys.
- Register selected generic devices and entities at boot through the native ESPHome API.
- Keep selected HA devices and entities registered while Mesh is disabled or firmware-update mode is active, preserving area assignments.
- Omit empty device slots from native API responses so Home Assistant does not create a blank gateway device.
- Show one sensor-state tile in the optional NightmatIQ area view without disabling supporting entities; resolve the target by gateway identity and current entity IDs.
- Add generic SIG OnOff, Lightness, LC mode/properties and Sensor support with imported model bindings and authenticated replies; reject confirmed live Composition Data mismatches.
- Give each selected device its own web card and native HA sub-device.
- Detect individual functions from replies/descriptors and persist discovery before synchronizing HA entities through a controlled restart. Poll every Sensor element, including explicit illuminance fallback.
- Display device names, identifiers and firmware/hardware revisions.
- Separate confirmed Steinel devices from entries without a manufacturer identifier; exclude devices identified as another manufacturer from the web list and HA selection.
- Collapse cloud sign-in and backup import after setup, with cloud configuration before the optional local import.
- Use consistent expandable device details, controls and gateway diagnostics, orange checkboxes and lowercase hexadecimal identifiers. Show manufacturer names without UUID or Composition VID in regular web details.
- Keep NightmatIQ Plus controls specific to a twilight sensor; do not expose brightness, motion or motion run time from generic model declarations.
- Refresh and retain authenticated device firmware/hardware identity across restarts and availability changes.
- Improve Wi-Fi/Bluetooth radio coexistence and serialize browser status reads to prevent overlapping requests and misleading connection timeouts.
- Expose the gateway's current IP address as a Home Assistant diagnostic entity.
- Recognize L 820 SC and L 830 SC product identifiers; cover their four-element output, LC and dual-Sensor layout in import and polling regression tests.
- Correct Scene Server detection to SIG model `0x1203`; Time Server `0x1200` no longer grants scene capability. Add bound/unbound scene and Time-only parser regression checks.
- Add acknowledged operating-mode control with continuation/retry handling and device-confirmed state.
- Update requested settings immediately in the web interface and Home Assistant, confirm them in the background, and restore device readings on rejection or timeout. Prioritize targeted readback after writes; web reads are serialized and refresh every second while confirmation is pending.
- Verify physical output before LC mode and optional properties after control changes, including returning to Auto; retry missing or unsettled output readings without waiting for the normal polling cycle.
- Use a bounded/coalescing command queue, sequential acknowledged transactions, reply correlation and per-value expiry.
- Match property-specific Sensor replies to the requested property; unrelated or empty replies no longer complete the request.
- Keep refreshing expired functions and reserve background polling time; adapt freshness limits to the confirmed network workload.
- Select a usable AppKey/NetKey pair independently of backup node order.
- Expose On/Off controls as native selects with explicit unknown-state updates after expiry.
- Add bounded per-element raw Sensor diagnostics in the web panel and a disabled-by-default HA text entity.
- Add read-only device-report collection and JSON download, including imported devices not selected in HA, observation markers, captured model lists and explicit buffer limits. Exclude network secrets and private catalog identifiers. Preserve collected data for download if the gateway disconnects.
- Decode Composition VID firmware for additional Steinel products and recognize the L 42 SC Product ID.
- Keep cloud HTTPS separate from active Mesh; stream backups through a flash reader and erase scratch data after processing.
- Fail closed after an interrupted multi-key import rather than start with mixed configuration.
- Add English, Polish, German and French web UI and README documentation. English is the default; browser language selection is remembered.
- Provide one factory image and one OTA image with SHA-256 checksums; use the new repository address for firmware updates.
- Add sanitizer-backed protocol/parser, request construction, command queue and Sensor callback tests, plus browser DOM regression tests.

### Supported devices

- NightmatIQ Plus, IS 180 (Mesh variant)
- L 800 SC, L 810 SC / C, L 820 SC, L 830 SC / C, L 835 SC / C, L 840 SC / C
- L 40 SC / C, L 42 SC / C
- L 270 digi SC, L 271 digi SC / C
- RS 200 SC / C, GL 80 SC / C
- Spot One SC, Spot Duo SC, Spot Way SC, Spot Garden SC, XLED home 2 SC

The list applies to Steinel Connect Mesh variants. Available functions depend on device equipment and firmware. Groups, neighbouring groups and Master/Slave remain configured in Steinel Connect. Older Bluetooth firmware may require an irreversible Mesh conversion. Non-connected and Z-Wave/KNX/DALI-only variants are excluded.

See [compatibility details](docs/DEVICE_COMPATIBILITY.md) and the [release description](docs/releases/v2.0.0.md).

## 1.1.1 — 2026-08-27

- Wi-Fi credentials can now be changed from the local gateway administration page.
- The configured SSID is displayed without exposing the saved Wi-Fi password.
- The standard ESPHome captive portal starts after 60 seconds when the configured Wi-Fi network is unavailable.
- The fallback access point uses channel 6 for predictable discovery and connection.

## 1.1.0 — 2026-08-26

- One universal firmware image for every supported ESP32-C3 board.
- First-run Wi-Fi provisioning through a password-protected access point and captive portal.
- Unique device and access-point names derived from the ESP32-C3 MAC address.
- Persistent administrator password managed from the local page and shared with firmware updates.
- Build, validation and upload scripts no longer require `secrets.yaml`.
- Clean, pinned ESPHome 2026.7.3 build environment without modifications to the installed toolchain.
- Automatic update checks against the latest stable GitHub release when the gateway page opens.
- Dedicated Mesh-free HTTPS update mode with image-size and SHA-256 verification.
- Reproducible release packaging for factory, OTA and checksum files.
- Unified gateway administration panel for firmware updates, directly visible administrator access and factory reset.
- Confirmed factory reset that clears Wi-Fi, administrator and Bluetooth Mesh settings without changing firmware.
- Automatic Mesh address recovery now requires a NightmatIQ advertisement detected during the current boot, preventing address rotation and restart while the sensor is offline or out of range.
- Ready-made factory-image installation instructions and complete English, Polish and German documentation.

## 1.0.0 — 2026-08-24

- Initial stable standalone release for ESP32-C3 Super Mini.
- Local Steinel NightmatIQ Plus control through Bluetooth Mesh.
- Browser-assisted import of the Steinel network configuration.
- Automatic gateway Mesh address selection, recovery and confirmation.
- Local bilingual web interface with control, diagnostics and firmware updates.
- Native Home Assistant integration through ESPHome.
- Optional bilingual Home Assistant area tile and compact control dialog.
- USB, OTA, validation and secrets-configuration scripts.
- Password-protected fallback access point and captive portal.
