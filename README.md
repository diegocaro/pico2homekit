# pico2homekit

Raspberry Pi Pico W firmware that reads Xiaomi LYWSD03MMC thermometers over BLE
(stock firmware, encrypted MiBeacon v5 with bind keys) and exposes them to Apple
HomeKit natively over Wi-Fi. Plain C on the Pico SDK.

Status:
- [x] BLE scan + MiBeacon decryption, Wi-Fi connect (prints readings over USB serial)
- [x] HomeKit Accessory Protocol server: a bridge with one accessory per thermometer
      (temperature, humidity, battery level, low-battery alert), paired with an iPhone

How it fits together:

| Part | Files |
|---|---|
| BLE scan, MiBeacon decryption | `src/main.c`, `src/mibeacon.c`, `src/sensors.c` |
| HomeKit protocol core (HTTP, SRP pairing, sessions, events) | `src/hap/` |
| Accessory layout (what Home shows) | `src/bridge.c` |
| Pico networking, mDNS, flash storage | `src/hap_port_pico.c` |
| Crypto: Monocypher (Ed25519, X25519, ChaCha20-Poly1305, SHA-512); mbedTLS (bignum for SRP, AES-CCM) | `third_party/monocypher/`, Pico SDK |

Third-party libraries, their versions and licences (including the Raspberry Pi-only terms of
the Wi-Fi/Bluetooth driver and BTstack): [THIRD_PARTY.md](THIRD_PARTY.md).

## Security

Threat model: anyone on the Wi-Fi network can connect to the HomeKit port; anyone in
Bluetooth range can broadcast advertisements. Protections in place:

- HTTP parsing is strict (digits-only `Content-Length`, hard size caps); encrypted sessions
  never accept trailing plaintext after Pair Verify.
- Pair Setup is rate limited (1, 2, 4 … 60 s between attempts, reset on success), and an
  idle or half-open connection cannot hold one of the 8 slots: the oldest connection that
  never completed Pair Verify is closed for newcomers, and all of them after 60 s.
- Thermometers with a bind key are only accepted encrypted and authenticated; the
  authenticated packet counter must increase (replays are dropped); NaN or out-of-range
  values are ignored.
- Keys come from a SHA-512 entropy pool (ring-oscillator noise + SDK generator + timer)
  expanded with ChaCha20; key material is wiped after use.

`make fuzz` builds the HomeKit server and the BLE/TLV parsers with AddressSanitizer and
UndefinedBehaviorSanitizer, runs the protocol test, 2 million random parser inputs and a
minute of malformed network traffic.

Known limits: the setup code is printed on the USB serial console (physical access); a
flash dump of the Pico allows offline guessing of the 8-digit setup code (inherent to SRP);
the Pair Setup backoff restarts after a reboot.

## Toolchain (macOS, no Homebrew, no sudo)

Step-by-step install instructions: [SETUP.md](SETUP.md). Everything lives in `~/.pico-sdk`:

| Component | Path |
|---|---|
| Pico SDK 2.2.0 | `~/.pico-sdk/sdk/2.2.0` |
| xPack arm-none-eabi-gcc 14.2.1 | `~/.pico-sdk/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1` |
| CMake 3.31.6 | `~/.pico-sdk/cmake/` |
| Ninja 1.12.1 | `~/.pico-sdk/ninja/` |

(The official Arm GCC macOS x86_64 builds need Homebrew's `libzstd`, which is why xPack is used.)

`make monitor` also needs [minicom](https://salsa.debian.org/minicom-team/minicom), from
MacPorts (`sudo port install minicom`) or Homebrew (`brew install minicom`). `make log` works
without it.

## Configure

```bash
cp config/config.example.h config/config.h   # git-ignored
```

Fill in Wi-Fi credentials, a HomeKit setup code of your choice (`XXX-XX-XXX`) and, for each
thermometer, its name, MAC and bind key.

## Build, flash, watch

```bash
/usr/bin/make            # build/pico2homekit.uf2
/usr/bin/make test       # host unit tests for the MiBeacon parser
/usr/bin/make hap-test   # HomeKit server on this Mac vs. an independent Python controller
/usr/bin/make flash      # reboots a running Pico into BOOTSEL itself; first time, hold BOOTSEL while plugging in
/usr/bin/make monitor    # serial console in minicom (needs minicom installed); quit with Ctrl-A then X
/usr/bin/make log        # read-only serial log that survives reboots; stop with Ctrl-C
```

## Add it to Apple Home

1. Home app: **+**, then **Add Accessory**, then **More options…**, then **Pico Bridge**.
2. iOS warns it is an uncertified accessory: **Add Anyway**.
3. Enter the setup code from `config/config.h` (also printed at boot). The step after
   the code takes about 8 seconds on the Pico.

The first boot after changing the setup code takes about 15 seconds longer (self-test and
SRP verifier). Pairings are kept in flash across reboots and reflashes.

Commands you can type in `make monitor`:

| Command | Effect |
|---|---|
| `status` | firmware version (git commit, `-dirty` if built with uncommitted changes), setup code, device id, pairings, open sessions |
| `debug` | toggle the protocol log: every request, pairing TLV items, responses, events |
| `selftest` | re-run the SRP known-answer test (about 12 s; pauses HomeKit meanwhile) |
| `unpair` | forget all controllers (then add the bridge again in Home) |
| `reset` | new HomeKit identity; Home sees a brand-new accessory |

If Home shows the bridge as "No Response" forever after you removed it from Home while the
Pico was off, run `unpair` and add it again.

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
