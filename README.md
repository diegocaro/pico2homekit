# pico2homekit

Raspberry Pi Pico W firmware that reads Xiaomi LYWSD03MMC thermometers over BLE
(stock firmware, encrypted MiBeacon v5 with bind keys) and exposes them to Apple
HomeKit natively over Wi-Fi. Plain C on the Pico SDK.

Status:
- [x] BLE scan + MiBeacon decryption, Wi-Fi connect (prints readings over USB serial)
- [ ] HomeKit Accessory Protocol server (bridge with temperature/humidity/battery)

## Toolchain (macOS, no Homebrew, no sudo)

Step-by-step install instructions: [SETUP.md](SETUP.md). Everything lives in `~/.pico-sdk`:

| Component | Path |
|---|---|
| Pico SDK 2.2.0 | `~/.pico-sdk/sdk/2.2.0` |
| xPack arm-none-eabi-gcc 14.2.1 | `~/.pico-sdk/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1` |
| CMake 3.31.6 | `~/.pico-sdk/cmake/` |
| Ninja 1.12.1 | `~/.pico-sdk/ninja/` |

(The official Arm GCC macOS x86_64 builds need Homebrew's `libzstd`, which is why xPack is used.)

## Configure

```bash
cp config/config.example.h config/config.h   # git-ignored
```

Fill in Wi-Fi credentials and, for each thermometer, its name, MAC and bind key.

## Build, flash, watch

```bash
/usr/bin/make            # build/pico2homekit.uf2
/usr/bin/make test       # host unit tests for the MiBeacon parser
/usr/bin/make flash      # reboots a running Pico into BOOTSEL itself; first time, hold BOOTSEL while plugging in
/usr/bin/make monitor    # serial log; quit with Ctrl-A then K
```

## Read the thermometers from the Mac (no Pico)

`tools/mac_ble.m` scans with the Mac's own Bluetooth and prints the same decrypted readings,
using `src/mibeacon.c`, `src/sensors.c` and `config/config.h`. It needs the Pico SDK from
[SETUP.md](SETUP.md) (for its mbedTLS sources) and the Xcode Command Line Tools.
How it works: [MAC_BLE.md](MAC_BLE.md).

From the project root, compile (if needed) and run it. Stop it with Ctrl-C:

```bash
/usr/bin/make mac-ble
```

To only compile it (output goes to `build-host/mac_ble`):

```bash
/usr/bin/make build-host/mac_ble
```

The build embeds a minimal Info.plist in the binary, because macOS kills programs that use
Bluetooth without saying why.

The first time, macOS asks whether `mac_ble` may use Bluetooth: allow it. If you said no,
re-enable it in System Settings > Privacy & Security > Bluetooth.

Example output:

```
scanning...
Oficina      53.9 %RH   (rssi -54)
Living       59.8 %RH   (rssi -75)
Oficina      26.6 C   53.9 %RH   (rssi -47)
Bedroom      24.8 C   (rssi -81)
```

Each sensor sends temperature, humidity and battery in separate broadcasts, so it takes
roughly 30 seconds for every value to show up (battery can take a few minutes).
