# How the Mac thermometer reader works

`tools/mac_ble.m` is a small Objective-C command-line program. It listens to the
Xiaomi LYWSD03MMC thermometers with the Mac's built-in Bluetooth and prints their decrypted
readings. It uses the same C code as the Pico firmware (`src/mibeacon.c`, `src/sensors.c`)
and the same `config/config.h`, so it's also a quick way to check your MACs and bind keys
without flashing anything.

Build and run it with `/usr/bin/make mac-ble` (see [README.md](README.md)).

## 1. The thermometers only broadcast

The thermometers never need a connection. Every few seconds each one sends a BLE
*advertisement*: a small packet (up to 31 bytes) that any nearby device can hear. Xiaomi's
format for these packets is called **MiBeacon**. It goes in the advertisement's
*service data* field, tagged with the 16-bit UUID `0xFE95`.

Each advertisement carries **one** measurement: temperature, humidity or battery. That's
why a sensor's line fills in over time.

## 2. Scanning with CoreBluetooth

On macOS, Bluetooth goes through Apple's CoreBluetooth framework. The program:

1. Creates a `CBCentralManager` (the "central" role, which listens or connects).
2. Waits for `centralManagerDidUpdateState:` to report `PoweredOn`.
3. Calls `scanForPeripheralsWithServices:nil` with
   `CBCentralManagerScanOptionAllowDuplicatesKey = YES`. Without that option, macOS reports
   each device only once, and the program would miss every later reading.
4. Gets `centralManager:didDiscoverPeripheral:advertisementData:RSSI:` for every advertisement
   heard. It takes the service data for UUID `FE95` and ignores everything else.

`main()` then calls `dispatch_main()`, which keeps the process alive and handles these callbacks
(all on the main queue). The program never exits on its own: stop it with Ctrl-C.

## 3. macOS hides Bluetooth addresses

On the Pico, BTstack gives us each sender's address (MAC). CoreBluetooth doesn't: for privacy
it only gives a random per-Mac `NSUUID` for each device. The MAC is still needed, because
decryption uses it (see below) and `config.h` identifies each sensor by MAC.

The trick is that most MiBeacon frames include the device's MAC inside the payload (when the
frame-control flag `0x0010` is set). `mibeacon_frame_mac()` reads it from there.
The program keeps a small dictionary `NSUUID → MAC`:

- frame contains a MAC → remember it for that UUID;
- frame without a MAC → look the UUID up; if it isn't known yet, skip the frame until a frame with the MAC arrives.

After that, the MAC is matched against `SENSOR_LIST` in `config.h` with `sensors_find()`.

## 4. Skipping repeats

Each frame is re-broadcast many times. A MiBeacon frame has a 1-byte frame counter that changes
with every new measurement. The program remembers the last counter per sensor and only
decrypts frames with a new one.

## 5. Decryption (shared with the Pico)

`mibeacon_parse()` in `src/mibeacon.c` does the real work, identically on Mac and Pico.

A MiBeacon v5 frame looks like this (offsets into the service data, after `0xFE95`):

```
0-1   frame control   flags: encrypted 0x08, has MAC 0x10, has capability 0x20, has object 0x40; version in top 4 bits
2-3   product id      0x055B = LYWSD03MMC
4     frame counter
5-10  MAC             (if flagged), least-significant byte first
...   capability      (if flagged)
...   ciphertext      the encrypted measurement
-7    ext counter     3 bytes
-4    MIC             4-byte authentication tag
```

The measurement is encrypted with **AES-128-CCM** (via mbedTLS) using the sensor's bind key:

- **nonce** (12 bytes) = MAC (LSB first) + product id + frame counter + ext counter;
- **AAD** = the single byte `0x11`;
- **tag** = the 4-byte MIC.

If the key is wrong, the tag check fails and you get `decryption failed (wrong key?)`.
Garbage is never printed.

The decrypted payload is a list of objects `[type: 2][length: 1][value]`:

| Type | Meaning | Value |
|---|---|---|
| `0x1004` | temperature | int16 ÷ 10 → °C |
| `0x1006` | humidity | uint16 ÷ 10 → % |
| `0x100A` | battery | uint8 % |
| `0x100D` | temperature + humidity | int16 ÷ 10, uint16 ÷ 10 |

`sensors_update()` merges each object into the sensor's latest state, and the program prints
everything known so far for that sensor, plus the signal strength (RSSI, in dBm; closer to 0 is
stronger).

## 6. Bluetooth permission (the macOS quirks)

Two things are needed before macOS lets a command-line program use Bluetooth:

1. **A usage description.** Programs that use Bluetooth must declare
   `NSBluetoothAlwaysUsageDescription` in an Info.plist; otherwise macOS kills them
   (exit code 134, "attempted to access privacy-sensitive data without a usage description").
   A plain binary has no app bundle, so the Makefile writes a tiny `build-host/Info.plist` and
   the linker embeds it in the binary with `-sectcreate __TEXT __info_plist`.

2. **Being "responsible" for itself.** macOS checks permissions against the *responsible*
   process: normally the app that launched you (Terminal, VS Code, the Claude app...). If that
   app doesn't declare Bluetooth use itself, the program is killed even though its own plist is fine.
   `become_responsible()` re-launches the program in place (`posix_spawn` with
   `POSIX_SPAWN_SETEXEC`, which replaces the process and keeps its output) and calls
   `responsibility_spawnattrs_setdisclaim()`, so the program is responsible for itself.
   An environment variable (`MAC_BLE_DISCLAIMED`) stops it from doing this twice. macOS
   then shows its own "mac_ble would like to use Bluetooth" prompt. You can manage that
   permission in System Settings → Privacy & Security → Bluetooth.

   `responsibility_spawnattrs_setdisclaim` is an undocumented macOS function (other tools use
   it too). If a future macOS update breaks it, the program still works from Terminal.app,
   which declares Bluetooth use itself.

## Files

| File | Role |
|---|---|
| `tools/mac_ble.m` | CoreBluetooth scanning, UUID → MAC mapping, printing, permission handling |
| `src/mibeacon.c` / `.h` | MiBeacon parsing and AES-CCM decryption (shared with the Pico) |
| `src/sensors.c` / `.h` | reads `SENSOR_LIST` from `config.h`, stores latest readings (shared) |
| `config/config.h` | your sensors' names, MACs and bind keys (git-ignored) |
| `Makefile` (`mac-ble`) | builds with Apple clang + the Pico SDK's mbedTLS sources, embeds the Info.plist |
