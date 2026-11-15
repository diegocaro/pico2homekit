# Security

## Threat model

- Anyone on the same Wi-Fi network can connect to the HomeKit port (TCP 5001) and send anything.
- Anyone in Bluetooth range can broadcast fake thermometer advertisements.
- Someone with physical access to the Pico (USB cable, or a copy of its flash) is out of scope.

## Protections

- **Strict HTTP parsing**: `Content-Length` must be digits only, with hard size caps, and an
  encrypted session never accepts plaintext that arrives after Pair Verify.
- **Pair Setup rate limit**: 1, 2, 4 … up to 60 s between setup-code attempts, reset on
  success.
- **Connection slots**: an idle or half-open connection cannot hold one of the 8 slots. When
  they are full, the oldest connection that never completed Pair Verify is closed for the
  newcomer, and all such connections are closed after 60 s.
- **Thermometers**: a thermometer with a bind key is only accepted encrypted and
  authenticated; its authenticated packet counter must increase (replays are dropped); NaN or
  out-of-range values are ignored.
- **Keys**: generated from a SHA-512 entropy pool (ring-oscillator noise, the SDK's generator
  and a timer) expanded with ChaCha20; key material is wiped from memory after use. The
  HomeKit store read from flash is sanity-checked before use.

## Testing

```bash
make fuzz
```

builds the HomeKit server and the BLE/TLV parsers with AddressSanitizer and
UndefinedBehaviorSanitizer, then runs the protocol test (`make hap-test` checks, including
request smuggling, backoff and slot exhaustion), 2 million random parser inputs and a minute of
malformed network traffic (`tests/fuzz_hap.py`). It takes about 1.5 minutes.

## Known limits

- The setup code is printed on the USB serial console (physical access).
- A flash dump of the Pico allows offline guessing of the 8-digit setup code. This is inherent
  to SRP, which HomeKit requires.
- The Pair Setup backoff restarts after a reboot (the attempt counter is kept in RAM only).
