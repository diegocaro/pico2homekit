# Toolchain setup (macOS)

How to set up a Mac to build this project from scratch. Needs no Homebrew and no sudo.
Everything goes into `~/.pico-sdk`, about 1.6 GB. The `Makefile` expects exactly these paths.

## 1. Xcode Command Line Tools

These give you `git`, `make` and the host C compiler (used for `make test` and to build picotool).

```bash
xcode-select -p || xcode-select --install
```

## 2. Folders

```bash
mkdir -p ~/.pico-sdk/{toolchain,cmake,ninja,sdk}
```

## 3. ARM GCC compiler (xPack 14.2.1)

Use the xPack build. Arm's official macOS builds (13.3, 14.2) fail with
`Library not loaded: /usr/local/opt/zstd/lib/libzstd.1.dylib` unless Homebrew's zstd is installed.

Pick the line for your Mac (`uname -m` prints `arm64` on Apple Silicon or `x86_64` on Intel).

Apple Silicon:

```bash
curl -fL https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v14.2.1-1.1/xpack-arm-none-eabi-gcc-14.2.1-1.1-darwin-arm64.tar.gz | tar -xz -C ~/.pico-sdk/toolchain
```

Intel:

```bash
curl -fL https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v14.2.1-1.1/xpack-arm-none-eabi-gcc-14.2.1-1.1-darwin-x64.tar.gz | tar -xz -C ~/.pico-sdk/toolchain
```

Both unpack to `~/.pico-sdk/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1`. Check it works:

```bash
~/.pico-sdk/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin/arm-none-eabi-gcc --version
```

## 4. CMake 3.31.6 (universal binary)

```bash
curl -fL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-macos-universal.tar.gz | tar -xz -C ~/.pico-sdk/cmake
```

## 5. Ninja 1.12.1 (universal binary)

```bash
curl -fL -o /tmp/ninja-mac.zip https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-mac.zip
```

```bash
unzip -o /tmp/ninja-mac.zip -d ~/.pico-sdk/ninja && rm /tmp/ninja-mac.zip
```

## 6. Pico SDK 2.2.0 (with submodules: BTstack, cyw43-driver, lwIP, mbedTLS, TinyUSB)

```bash
git clone --depth 1 -b 2.2.0 https://github.com/raspberrypi/pico-sdk.git ~/.pico-sdk/sdk/2.2.0
```

```bash
git -C ~/.pico-sdk/sdk/2.2.0 submodule update --init --depth 1
```

## 7. picotool

Nothing to do. On the first build, the SDK downloads and builds picotool into `build/_deps/`.
That first build needs internet access.

## 8. Project config

```bash
cp config/config.example.h config/config.h
```

Then edit `config/config.h` and fill in:
- your Wi-Fi SSID and password (the Pico W only supports 2.4 GHz)
- for each thermometer: its name, MAC address and bind key.

The bind keys came from the Xiaomi Cloud token extractor, saved to `tmp/keys.txt`. Both `tmp/` and `config/config.h` are git-ignored,
so copy them to the other Mac yourself (e.g. AirDrop). Don't commit them.

## 9. Build, test, flash

Run these from the project root. If `make` misbehaves in zsh (`function definition file not found`),
use `/usr/bin/make` as shown.

```bash
/usr/bin/make
```

```bash
/usr/bin/make test
```

Hold BOOTSEL while plugging the Pico W in, then:

```bash
/usr/bin/make flash
```

```bash
/usr/bin/make monitor
```

Quit the monitor with Ctrl-A then K.

## Uninstall

```bash
rm -rf ~/.pico-sdk
```

Also delete `build/` and `build-host/` in the project folder.
