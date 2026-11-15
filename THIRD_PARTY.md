# Third-party software

What pico2homekit is built from, what each piece does, and under which licence.
Audited from the linker map (`build/pico2homekit.elf.map`) of the firmware: only code that
actually ends up in `pico2homekit.uf2` is listed under "In the firmware".

## In the firmware

| Component | Version | What it does here | Licence | Location |
|---|---|---|---|---|
| **Monocypher** | 4.0.2 | HomeKit crypto: Ed25519 signatures, X25519 key exchange, ChaCha20-Poly1305, SHA-512, HKDF | CC0-1.0 **or** BSD-2-Clause (your choice) | `third_party/monocypher/` (vendored, unmodified) |
| **Raspberry Pi Pico SDK** | 2.2.0 | Boot code, hardware drivers, USB serial, flash access, timers, random numbers, `printf` | BSD-3-Clause | `~/.pico-sdk/sdk/2.2.0` (see [docs/SETUP.md](docs/SETUP.md)) |
| ↳ `pico_printf` | (in SDK) | Small `printf` implementation | MIT (Marco Paland) | SDK `src/rp2_common/pico_printf/` |
| **cyw43-driver** | commit `dd75682` | Driver for the Pico W's Infineon CYW43439 Wi-Fi/Bluetooth chip | Raspberry Pi licence (`LICENSE.RP`): use and redistribution **only with Raspberry Pi chips** (RP2040, RP2350) | SDK `lib/cyw43-driver/` |
| ↳ CYW43439 firmware blobs | Wi-Fi 7.95.49, BT patch, NVRAM | Closed-source firmware the driver uploads into the radio chip at boot (about 230 KB of the image) | Distributed with cyw43-driver under the same terms | SDK `lib/cyw43-driver/firmware/` |
| **BTstack** (core only) | 2.0.0 | Bluetooth LE host stack: HCI, passive scanning, advertisement parsing | Raspberry Pi licence (`src/rp2_common/pico_btstack/LICENSE.RP`): free for use **on Pico W / Pico 2 W family boards** and products built on them. BTstack's own `LICENSE` alone is non-commercial. | SDK `lib/btstack/` |
| **lwIP** | 2.2.1 | TCP/IP: TCP server for HomeKit, DHCP, DNS, IGMP, mDNS responder (`_hap._tcp`) | BSD-3-Clause | SDK `lib/lwip/` |
| **Mbed TLS** (3 modules only) | 3.6.2 | AES-CCM to decrypt the thermometers (MiBeacon); big-number arithmetic for SRP pairing; the RFC 3526 3072-bit prime | Apache-2.0 (dual with GPL-2.0-or-later; Apache chosen) | SDK `lib/mbedtls/` |
| **TinyUSB** | 0.18.0 | USB device stack behind the serial console | MIT | SDK `lib/tinyusb/` |
| **newlib** (libc, libm) | from xPack GCC 14.2.1 | C library: `memcpy`, `snprintf` internals, `malloc` | Mix of BSD-style licences (`COPYING.NEWLIB`) | toolchain |
| **libgcc** | GCC 14.2.1 | Compiler runtime helpers | GPL-3.0 with GCC Runtime Library Exception (no obligations for the firmware) | toolchain |

Compiled but **not** in the firmware (removed by the linker as unused): BTstack's bundled
micro-ecc, rijndael, md5, yxml and SEGGER RTT, and every part of Mbed TLS other than AES,
CCM, cipher glue and bignum (`src/mbedtls_config.h` enables only those).

### What the licences mean for this project

- **Raspberry Pi hardware only.** cyw43-driver and BTstack are licensed through Raspberry
  Pi for use on their chips and boards. The firmware may be shared and used freely on a
  Pico W (and Pico 2 W family), which is the only hardware it runs on anyway; the code of
  those two components may not be reused on other microcontrollers under these licences.
- **Binary redistribution needs the notices.** Sharing `pico2homekit.uf2` requires shipping
  the copyright notices and licence texts of the components above (BSD/MIT/Apache terms).
  The files to include: Pico SDK `LICENSE.TXT`, `lib/cyw43-driver/LICENSE.RP`,
  `src/rp2_common/pico_btstack/LICENSE.RP` (the Raspberry Pi BTstack grant) together with
  `lib/btstack/LICENSE`, `lib/lwip/COPYING`, `lib/mbedtls/LICENSE`, `lib/tinyusb/LICENSE`,
  `third_party/monocypher/LICENCE.md`, and newlib's `COPYING.NEWLIB`.
- **This project's own code has no licence yet.** Until one is chosen, others may not legally
  reuse or modify `src/`, `tools/` and `tests/`.

## Only for development and testing (not in the firmware)

| Component | Used by | Licence |
|---|---|---|
| xPack GNU Arm Embedded GCC 14.2.1, CMake 3.31.6, Ninja 1.12.1 | Building | GPL-3.0 (GCC), BSD-3-Clause (CMake), Apache-2.0 (Ninja) |
| picotool (built by the SDK during the build) | Creating `.uf2` files | BSD-3-Clause |
| Python `cryptography` package (installed into `build-host/venv` by `make hap-test`) | `tools/hap_client.py`, the independent HomeKit controller used in tests | Apache-2.0 or BSD-3-Clause |
| macOS Foundation and CoreBluetooth frameworks | `tools/mac_ble.m` (reading thermometers from a Mac) | Apple SDK |
| Test vector from **xiaomi-ble** (Bluetooth-Devices/xiaomi-ble) | `tests/test_mibeacon.c`: one captured LYWSD03MMC packet and its published bind key | MIT |

No other third-party code is copied into this repository. `pico_sdk_import.cmake` is the
standard helper file from the Pico SDK (BSD-3-Clause).

## Re-running the audit

```bash
make
grep -c 'third_party\|/lib/' build/pico2homekit.elf.map   # what was offered to the linker
~/.pico-sdk/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin/arm-none-eabi-nm -S --size-sort build/pico2homekit.elf | tail -20
```

Library versions: `git -C ~/.pico-sdk/sdk/2.2.0/lib/<name> log -1`, or the version headers
(`lwip/src/include/lwip/init.h`, `mbedtls/include/mbedtls/build_info.h`,
`tinyusb/src/tusb_option.h`, `btstack/CHANGELOG.md`).
