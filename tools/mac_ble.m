// Print Xiaomi thermometer readings using the Mac's own Bluetooth.
// Reuses src/mibeacon.c, src/sensors.c and config/config.h. Build/run: see the end of this file.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>

#include "mibeacon.h"
#include "sensors.h"

@interface Reader : NSObject <CBCentralManagerDelegate>
@property(strong) CBCentralManager *cm;
@property(strong) NSMutableDictionary<NSUUID *, NSData *> *macs; // macOS hides MACs; learn them from frames
@end

@implementation Reader
- (instancetype)init {
    if ((self = [super init])) {
        _macs = [NSMutableDictionary new];
        _cm = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
    }
    return self;
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)cm {
    if (cm.state == CBManagerStatePoweredOn) {
        [cm scanForPeripheralsWithServices:nil
                                   options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @YES}];
        printf("scanning...\n");
    } else if (cm.state == CBManagerStateUnauthorized) {
        printf("no Bluetooth permission: System Settings > Privacy & Security > Bluetooth\n");
        exit(1);
    } else if (cm.state == CBManagerStatePoweredOff) {
        printf("Bluetooth is off\n");
    }
}

- (void)centralManager:(CBCentralManager *)cm
    didDiscoverPeripheral:(CBPeripheral *)p
        advertisementData:(NSDictionary *)ad
                     RSSI:(NSNumber *)rssi {
    NSData *d = ad[CBAdvertisementDataServiceDataKey][[CBUUID UUIDWithString:@"FE95"]];
    if (!d) return;
    const uint8_t *sd = d.bytes;
    size_t len = d.length;

    uint8_t mac[6];
    if (mibeacon_frame_mac(sd, len, mac)) self.macs[p.identifier] = [NSData dataWithBytes:mac length:6];
    NSData *known = self.macs[p.identifier];
    if (!known) return; // MAC not learned yet; the device includes it in most frames
    memcpy(mac, known.bytes, 6);

    sensor_t *s = sensors_find(mac);
    if (!s) return;
    int fc = mibeacon_frame_counter(sd, len);
    if (fc < 0 || fc == s->last_frame) return;
    s->last_frame = fc;

    mibeacon_reading_t r;
    mibeacon_result_t res = mibeacon_parse(sd, len, mac, s->key, &r);
    if (res == MIBEACON_NO_DATA) return;
    if (res != MIBEACON_OK) {
        printf("%s: %s\n", s->name, mibeacon_result_str(res));
        return;
    }
    sensors_update(s, &r, 0);
    printf("%-10s", s->name);
    if (s->has_temperature) printf("  %5.1f C", s->temperature_c);
    if (s->has_humidity) printf("  %5.1f %%RH", s->humidity_pct);
    if (s->has_battery) printf("  batt %3u%%", s->battery_pct);
    printf("   (rssi %d)\n", rssi.intValue);
    fflush(stdout);
}
@end

// macOS checks Bluetooth permission against the app that launched us (Terminal, an IDE, ...).
// Re-exec ourselves as our own "responsible" process so the embedded Info.plist is used instead.
extern int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t *attrs, int disclaim);
extern char **environ;

static void become_responsible(char **argv) {
    if (getenv("MAC_BLE_DISCLAIMED")) return;
    setenv("MAC_BLE_DISCLAIMED", "1", 1);
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC); // replace this process, keep stdio
    responsibility_spawnattrs_setdisclaim(&attr, 1);
    posix_spawn(NULL, path, NULL, &attr, argv, environ);
    perror("posix_spawn"); // only reached on failure; carry on and hope for the best
}

int main(int argc, char **argv) {
    (void)argc;
    become_responsible(argv);
    @autoreleasepool {
        if (!sensors_init()) return 1;
        Reader *reader = [Reader new];
        (void)reader;
        dispatch_main();
    }
}

/*
Build (from the project root):

  M=~/.pico-sdk/sdk/2.2.0/lib/mbedtls/library
  mkdir -p build-host
  # macOS kills Bluetooth programs that don't say why they need it, so embed a minimal Info.plist.
  printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>local.pico2homekit.mac-ble</string><key>NSBluetoothAlwaysUsageDescription</key><string>Read Xiaomi thermometers</string></dict></plist>' >| build-host/Info.plist
  cc -fobjc-arc -O1 -Isrc -Iconfig -sectcreate __TEXT __info_plist build-host/Info.plist \
    -I$M/../include -I$HOME/.pico-sdk/sdk/2.2.0/src/rp2_common/pico_mbedtls/include \
    -DMBEDTLS_CONFIG_FILE='"pico_mbedtls_config.h"' \
    tools/mac_ble.m src/mibeacon.c src/sensors.c \
    $M/aes.c $M/aesni.c $M/aesce.c $M/ccm.c $M/cipher.c $M/cipher_wrap.c $M/platform_util.c $M/constant_time.c \
    -framework Foundation -framework CoreBluetooth -o build-host/mac_ble

Run:

  ./build-host/mac_ble        (Ctrl-C to stop)
*/
