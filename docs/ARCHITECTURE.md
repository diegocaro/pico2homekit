# How it works

The Pico W does three jobs in one single-threaded main loop (`pico_cyw43_arch_lwip_poll`):
listen to the thermometers over Bluetooth, keep Wi-Fi up, and serve HomeKit over TCP.

```
thermometers --BLE broadcast--> BTstack scan --> MiBeacon decrypt --> sensor state
                                                                        |
iPhone / HomePod <--Wi-Fi, HomeKit (HAP over IP)-- HAP server <---------+
```

## Code map

| Part | Files |
|---|---|
| Start-up, main loop, Wi-Fi, BLE scan, serial commands | `src/main.c` |
| MiBeacon v5 parsing and AES-CCM decryption | `src/mibeacon.c` |
| Configured thermometers and their latest readings | `src/sensors.c` (reads `SENSOR_LIST` from `config/config.h`) |
| Accessory layout: what Home shows | `src/bridge.c` |
| HomeKit protocol core: HTTP, Pair Setup (SRP), Pair Verify, encrypted sessions, events | `src/hap/hap.c` |
| SRP-6a with a known-answer self-test | `src/hap/srp.c`, `src/hap/srp_selftest.c` |
| TLV8 encoding | `src/hap/tlv8.c` |
| Pico glue: lwIP TCP server, mDNS, flash storage, random numbers | `src/hap_port_pico.c` |
| Library settings | `src/btstack_config.h`, `src/lwipopts.h`, `src/mbedtls_config.h` |
| Crypto | Monocypher in `third_party/monocypher/` (Ed25519, X25519, ChaCha20-Poly1305, SHA-512); mbedTLS from the Pico SDK (bignum for SRP, AES-CCM) |

`src/hap/` has no Pico-specific code: it talks to the hardware only through `hap_port.h`, so the
same server also runs on the Mac for testing (`tests/hap_host.c`).

## Start-up

1. Wait up to 3 s for a serial monitor, print the version, load the thermometers from `config.h`.
2. Load (or create) the HomeKit identity from flash; the first boot after a setup-code change
   runs the SRP self-test and computes the verifier (about 15 s).
3. Join Wi-Fi (WPA2/WPA3 first, then WPA2; retried every 30 s), turn off Wi-Fi power saving,
   start the HomeKit server on TCP port 5001 and announce `_hap._tcp` over mDNS.
4. Start the Bluetooth scan once Wi-Fi is up (or after 90 s regardless), so both radios on the
   shared CYW43439 chip don't start at the same moment.

## Thermometers

The LYWSD03MMC never needs a connection: it broadcasts BLE advertisements with Xiaomi's
MiBeacon service data (UUID `0xFE95`), one measurement per broadcast. Frames are AES-128-CCM
encrypted with the thermometer's bind key; the nonce is built from the MAC, product id and
counters, and a 4-byte tag authenticates each frame. [MAC_BLE.md](MAC_BLE.md) describes the frame
format in detail; the same `src/mibeacon.c` runs on the Pico and the Mac.

## HomeKit

The Pico is a HomeKit **bridge** (accessory id 1). Each thermometer is a bridged accessory
(ids 2, 3, …) with three services: temperature, humidity and battery (level, charging state,
low-battery alert). Values change → the server sends EVENT messages to controllers that
subscribed.

Pairing uses the standard HAP over IP flow: Pair Setup (SRP-6a, 3072-bit, SHA-512, the setup
code) creates a long-term Ed25519 pairing; each connection then runs Pair Verify (X25519 +
Ed25519 + HKDF-SHA512) and switches to ChaCha20-Poly1305 frames. Up to 8 connections at once.

## Flash layout (2 MB)

| Where | What |
|---|---|
| Start of flash | the firmware |
| End − 16 KB and − 12 KB | HomeKit store, two copies (A/B) with a CRC, so a power cut during a write keeps the previous one: device id, keys, pairings, config number |
| Last 8 KB | BTstack's own storage |

Reflashing the firmware leaves the HomeKit store alone, so pairings survive updates.

## Tests

| Command | What it checks |
|---|---|
| `make test` | MiBeacon parser and decryption against a known packet, spoofing and bad-value cases |
| `make hap-test` | the HomeKit server built for the Mac, driven by an independent Python controller (`tools/hap_client.py`): pairing, sessions, every endpoint, error cases |
| `make fuzz` | the server and parsers built with AddressSanitizer/UndefinedBehaviorSanitizer, fed random and malformed input; see [SECURITY.md](SECURITY.md) |

`hap-test` and `fuzz` create a Python virtual environment in `build-host/venv` the first time.
