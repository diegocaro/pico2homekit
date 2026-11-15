# pico2homekit

Firmware for a Raspberry Pi Pico W that listens to Xiaomi LYWSD03MMC Bluetooth thermometers
and shows them in Apple Home as a native HomeKit bridge. No Home Assistant, no hub, no custom
thermometer firmware: the Pico decrypts the thermometers' own broadcasts and talks HomeKit
over Wi-Fi. Plain C on the Pico SDK.

Each thermometer appears in Home with its temperature, humidity, battery level and a
low-battery alert. Up to 8 thermometers per Pico.

## What you need

- A **Raspberry Pi Pico W** (the Wi-Fi model) and a USB data cable.
- **Xiaomi LYWSD03MMC** thermometers on their stock firmware.
- Each thermometer's **MAC address and bind key** (see below).
- A **2.4 GHz Wi-Fi** network (the Pico W has no 5 GHz).
- A **Mac** to build and flash. Linux should work with small path changes; it is not tested.

### Getting the bind keys

The thermometers encrypt their broadcasts with a 16-byte *bind key* that Xiaomi assigns when
you add the thermometer to the Mi Home app. To get it:

1. Add each thermometer to the **Mi Home** app (one time; you can stop using the app after).
2. Run the [Xiaomi Cloud Tokens Extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor)
   and sign in with the same Xiaomi account. For each LYWSD03MMC it prints the MAC and the
   **BLE key** (32 hex characters).

Keep the keys private. Re-adding a thermometer to Mi Home gives it a new key.

## Quick start

1. **Install the toolchain** (one time, no Homebrew or sudo needed): [docs/SETUP.md](docs/SETUP.md).
2. **Configure.** From the project folder:
   ```bash
   cp config/config.example.h config/config.h
   ```
   Edit `config/config.h` (git-ignored): Wi-Fi name and password, a HomeKit setup code of your
   choice (`XXX-XX-XXX`; trivial codes like `123-45-678` are refused), the bridge name, and one
   line per thermometer with its name, MAC and bind key. Details: [docs/CONFIGURATION.md](docs/CONFIGURATION.md).
3. **Build and flash.** The first time, hold the Pico's **BOOTSEL** button while plugging it in.
   ```bash
   make flash
   ```
   Later flashes need no button: `make flash` reboots a running Pico into BOOTSEL itself.
4. **Watch it start** (optional):
   ```bash
   make log
   ```
   You should see it join Wi-Fi, print the setup code, and then thermometer readings.
5. **Add it to Apple Home:**
   1. Home app: **+**, **Add Accessory**, **More options…**, then **Pico Bridge** (or your name).
   2. iOS warns that it is not certified: **Add Anyway**.
   3. Enter your setup code. The step after the code takes about 8 seconds on the Pico.

The first boot after changing the setup code takes about 15 seconds longer (self-test and SRP
verifier). Pairings are kept in flash across reboots and reflashes.

## Everyday use

| Command | What it does |
|---|---|
| `make` | build `build/pico2homekit.uf2` |
| `make flash` | build and copy the firmware to the Pico |
| `make log` | print the Pico's serial output; keeps going across reboots; stop with Ctrl-C |
| `make monitor` | interactive serial console in [minicom](docs/SETUP.md#8-minicom) to type commands; quit with Ctrl-A then X |
| `make mac-ble` | read the thermometers with the Mac's Bluetooth, no Pico needed ([docs/MAC_BLE.md](docs/MAC_BLE.md)) |

Commands you can type in `make monitor`:

| Command | Effect |
|---|---|
| `status` | firmware version (git commit, `-dirty` if built with uncommitted changes), setup code, device id, pairings, open sessions |
| `debug` | toggle the protocol log: every request, pairing step, response and event (off again after a reboot) |
| `selftest` | re-run the SRP known-answer test (about 12 s; pauses HomeKit meanwhile) |
| `unpair` | forget all controllers (then add the bridge again in Home) |
| `reset` | new HomeKit identity; Home sees a brand-new accessory |

Each thermometer sends temperature, humidity and battery in separate broadcasts, so after a
reboot it takes about 30 seconds for values to appear (battery can take a few minutes).

## LED

| LED | Meaning |
|---|---|
| Fast blink | not ready yet: joining Wi-Fi, or waiting to be added in Home |
| Short blink every 5 seconds | all good |
| Double blink | error in `config/config.h` |

More in [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#the-led).

## Troubleshooting

See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## More documentation

| Document | Contents |
|---|---|
| [docs/SETUP.md](docs/SETUP.md) | Installing the toolchain on a Mac |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Every setting in `config/config.h` and what changing it does |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the firmware is put together, flash layout, tests |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | What to do when something does not work |
| [docs/SECURITY.md](docs/SECURITY.md) | Threat model, protections, fuzzing, known limits |
| [docs/MAC_BLE.md](docs/MAC_BLE.md) | How the Mac thermometer reader works |
| [THIRD_PARTY.md](THIRD_PARTY.md) | Third-party libraries and their licences |
