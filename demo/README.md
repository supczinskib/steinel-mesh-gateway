# Local web demo

```sh
node demo/serve.cjs
```

Open <http://127.0.0.1:8765/>. Stop the server with Ctrl+C.

The demo uses the production web interface and translations.

The server renders the actual gateway HTML and translations with a local, in-memory API. It listens only on loopback, blocks external requests and does not connect to Bluetooth, Wi-Fi devices, Home Assistant or Steinel Cloud. Changes are lost on server restart.

The fixture contains an example installation with NightmatIQ Plus, L 830 SC, L 820 SC, L 42 SC, IS 180, L 810 SC, L 810 C, GL 80 SC and XLED home 2 SC. Readings, device availability and gateway diagnostics are simulated, not live measurements. Product names use the firmware mappings where available; other devices retain their backup names.

Device selection, remembered language and supported controls can be exercised locally. Firmware installation, imports, diagnostic collection and administration changes require real hardware and are disabled in the demo.
