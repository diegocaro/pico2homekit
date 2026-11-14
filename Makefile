# Convenience wrapper. The toolchain lives in ~/.pico-sdk (see README).
PICO_ROOT ?= $(HOME)/.pico-sdk
export PICO_SDK_PATH       ?= $(PICO_ROOT)/sdk/2.2.0
export PICO_TOOLCHAIN_PATH ?= $(PICO_ROOT)/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1
CMAKE := $(PICO_ROOT)/cmake/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake
NINJA := $(PICO_ROOT)/ninja/ninja

.DEFAULT_GOAL := all
.PHONY: all flash monitor clean test mac-ble

# Host-side unit tests (Apple clang), using the SDK's mbedTLS sources.
MBEDTLS := $(PICO_SDK_PATH)/lib/mbedtls
test:
	@mkdir -p build-host
	cc -O1 -Wall -Wextra -Isrc -I$(MBEDTLS)/include -I$(PICO_SDK_PATH)/src/rp2_common/pico_mbedtls/include \
	  -DMBEDTLS_CONFIG_FILE='"pico_mbedtls_config.h"' \
	  tests/test_mibeacon.c src/mibeacon.c \
	  $(MBEDTLS)/library/aes.c $(MBEDTLS)/library/aesni.c $(MBEDTLS)/library/aesce.c $(MBEDTLS)/library/ccm.c $(MBEDTLS)/library/cipher.c \
	  $(MBEDTLS)/library/cipher_wrap.c $(MBEDTLS)/library/platform_util.c $(MBEDTLS)/library/constant_time.c \
	  -o build-host/test_mibeacon
	./build-host/test_mibeacon

# Read the thermometers with the Mac's own Bluetooth (no Pico). `make mac-ble` builds and runs it.
# macOS kills Bluetooth programs without a usage description, so a minimal Info.plist is embedded.
MAC_BLE_SRC := tools/mac_ble.m src/mibeacon.c src/sensors.c \
  $(MBEDTLS)/library/aes.c $(MBEDTLS)/library/aesni.c $(MBEDTLS)/library/aesce.c $(MBEDTLS)/library/ccm.c \
  $(MBEDTLS)/library/cipher.c $(MBEDTLS)/library/cipher_wrap.c $(MBEDTLS)/library/platform_util.c \
  $(MBEDTLS)/library/constant_time.c
build-host/mac_ble: $(MAC_BLE_SRC) src/mibeacon.h src/sensors.h config/config.h
	@mkdir -p build-host
	@printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>local.pico2homekit.mac-ble</string><key>NSBluetoothAlwaysUsageDescription</key><string>Read Xiaomi thermometers</string></dict></plist>' > build-host/Info.plist
	cc -fobjc-arc -O1 -Isrc -Iconfig -sectcreate __TEXT __info_plist build-host/Info.plist \
	  -I$(MBEDTLS)/include -I$(PICO_SDK_PATH)/src/rp2_common/pico_mbedtls/include \
	  -DMBEDTLS_CONFIG_FILE='"pico_mbedtls_config.h"' \
	  $(MAC_BLE_SRC) -framework Foundation -framework CoreBluetooth -o $@

mac-ble: build-host/mac_ble
	./build-host/mac_ble

all: build/build.ninja
	$(CMAKE) --build build

build/build.ninja: CMakeLists.txt
	$(CMAKE) -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=$(NINJA) -DCMAKE_BUILD_TYPE=Release

# If our firmware is running, opening its serial port at 1200 baud reboots it
# into BOOTSEL. Otherwise hold BOOTSEL while plugging the Pico W in.
flash: all
	@if [ ! -d /Volumes/RPI-RP2 ]; then \
	  port=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); \
	  if [ -z "$$port" ]; then echo "Pico not found: hold BOOTSEL while plugging it in"; exit 1; fi; \
	  echo "Rebooting $$port into BOOTSEL..."; \
	  stty -f $$port 1200 2>/dev/null || true; \
	  for i in $$(seq 1 40); do [ -d /Volumes/RPI-RP2 ] && break; sleep 0.25; done; \
	  [ -d /Volumes/RPI-RP2 ] || { echo "BOOTSEL drive did not appear: hold BOOTSEL while plugging it in"; exit 1; }; \
	fi
	@# the drive can show up a moment before it is writable
	@for i in $$(seq 1 20); do cp build/pico2homekit.uf2 /Volumes/RPI-RP2/ 2>/dev/null && exit 0; sleep 0.5; done; \
	  cp build/pico2homekit.uf2 /Volumes/RPI-RP2/
	@echo "Flashed. Run 'make monitor' to watch the log."

# Waits for the Pico's serial port. Quit screen with Ctrl-A then K.
monitor:
	@for i in $$(seq 1 40); do ls /dev/tty.usbmodem* >/dev/null 2>&1 && break; sleep 0.25; done
	@port=$$(ls /dev/tty.usbmodem* 2>/dev/null | head -1); \
	  if [ -z "$$port" ]; then echo "No Pico serial port found (still in BOOTSEL, or not plugged in?)"; exit 1; fi; \
	  screen $$port 115200

clean:
	$(CMAKE) -E rm -rf build
