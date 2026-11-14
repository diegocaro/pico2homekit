#include "bridge.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hap/hap.h"
#include "hap/hap_db.h"
#include "hap/hap_port.h"
#include "sensors.h"

// Readings older than this are reported as "No Response".
#define STALE_MS (60u * 60u * 1000u)
#define LOW_BATTERY_PCT 15

// Instance IDs, identical in every sensor accessory.
enum {
    IID_INFO = 1, IID_IDENTIFY, IID_MANUFACTURER, IID_MODEL, IID_NAME, IID_SERIAL, IID_FIRMWARE,
    IID_PROTO = 8, IID_PROTO_VERSION,          // bridge only
    IID_TEMP_SVC = 8, IID_TEMP,
    IID_HUM_SVC, IID_HUM,
    IID_BATT_SVC, IID_BATT_LEVEL, IID_BATT_CHARGING, IID_BATT_LOW,
};

#define FIRMWARE_VERSION "0.2.0"

static const char *bridge_name;
static bool identify_requested;

// Last values sent as events, to only notify on change.
static struct { int temp10, hum, batt, low; } last_sent[BRIDGE_MAX_SENSORS];

static int nsensors(void) {
    int n = sensors_count();
    return n > BRIDGE_MAX_SENSORS ? BRIDGE_MAX_SENSORS : n;
}

static sensor_t *sensor_for_aid(unsigned aid) {
    if (aid < 2 || (int)aid - 2 >= nsensors()) return NULL;
    return sensors_get((int)aid - 2);
}

static bool fresh(const sensor_t *s) {
    return s->seen && hap_port_millis() - s->last_update_ms < STALE_MS;
}

void bridge_init(const char *name) {
    bridge_name = name;
    for (int i = 0; i < BRIDGE_MAX_SENSORS; i++) last_sent[i].temp10 = last_sent[i].hum = last_sent[i].batt = last_sent[i].low = -1000;
}

bool bridge_take_identify_request(void) {
    bool r = identify_requested;
    identify_requested = false;
    return r;
}

// ---------------------------------------------------------------------------
// /accessories JSON

typedef struct { char *buf; size_t cap, len; bool overflow; } out_t;

static void put(out_t *o, const char *fmt, ...) {
    if (o->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) { o->overflow = true; return; }
    o->len += (size_t)n;
}

// JSON string with escaping of quotes and backslashes.
static void put_str(out_t *o, const char *s) {
    put(o, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') put(o, "\\%c", *s);
        else if ((unsigned char)*s >= 0x20) put(o, "%c", *s);
    }
    put(o, "\"");
}

static void put_info(out_t *o, const char *manufacturer, const char *model, const char *name, const char *serial) {
    put(o, "{\"type\":\"3E\",\"iid\":%d,\"characteristics\":[", IID_INFO);
    put(o, "{\"type\":\"14\",\"iid\":%d,\"perms\":[\"pw\"],\"format\":\"bool\"},", IID_IDENTIFY);
    put(o, "{\"type\":\"20\",\"iid\":%d,\"perms\":[\"pr\"],\"format\":\"string\",\"value\":", IID_MANUFACTURER);
    put_str(o, manufacturer);
    put(o, "},{\"type\":\"21\",\"iid\":%d,\"perms\":[\"pr\"],\"format\":\"string\",\"value\":", IID_MODEL);
    put_str(o, model);
    put(o, "},{\"type\":\"23\",\"iid\":%d,\"perms\":[\"pr\"],\"format\":\"string\",\"value\":", IID_NAME);
    put_str(o, name);
    put(o, "},{\"type\":\"30\",\"iid\":%d,\"perms\":[\"pr\"],\"format\":\"string\",\"value\":", IID_SERIAL);
    put_str(o, serial);
    put(o, "},{\"type\":\"52\",\"iid\":%d,\"perms\":[\"pr\"],\"format\":\"string\",\"value\":\"" FIRMWARE_VERSION "\"}]}",
        IID_FIRMWARE);
}

// HomeKit requires a value for every readable characteristic in /accessories;
// until a sensor reports, use a neutral placeholder (live reads still return
// "No Response" for it).
static void put_value(out_t *o, unsigned aid, unsigned iid, const char *placeholder) {
    char val[16];
    put(o, ",\"value\":%s", hap_db_read(aid, iid, val, sizeof(val)) == HAP_STATUS_OK ? val : placeholder);
}

size_t hap_db_accessories_json(char *buf, size_t cap) {
    out_t o = {buf, cap, 0, false};

    put(&o, "{\"accessories\":[{\"aid\":1,\"services\":[");
    put_info(&o, "pico2homekit", "Pico W Bridge", bridge_name, hap_device_id());
    put(&o, ",{\"type\":\"A2\",\"iid\":%d,\"characteristics\":[{\"type\":\"37\",\"iid\":%d,\"perms\":[\"pr\"],"
            "\"format\":\"string\",\"value\":\"1.1.0\"}]}]}", IID_PROTO, IID_PROTO_VERSION);

    for (int i = 0; i < nsensors(); i++) {
        sensor_t *s = sensors_get(i);
        unsigned aid = (unsigned)i + 2;
        char serial[18];
        snprintf(serial, sizeof(serial), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
        put(&o, ",{\"aid\":%u,\"services\":[", aid);
        put_info(&o, "Xiaomi", "LYWSD03MMC", s->name, serial);

        put(&o, ",{\"type\":\"8A\",\"iid\":%d,\"primary\":true,\"characteristics\":[{\"type\":\"11\",\"iid\":%d,"
                "\"perms\":[\"pr\",\"ev\"],\"format\":\"float\",\"unit\":\"celsius\",\"minValue\":-40,\"maxValue\":100,"
                "\"minStep\":0.1", IID_TEMP_SVC, IID_TEMP);
        put_value(&o, aid, IID_TEMP, "0");
        put(&o, "}]}");

        put(&o, ",{\"type\":\"82\",\"iid\":%d,\"characteristics\":[{\"type\":\"10\",\"iid\":%d,"
                "\"perms\":[\"pr\",\"ev\"],\"format\":\"float\",\"unit\":\"percentage\",\"minValue\":0,\"maxValue\":100,"
                "\"minStep\":1", IID_HUM_SVC, IID_HUM);
        put_value(&o, aid, IID_HUM, "0");
        put(&o, "}]}");

        put(&o, ",{\"type\":\"96\",\"iid\":%d,\"characteristics\":[", IID_BATT_SVC);
        put(&o, "{\"type\":\"68\",\"iid\":%d,\"perms\":[\"pr\",\"ev\"],\"format\":\"uint8\",\"unit\":\"percentage\","
                "\"minValue\":0,\"maxValue\":100,\"minStep\":1", IID_BATT_LEVEL);
        put_value(&o, aid, IID_BATT_LEVEL, "100");
        put(&o, "},{\"type\":\"8F\",\"iid\":%d,\"perms\":[\"pr\",\"ev\"],\"format\":\"uint8\",\"minValue\":0,"
                "\"maxValue\":2,\"minStep\":1,\"value\":2}", IID_BATT_CHARGING);
        put(&o, ",{\"type\":\"79\",\"iid\":%d,\"perms\":[\"pr\",\"ev\"],\"format\":\"uint8\",\"minValue\":0,"
                "\"maxValue\":1,\"minStep\":1", IID_BATT_LOW);
        put_value(&o, aid, IID_BATT_LOW, "0");
        put(&o, "}]}]}");
    }
    put(&o, "]}");
    return o.overflow ? 0 : o.len;
}

// ---------------------------------------------------------------------------
// Reads / writes / events

static int info_read(unsigned aid, unsigned iid, char *buf, size_t cap) {
    out_t o = {buf, cap, 0, false};
    sensor_t *s = aid == 1 ? NULL : sensor_for_aid(aid);
    char serial[18];
    if (s) snprintf(serial, sizeof(serial), "%02X:%02X:%02X:%02X:%02X:%02X",
                    s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
    switch (iid) {
    case IID_IDENTIFY: return HAP_STATUS_WRITE_ONLY;
    case IID_MANUFACTURER: put_str(&o, s ? "Xiaomi" : "pico2homekit"); break;
    case IID_MODEL: put_str(&o, s ? "LYWSD03MMC" : "Pico W Bridge"); break;
    case IID_NAME: put_str(&o, s ? s->name : bridge_name); break;
    case IID_SERIAL: put_str(&o, s ? serial : hap_device_id()); break;
    case IID_FIRMWARE: put_str(&o, FIRMWARE_VERSION); break;
    default:
        if (aid == 1 && iid == IID_PROTO_VERSION) { put_str(&o, "1.1.0"); break; }
        return HAP_STATUS_NOT_FOUND;
    }
    return o.overflow ? HAP_STATUS_INVALID_VALUE : HAP_STATUS_OK;
}

int hap_db_read(unsigned aid, unsigned iid, char *buf, size_t cap) {
    if (aid != 1 && !sensor_for_aid(aid)) return HAP_STATUS_NOT_FOUND;
    if (aid == 1 || iid <= IID_FIRMWARE) return info_read(aid, iid, buf, cap);

    sensor_t *s = sensor_for_aid(aid);
    switch (iid) {
    case IID_TEMP:
        if (!fresh(s) || !s->has_temperature) return HAP_STATUS_COMMUNICATION_FAILURE;
        snprintf(buf, cap, "%.1f", (double)s->temperature_c);
        return HAP_STATUS_OK;
    case IID_HUM:
        if (!fresh(s) || !s->has_humidity) return HAP_STATUS_COMMUNICATION_FAILURE;
        snprintf(buf, cap, "%.0f", (double)roundf(s->humidity_pct));
        return HAP_STATUS_OK;
    case IID_BATT_LEVEL:
        if (!s->has_battery) return HAP_STATUS_COMMUNICATION_FAILURE;
        snprintf(buf, cap, "%u", s->battery_pct);
        return HAP_STATUS_OK;
    case IID_BATT_CHARGING:
        snprintf(buf, cap, "2");   // not chargeable
        return HAP_STATUS_OK;
    case IID_BATT_LOW:
        if (!s->has_battery) return HAP_STATUS_COMMUNICATION_FAILURE;
        snprintf(buf, cap, "%d", s->battery_pct < LOW_BATTERY_PCT);
        return HAP_STATUS_OK;
    default:
        return HAP_STATUS_NOT_FOUND;
    }
}

int hap_db_write(unsigned aid, unsigned iid, const char *value, size_t len) {
    (void)value; (void)len;
    if (aid != 1 && !sensor_for_aid(aid)) return HAP_STATUS_NOT_FOUND;
    if (iid == IID_IDENTIFY) {
        printf("bridge: identify requested (aid %u)\n", aid);
        identify_requested = true;
        return HAP_STATUS_OK;
    }
    if (hap_db_event_slot(aid, iid) == HAP_STATUS_NOT_FOUND && !(iid <= IID_FIRMWARE || (aid == 1 && iid == IID_PROTO_VERSION)))
        return HAP_STATUS_NOT_FOUND;
    return HAP_STATUS_READ_ONLY;
}

int hap_db_event_slot(unsigned aid, unsigned iid) {
    if (aid == 1) return (iid >= 1 && iid <= IID_PROTO_VERSION) ? HAP_STATUS_NO_NOTIFICATION : HAP_STATUS_NOT_FOUND;
    if (!sensor_for_aid(aid)) return HAP_STATUS_NOT_FOUND;
    int base = ((int)aid - 2) * 8;
    switch (iid) {
    case IID_TEMP: return base + 0;
    case IID_HUM: return base + 1;
    case IID_BATT_LEVEL: return base + 2;
    case IID_BATT_CHARGING: return base + 3;
    case IID_BATT_LOW: return base + 4;
    default:
        return (iid >= 1 && iid <= IID_BATT_LOW) ? HAP_STATUS_NO_NOTIFICATION : HAP_STATUS_NOT_FOUND;
    }
}

uint32_t hap_db_layout_hash(void) {
    // FNV-1a over everything that shapes /accessories.
    uint32_t h = 2166136261u;
#define MIX(p, n) for (size_t k_ = 0; k_ < (n); k_++) { h ^= ((const uint8_t *)(p))[k_]; h *= 16777619u; }
    int n = nsensors();
    MIX(&n, sizeof(n));
    MIX(bridge_name, strlen(bridge_name));
    MIX(FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION));
    for (int i = 0; i < n; i++) {
        sensor_t *s = sensors_get(i);
        MIX(s->name, strlen(s->name));
        MIX(s->mac, 6);
    }
#undef MIX
    return h ? h : 1;
}

void bridge_sensor_updated(int i) {
    if (i < 0 || i >= nsensors()) return;
    sensor_t *s = sensors_get(i);
    unsigned aid = (unsigned)i + 2;
    if (s->has_temperature) {
        int t10 = (int)lroundf(s->temperature_c * 10);
        if (t10 != last_sent[i].temp10) { last_sent[i].temp10 = t10; hap_notify(aid, IID_TEMP); }
    }
    if (s->has_humidity) {
        int hum = (int)lroundf(s->humidity_pct);
        if (hum != last_sent[i].hum) { last_sent[i].hum = hum; hap_notify(aid, IID_HUM); }
    }
    if (s->has_battery) {
        if (s->battery_pct != last_sent[i].batt) { last_sent[i].batt = s->battery_pct; hap_notify(aid, IID_BATT_LEVEL); }
        int low = s->battery_pct < LOW_BATTERY_PCT;
        if (low != last_sent[i].low) { last_sent[i].low = low; hap_notify(aid, IID_BATT_LOW); }
    }
}
