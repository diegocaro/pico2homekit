# Convenience wrapper. The toolchain lives in ~/.pico-sdk (see README).
PICO_ROOT ?= $(HOME)/.pico-sdk
export PICO_SDK_PATH       ?= $(PICO_ROOT)/sdk/2.2.0
export PICO_TOOLCHAIN_PATH ?= $(PICO_ROOT)/toolchain/xpack-arm-none-eabi-gcc-14.2.1-1.1
CMAKE := $(PICO_ROOT)/cmake/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake
NINJA := $(PICO_ROOT)/ninja/ninja

.DEFAULT_GOAL := all
.PHONY: all flash monitor log clean test hap-test mac-ble fuzz

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

# HAP server core running on this computer (127.0.0.1) with fake sensors, plus an
# independent Python HomeKit controller that pairs with it and checks every endpoint.
MONO := third_party/monocypher
HAP_SRC := src/hap/hap.c src/hap/srp.c src/hap/srp_selftest.c src/hap/tlv8.c src/bridge.c src/sensors.c src/mibeacon.c \
  $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c \
  $(MBEDTLS)/library/bignum.c $(MBEDTLS)/library/bignum_core.c $(MBEDTLS)/library/constant_time.c \
  $(MBEDTLS)/library/platform_util.c $(MBEDTLS)/library/aes.c $(MBEDTLS)/library/aesni.c \
  $(MBEDTLS)/library/aesce.c $(MBEDTLS)/library/ccm.c $(MBEDTLS)/library/cipher.c $(MBEDTLS)/library/cipher_wrap.c
build-host/hap_host: $(HAP_SRC) tests/hap_host.c src/hap/*.h src/*.h tests/host_config/config.h
	@mkdir -p build-host
	cc -O2 -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/host_config -I$(MONO) -I$(MBEDTLS)/include \
	  -I$(PICO_SDK_PATH)/src/rp2_common/pico_mbedtls/include -DMBEDTLS_CONFIG_FILE='"pico_mbedtls_config.h"' \
	  tests/hap_host.c $(HAP_SRC) -o $@

build-host/venv:
	python3 -m venv build-host/venv && build-host/venv/bin/pip -q install cryptography

hap-test: build-host/hap_host build-host/venv
	build-host/venv/bin/python tools/hap_client.py --spawn build-host/hap_host

# Security testing: the same server and the BLE/TLV parsers built with AddressSanitizer
# and UndefinedBehaviorSanitizer, fed malformed and random input (about 1.5 minutes).
SAN := -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
build-host/hap_host_asan: $(HAP_SRC) tests/hap_host.c src/hap/*.h src/*.h tests/host_config/config.h
	@mkdir -p build-host
	cc $(SAN) -Wall -Wno-unused-parameter -Isrc -Itests/host_config -I$(MONO) -I$(MBEDTLS)/include \
	  -I$(PICO_SDK_PATH)/src/rp2_common/pico_mbedtls/include -DMBEDTLS_CONFIG_FILE='"pico_mbedtls_config.h"' \
	  tests/hap_host.c $(HAP_SRC) -o $@
build-host/fuzz_parsers: tests/fuzz_parsers.c src/mibeacon.c src/hap/tlv8.c
	@mkdir -p build-host
	cc $(SAN) -Isrc -I$(MBEDTLS)/include -I$(PICO_SDK_PATH)/src/rp2_common/pico_mbedtls/include \
	  -DMBEDTLS_CONFIG_FILE='"pico_mbedtls_config.h"' tests/fuzz_parsers.c src/mibeacon.c src/hap/tlv8.c \
	  $(MBEDTLS)/library/aes.c $(MBEDTLS)/library/aesni.c $(MBEDTLS)/library/aesce.c $(MBEDTLS)/library/ccm.c \
	  $(MBEDTLS)/library/cipher.c $(MBEDTLS)/library/cipher_wrap.c $(MBEDTLS)/library/platform_util.c \
	  $(MBEDTLS)/library/constant_time.c -o $@
fuzz: build-host/hap_host_asan build-host/fuzz_parsers build-host/venv
	./build-host/fuzz_parsers 2000000
	ASAN_OPTIONS=detect_leaks=0 build-host/venv/bin/python tools/hap_client.py --spawn build-host/hap_host_asan
	python3 tests/fuzz_hap.py build-host/hap_host_asan 60

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

# Read-only serial log: no keyboard input, keeps going when the Pico reboots or is
# replugged. Stop with Ctrl-C.
log:
	@echo "waiting for the Pico (Ctrl-C to stop)"; \
	  while true; do \
	    port=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); \
	    if [ -n "$$port" ]; then \
	      echo "--- connected $$port $$(date +%T)"; \
	      stty -f $$port raw -echo 2>/dev/null; \
	      cat $$port 2>/dev/null; \
	      echo "--- disconnected $$(date +%T)"; \
	    fi; \
	    sleep 0.5; \
	  done

clean:
	$(CMAKE) -E rm -rf build
