# Device compatibility

## Supported devices

- NightmatIQ Plus, IS 180 (Mesh variant)
- L 800 SC, L 810 SC / C, L 820 SC, L 830 SC / C, L 835 SC / C, L 840 SC / C
- L 40 SC / C, L 42 SC / C
- L 270 digi SC, L 271 digi SC / C
- RS 200 SC / C, GL 80 SC / C
- Spot One SC, Spot Duo SC, Spot Way SC, Spot Garden SC, XLED home 2 SC

The list applies to **Steinel Connect Mesh** variants. Available controls and readings depend on each device's equipment and firmware. The gateway detects these functions automatically and displays each device separately in the web interface and Home Assistant.

## Network requirements

Import an existing Steinel Connect network from the cloud or an app-exported JSON backup. Devices must share the imported NetKey/AppKey pair, have a valid DeviceKey and use Company ID `0x0563`. Models must be bound to the imported AppKey.

The gateway supports up to 16 imported devices, 8 elements per device and 16 distinct sensor publication group addresses. These publication addresses are not a limit on the number of lighting groups configured in Steinel Connect.

## Functions

| SIG model | ID | Gateway function |
|---|---|---|
| Generic OnOff Server | `0x1000` | Switching and confirmed output state |
| Light Lightness Server | `0x1300` | Brightness |
| Light LC Server | `0x130F` | Automatic control, light threshold and run time |
| Sensor Server | `0x1100` | Illuminance, presence and motion |
| Scene Server | `0x1203` | Device operating-mode commands |
| Scheduler Server | `0x1206` | Model information in device details |

The gateway enables controls from imported model bindings and authenticated device replies. Live Composition Data adds identity/model validation; a missing reply does not block SIG communication, but a confirmed mismatch does. It exposes one output per device and reads all Sensor elements. Sensorless variants provide output controls without their own sensor readings.

Device details show the model, manufacturer/product identifiers, Mesh models and firmware/hardware revisions when provided by the device. The list uses imported device names and lowercase Mesh addresses. UUID and Composition VID are not shown in regular web details.

The main list contains Steinel devices. Entries without a manufacturer identifier appear in a separate section for identification and reports; devices identified as another manufacturer are excluded.

NightmatIQ Plus exposes its operating mode, twilight threshold, illuminance and actual output. Brightness, motion and motion run-time controls are not exposed for this twilight sensor, even if its firmware advertises the corresponding generic Mesh models.

## Steinel Connect configuration

Groups, neighbouring groups, Master/Slave, scenes and schedules remain configured in Steinel Connect. The gateway uses this configuration without changing it. Manual output control can interact with a device's automatic controller.

Provisioning new devices, lamp firmware updates and independently controlled multi-channel outputs are outside the gateway's scope.

## Firmware and interface requirements

Older proprietary Bluetooth/Smart Remote firmware may require the manufacturer's Mesh conversion. Conversion may be irreversible. Ordinary non-connected and Z-Wave/KNX/DALI-only variants, client-only remotes and the Control PRO II, True Presence, DCS and OEM ranges are not included in the device list.

## Manufacturer references

- [Steinel Connect FAQ](https://www.steinel-shop.de/service/produktinfos-beratung/faq/faq-connect-app/): Mesh product families and conversion requirements.
- [Steinel Mesh conversion guide](https://www.steinel.de/out/media/appnotesdoc/219655_Installation%20Guide_Bluetooth%20Mesh%20EN.pdf): conversion procedure and requirements.
