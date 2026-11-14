// pico2homekit: Xiaomi LYWSD03MMC thermometers (BLE) -> Apple HomeKit (Wi-Fi).
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "bridge.h"
#include "config.h"
#include "hap/hap.h"
#include "hap/srp.h"
#include "hap_port_pico.h"
#include "mibeacon.h"
#include "sensors.h"

#ifndef HAP_SETUP_CODE
#error "Add HAP_SETUP_CODE (\"XXX-XX-XXX\") to config/config.h, see config/config.example.h"
#endif
#ifndef HAP_BRIDGE_NAME
#define HAP_BRIDGE_NAME "Pico Bridge"
#endif

static btstack_packet_callback_registration_t hci_cb;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void print_mac(const uint8_t *m) {
    printf("%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Print each unconfigured Xiaomi device once, to help find MAC addresses.
static void note_unknown(const uint8_t mac[6]) {
    static uint8_t seen[16][6];
    static int n;
    for (int i = 0; i < n; i++)
        if (memcmp(seen[i], mac, 6) == 0) return;
    if (n < 16) memcpy(seen[n++], mac, 6);
    printf("ble: Xiaomi device not in config: ");
    print_mac(mac);
    printf("\n");
}

static void handle_mibeacon(const uint8_t mac[6], const uint8_t *sd, size_t len) {
    sensor_t *s = sensors_find(mac);
    if (!s) { note_unknown(mac); return; }

    // Each frame is repeated many times; only decrypt new ones.
    int fc = mibeacon_frame_counter(sd, len);
    if (fc < 0 || fc == s->last_frame) return;
    s->last_frame = fc;

    mibeacon_reading_t r;
    mibeacon_result_t res = mibeacon_parse(sd, len, mac, s->key, &r);
    if (res == MIBEACON_NO_DATA) return;
    if (res != MIBEACON_OK) {
        printf("ble: %s: %s\n", s->name, mibeacon_result_str(res));
        return;
    }
    if (sensors_update(s, &r, now_ms())) {
        printf("%-12s", s->name);
        if (s->has_temperature) printf("  %5.1f C", s->temperature_c);
        if (s->has_humidity) printf("  %5.1f %%RH", s->humidity_pct);
        if (s->has_battery) printf("  batt %3u%%", s->battery_pct);
        printf("\n");
        bridge_sensor_updated((int)(s - sensors_get(0)));
    }
    static bool led;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led = !led);
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            // Passive scan, 60 ms interval / 30 ms window (leaves airtime for Wi-Fi).
            gap_set_scan_parameters(0, 0x0060, 0x0030);
            gap_set_scan_duplicate_filter(false);
            gap_start_scan();
            printf("ble: scanning\n");
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT: {
        bd_addr_t addr;
        gap_event_advertising_report_get_address(packet, addr);
        uint8_t len = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        ad_context_t ctx;
        for (ad_iterator_init(&ctx, len, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
            if (ad_iterator_get_data_type(&ctx) != BLUETOOTH_DATA_TYPE_SERVICE_DATA_16_BIT_UUID) continue;
            const uint8_t *d = ad_iterator_get_data(&ctx);
            uint8_t dlen = ad_iterator_get_data_len(&ctx);
            if (dlen >= 2 && little_endian_read_16(d, 0) == MIBEACON_UUID)
                handle_mibeacon(addr, d + 2, dlen - 2);
        }
        break;
    }
    default:
        break;
    }
}

static const char *link_str(int st) {
    switch (st) {
    case CYW43_LINK_DOWN: return "down";
    case CYW43_LINK_JOIN: return "joined, waiting for DHCP";
    case CYW43_LINK_NOIP: return "joined, no IP";
    case CYW43_LINK_UP: return "up";
    case CYW43_LINK_FAIL: return "failed";
    case CYW43_LINK_NONET: return "network not found (2.4 GHz? in range?)";
    case CYW43_LINK_BADAUTH: return "authentication failed (wrong password?)";
    }
    return "?";
}

static void wifi_task(void) {
    static int last = -100;
    static uint32_t next_try, attempt_start;
    static bool trying;
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    int wl = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA); // raw driver state

    if (st != last) {
        last = st;
        if (st == CYW43_LINK_UP) {
            trying = false;
            printf("wifi: connected, IP %s\n", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
            // Power saving adds hundreds of ms of latency and drops mDNS multicast.
            cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 20, 1, 1, 1));
            hap_server_start(HAP_BRIDGE_NAME);
        } else {
            printf("wifi: %s (link %d, driver %d)\n", link_str(st), st, wl);
            if (st == CYW43_LINK_FAIL || st == CYW43_LINK_NONET || st == CYW43_LINK_BADAUTH) {
                trying = false;
                next_try = now_ms() + 5000;
            }
        }
    }

    // An attempt that hangs (no IP after 30 s) is abandoned and restarted.
    if (trying && st != CYW43_LINK_UP && now_ms() - attempt_start > 30000) {
        printf("wifi: no connection after 30 s (link %d, driver %d), restarting\n", st, wl);
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        trying = false;
        next_try = now_ms() + 2000;
    }

    if (!trying && st != CYW43_LINK_UP && (int32_t)(now_ms() - next_try) >= 0) {
        // WPA2/WPA3-transition first (routers in transition mode hang on plain
        // WPA2), then plain WPA2 for older routers; alternate until one works.
        static const struct { uint32_t auth; const char *name; } modes[] = {
            {CYW43_AUTH_WPA3_WPA2_AES_PSK, "WPA2/WPA3"},
            {CYW43_AUTH_WPA2_AES_PSK, "WPA2"},
        };
        static unsigned attempt;
        unsigned m = attempt++ % 2;
        printf("wifi: connecting to '%s' (%s)\n", WIFI_SSID, modes[m].name);
        int rc = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, modes[m].auth);
        if (rc) printf("wifi: connect call failed (%d)\n", rc);
        trying = true;
        attempt_start = now_ms();
    }
}

static void print_status(void) {
    printf("homekit: '%s', setup code %s, device id %s, %s (%d pairing%s, %d open session%s), config #%u\n",
           HAP_BRIDGE_NAME, HAP_SETUP_CODE, hap_device_id(), hap_is_paired() ? "paired" : "NOT paired",
           hap_pairing_count(), hap_pairing_count() == 1 ? "" : "s",
           hap_secure_connection_count(), hap_secure_connection_count() == 1 ? "" : "s",
           (unsigned)hap_config_number());
}

// Commands typed into the serial monitor.
static void serial_task(void) {
    static char line[32];
    static size_t n;
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch != '\r' && ch != '\n') {
            if (n < sizeof(line) - 1) line[n++] = (char)ch;
            continue;
        }
        line[n] = '\0';
        n = 0;
        if (strcmp(line, "status") == 0) {
            print_status();
        } else if (strcmp(line, "unpair") == 0) {
            hap_remove_all_pairings();
            printf("homekit: all pairings removed; add the bridge again in the Home app\n");
        } else if (strcmp(line, "debug") == 0) {
            hap_set_debug(!hap_debug_enabled());
            printf("homekit: debug logging %s\n", hap_debug_enabled() ? "ON" : "off");
        } else if (strcmp(line, "selftest") == 0) {
            srp_selftest();
        } else if (strcmp(line, "reset") == 0) {
            hap_factory_reset();
            printf("homekit: factory reset done (new device id %s)\n", hap_device_id());
        } else if (line[0]) {
            printf("commands: status | debug (toggle protocol log) | selftest (SRP check, ~12 s) |\n"
                   "          unpair (forget all controllers) | reset (new HomeKit identity)\n");
        }
    }
}

// Blink the LED for 3 s when HomeKit asks the accessory to identify itself.
static void identify_task(void) {
    static uint32_t until, next_toggle;
    static bool on;
    if (bridge_take_identify_request()) until = now_ms() + 3000;
    if (until && (int32_t)(now_ms() - next_toggle) >= 0) {
        if ((int32_t)(now_ms() - until) >= 0) { until = 0; on = false; }
        else on = !on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        next_toggle = now_ms() + 150;
    }
}

int main(void) {
    stdio_init_all();
    // Give a serial monitor up to 3 s to attach so the boot log is not lost.
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) sleep_ms(10);
    sleep_ms(200);
    printf("\npico2homekit starting\n");

    if (!sensors_init()) {
        printf("fix config/config.h and reflash\n");
        while (true) sleep_ms(1000);
    }
    for (int i = 0; i < sensors_count(); i++) {
        printf("sensor %d: %s ", i, sensors_get(i)->name);
        print_mac(sensors_get(i)->mac);
        printf("\n");
    }
    if (sensors_count() > BRIDGE_MAX_SENSORS)
        printf("homekit: only the first %d sensors are exposed\n", BRIDGE_MAX_SENSORS);

    bridge_init(HAP_BRIDGE_NAME);
    if (!hap_init(HAP_SETUP_CODE)) {
        printf("fix HAP_SETUP_CODE in config/config.h and reflash\n");
        while (true) sleep_ms(1000);
    }
    print_status();

    if (cyw43_arch_init()) {
        printf("cyw43 init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    hci_cb.callback = &packet_handler;
    hci_add_event_handler(&hci_cb);

    // Bluetooth starts once Wi-Fi is up (or after 90 s regardless), so BLE
    // scanning never competes with the Wi-Fi handshake for the shared radio.
    bool ble_on = false;
    while (true) {
        cyw43_arch_poll();
        wifi_task();
        hap_server_poll();
        serial_task();
        identify_task();
        if (!ble_on && (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ||
                        now_ms() > 90000)) {
            hci_power_control(HCI_POWER_ON);
            ble_on = true;
        }
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(10));
    }
}
