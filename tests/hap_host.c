// Runs the HAP core on the computer over TCP (127.0.0.1) with fake sensors,
// so tools/hap_client.py can exercise the whole protocol.
//   build-host/hap_host <port> <state-file>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"
#include "config.h"
#include "hap/hap.h"
#include "hap/hap_port.h"
#include "sensors.h"

typedef struct { int fd; bool closing; hap_conn_t *hap; } client_t;
static client_t clients[HAP_MAX_CONNECTIONS + 1];
static const char *state_path;

void hap_port_random(uint8_t *buf, size_t len) { arc4random_buf(buf, len); }

uint32_t hap_port_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool hap_port_store_load(void *buf, size_t len) {
    FILE *f = fopen(state_path, "rb");
    if (!f) return false;
    bool ok = fread(buf, 1, len, f) == len;
    fclose(f);
    return ok;
}

bool hap_port_store_save(const void *buf, size_t len) {
    FILE *f = fopen(state_path, "wb");
    if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len;
    fclose(f);
    return ok;
}

bool hap_port_send(void *handle, const uint8_t *data, size_t len) {
    client_t *c = handle;
    while (len > 0) {
        ssize_t n = send(c->fd, data, len, 0);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

void hap_port_close(void *handle) { ((client_t *)handle)->closing = true; }

void hap_port_status_changed(void) {
    printf("host: status changed: paired=%d c#=%u\n", hap_is_paired(), (unsigned)hap_config_number());
}

static void drop(client_t *c) {
    hap_conn_free(c->hap);
    close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <port> <state-file>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    state_path = argv[2];
    for (int i = 0; i <= HAP_MAX_CONNECTIONS; i++) clients[i].fd = -1;

    sensors_init();
    bridge_init(HAP_BRIDGE_NAME);
    if (getenv("HAP_DEBUG")) hap_set_debug(true);   // HAP_DEBUG=1 make hap-test
    if (!hap_init(HAP_SETUP_CODE)) return 1;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 8)) { perror("bind/listen"); return 1; }
    printf("host: listening on 127.0.0.1:%s, device id %s, setup code %s\n", argv[1], hap_device_id(), HAP_SETUP_CODE);

    // Sensor 0 gets a reading immediately and a new temperature every 2 s;
    // sensor 1 never reports (exercises "No Response").
    float temp = 21.0f;
    uint32_t next_update = 0;

    for (;;) {
        struct pollfd pfd[HAP_MAX_CONNECTIONS + 2];
        int map[HAP_MAX_CONNECTIONS + 2], n = 0;
        pfd[n] = (struct pollfd){.fd = ls, .events = POLLIN};
        map[n++] = -1;
        for (int i = 0; i <= HAP_MAX_CONNECTIONS; i++)
            if (clients[i].fd >= 0) { pfd[n] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN}; map[n++] = i; }
        poll(pfd, (nfds_t)n, 100);

        for (int k = 0; k < n; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (map[k] < 0) {
                int fd = accept(ls, NULL, NULL);
                if (fd < 0) continue;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                client_t *c = NULL;
                for (int i = 0; i <= HAP_MAX_CONNECTIONS; i++) if (clients[i].fd < 0) { c = &clients[i]; break; }
                if (!c) { close(fd); continue; }
                c->fd = fd;
                c->hap = hap_conn_new(c);
                if (!c->hap) { printf("host: no free HAP slot\n"); drop(c); }
                continue;
            }
            client_t *c = &clients[map[k]];
            uint8_t buf[4096];
            ssize_t r = recv(c->fd, buf, sizeof(buf), 0);
            if (r <= 0) { drop(c); continue; }
            hap_conn_recv(c->hap, buf, (size_t)r);
        }
        for (int i = 0; i <= HAP_MAX_CONNECTIONS; i++)
            if (clients[i].fd >= 0 && clients[i].closing) drop(&clients[i]);

        if ((int32_t)(hap_port_millis() - next_update) >= 0) {
            mibeacon_reading_t r = {.has_temperature = true, .temperature_c = temp,
                                    .has_humidity = true, .humidity_pct = 48.6f,
                                    .has_battery = true, .battery_pct = 12};
            sensors_update(sensors_get(0), &r, hap_port_millis());
            bridge_sensor_updated(0);
            temp += 0.5f;
            next_update = hap_port_millis() + 2000;
        }
    }
}
