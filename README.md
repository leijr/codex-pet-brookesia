# Codex Pet Brookesia

ESP32-S3 Touch AMOLED 1.75 firmware based on the Waveshare ESP-Brookesia phone demo.

This version keeps the official Brookesia launcher/app structure and adds a `Codex Pet` app. On boot, the firmware auto-opens the app, but the app is also registered in the Brookesia launcher.

## Behavior

- `running`: animated pet display.
- `done`: pet stops after the Mac bridge reports completion.
- `idle/offline`: static waiting state.
- State is fetched from the Mac bridge with `GET /state`.

The done voice is played on the Mac side by `codex-pet-audio`, so Bluetooth speakers can be handled by macOS.

## Mac Bridge

The ESP32 polls the bridge created by the companion `codex-pet` command:

```sh
codex-pet serve
codex-pet running
codex-pet done
```

## Build And Flash

```sh
cd codex-pet-brookesia
. "$IDF_PATH/export.sh"
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

Configure these values in `menuconfig` under `Codex Pet App`:

- Wi-Fi SSID
- Wi-Fi password
- Codex pet bridge state URL, for example `http://<mac-ip>:8765/state`

`sdkconfig`, `dependencies.lock`, `build/`, and `managed_components/` are intentionally ignored because they contain local settings, generated files, or downloaded dependencies.
