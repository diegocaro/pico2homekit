# Configuration

All settings live in `config/config.h`, which is compiled into the firmware. It is git-ignored,
so your Wi-Fi password and bind keys never end up in a commit. Start from the example:

```bash
cp config/config.example.h config/config.h
```

A filled-in file looks like this:

```c
#define WIFI_SSID     "my-wifi"
#define WIFI_PASSWORD "my-password"

#define HAP_SETUP_CODE  "531-27-864"
#define HAP_BRIDGE_NAME "Pico Bridge"

#define SENSOR_LIST \
    { "Living Room", "A4:C1:38:00:00:01", "00112233445566778899aabbccddeeff" }, \
    { "Bedroom",     "A4:C1:38:00:00:02", "00112233445566778899aabbccddeeff" },
```

| Setting | What to put |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | your 2.4 GHz network; WPA2 and WPA2/WPA3 mixed mode work |
| `HAP_SETUP_CODE` | the 8-digit code you type in the Home app, as `XXX-XX-XXX`. Pick your own: all-same-digit codes, `123-45-678` and `876-54-321` are refused |
| `HAP_BRIDGE_NAME` | the name shown when adding the bridge in Home |
| `SENSOR_LIST` | one `{ name, MAC, bind key }` line per thermometer, each line ending in `, \` except the last |

In `SENSOR_LIST`:
- **name**: what Home calls the thermometer when it is first added (you can rename it in Home).
- **MAC**: as printed by the token extractor; `:` or `-` separators, any letter case.
- **bind key**: the 32 hex characters of the BLE key.
- Only the first 8 thermometers are exposed to Home.

After any change, run `make flash` again. What a change does:
- **New setup code**: existing pairings stay; only new pairings need the new code. The next boot
  takes about 15 s longer.
- **Add a thermometer**: add it at the **end** of `SENSOR_LIST`; it appears in Home on its own.
- **Remove or reorder thermometers**: Home identifies each one by its position in the list, so
  the ones that move may swap names and rooms in Home. Fix them in Home, or run `reset` (in `make monitor`) and add
  the bridge again.
- **A typo in a MAC or key**: the Pico prints `config: bad MAC …` or `config: bad bind key …`
  at boot and stops. A key that is valid hex but wrong shows up as a thermometer with no
  values; `make mac-ble` tells you which.

