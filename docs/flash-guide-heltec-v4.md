# Flashing This Fork — Heltec V4

Thursday-ready guide. All four V4 firmware variants verified building from this fork.

## Prerequisites (once)

```bash
pip install --user --break-system-packages platformio   # or any PlatformIO install
export PATH="$HOME/.local/bin:$PATH"
cd src/MeshCore
```

## Which firmware to flash

| Goal | Environment |
|---|---|
| **Start here** — pairs with the stock phone app over BLE | `heltec_v4_companion_radio_ble` |
| Phone/PC connection over USB cable instead | `heltec_v4_companion_radio_usb` |
| WiFi connection (app talks over network) | `heltec_v4_companion_radio_wifi` |
| Always-on relay node (when you have a 2nd device) | `heltec_v4_repeater` |
| BBS / message server | `heltec_v4_room_server` |

## Flash

```bash
pio run -e heltec_v4_companion_radio_ble -t upload
```

(PlatformIO auto-detects the serial port; use `--upload-port COMx` / `/dev/ttyACM0` if needed.)

If upload can't connect: hold **BOOT**, tap **RESET**, release **BOOT** → ROM download mode → retry.

## First-boot checks (first 10 minutes)

1. **Boot** — display/backlight comes alive; LED blinks per stock behavior
2. **Pair** — stock MeshCore app (Android/iOS) → Add Device → BLE → node appears
3. **Identity** — node generates its keys on first boot (takes a few seconds)
4. **Radio** — app's node info shows frequency/modem params (defaults: region preset values)
5. **Serial console** (optional, USB): `pio device monitor` shows MESH_DEBUG output with our build

## Setting the US frequencies

From the app's node settings (raw parameters — our fork's whitelist accepts them):
- **910.250 MHz / BW500 / SF10 / CR5** — the FCC-aligned "USA Regulatory" setting (issue #945)
- **927.875 MHz / BW62.5 / SF7 / CR8** — SoCal alternative (issue #1798)

Stock 910.525 defaults work as always.

## What our fork changes that you can observe

- Nothing breaks: protocol-compatible with stock nodes and the stock app
- Our dual-path extras (`0x0C`) ride PATH packets — stock nodes ignore them silently
- On multi-node meshes (later): repeated messages rotate routes on retry; repeaters/room servers learn measured path quality from traceroutes

## Recovery (memorize this one line)

**Hold BOOT → tap RESET → release BOOT** → ROM download mode → reflash anything.
No firmware we ship can brick the board — the USB bootloader lives in silicon.

## Build artifacts

Binaries land in `.pio/build/<env>/firmware.bin` (+ boot_app0/partitions for ESP32).
Keep the `.bin` from each env you flash — flashing back is `esptool` with the same files.

## No-toolchain flash (prebuilt binaries)

Binaries staged at `artifacts/heltec-v4-companion-ble/` (bootloader.bin,
partitions.bin, boot_app0.bin, firmware.bin). On any machine with Python:

```bash
pip install esptool
esptool.py --chip esp32s3 --baud 921600 --port /dev/ttyACM0 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

(Windows: port `COMx`. If 921600 is unstable use 115200. BOOT+RESET for
download mode if it won't connect.)
