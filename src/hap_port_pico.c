#include "hap_port_pico.h"

#include <stdio.h>
#include <string.h>

#include "hap/hap.h"
#include "hap/hap_port.h"
#include "hardware/flash.h"
#include "hardware/structs/rosc.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "lwip/apps/mdns.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

// ---------------------------------------------------------------------------
// Basics

// Random bytes for keys (Ed25519 identity, SRP, X25519, salts). get_rand_64()
// alone is a fast PRNG, not a cryptographic one, so every call gathers fresh
// entropy (512 ring-oscillator bits, the SDK generator, the µs timer) into a
// SHA-512 pool and expands it with ChaCha20. The pool is replaced each call
// (forward secrecy: a later RAM dump does not reveal earlier outputs).
static uint8_t rng_pool[64];
static uint64_t rng_calls;

static void rng_mix(crypto_sha512_ctx *ctx) {
    uint8_t bits[64] = {0};
    for (int i = 0; i < 512; i++) {
        for (volatile int d = 0; d < 20; d++) {}   // let the oscillator drift between samples
        bits[i / 8] |= (uint8_t)((rosc_hw->randombit & 1u) << (i % 8));
    }
    uint64_t extra[3] = {get_rand_64(), get_rand_64(), time_us_64()};
    crypto_sha512_update(ctx, bits, sizeof(bits));
    crypto_sha512_update(ctx, (const uint8_t *)extra, sizeof(extra));
    crypto_wipe(bits, sizeof(bits));
    crypto_wipe(extra, sizeof(extra));
}

void hap_port_random(uint8_t *buf, size_t len) {
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, rng_pool, sizeof(rng_pool));
    crypto_sha512_update(&ctx, (const uint8_t *)&rng_calls, sizeof(rng_calls));
    rng_mix(&ctx);
    crypto_sha512_final(&ctx, rng_pool);
    // first half keys this output, second half carries over to the next call
    uint8_t nonce[8];
    memcpy(nonce, &rng_calls, sizeof(nonce));
    rng_calls++;
    memset(buf, 0, len);
    crypto_chacha20_djb(buf, buf, len, rng_pool, nonce, 0);
    crypto_sha512(rng_pool, rng_pool, sizeof(rng_pool));  // erase the key just used
    crypto_wipe(&ctx, sizeof(ctx));
}

uint32_t hap_port_millis(void) { return to_ms_since_boot(get_absolute_time()); }

// ---------------------------------------------------------------------------
// Flash storage: two sectors used alternately (A/B), just below the two
// sectors BTstack keeps at the very end of flash. A write only ever erases the
// older copy, so a power cut mid-write never loses the pairing data.

#define STORE_SECTOR_A (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)
#define STORE_SECTOR_B (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)
#define STORE_MAGIC 0x50324853u  // "SH2P"

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} store_hdr_t;

static uint8_t sector_buf[FLASH_SECTOR_SIZE];

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static const store_hdr_t *sector_valid(uint32_t offset, size_t len) {
    const store_hdr_t *h = (const store_hdr_t *)(XIP_BASE + offset);
    if (h->magic != STORE_MAGIC || h->len != len) return NULL;
    if (crc32((const uint8_t *)(h + 1), len) != h->crc) return NULL;
    return h;
}

static uint32_t current_sector(size_t len, const store_hdr_t **out) {
    const store_hdr_t *a = sector_valid(STORE_SECTOR_A, len), *b = sector_valid(STORE_SECTOR_B, len);
    if (a && (!b || (int32_t)(a->seq - b->seq) > 0)) { *out = a; return STORE_SECTOR_A; }
    *out = b;
    return b ? STORE_SECTOR_B : 0;
}

bool hap_port_store_load(void *buf, size_t len) {
    const store_hdr_t *h;
    if (!current_sector(len, &h)) return false;
    memcpy(buf, h + 1, len);
    return true;
}

static uint32_t write_offset;

static void flash_write_cb(void *param) {
    (void)param;
    flash_range_erase(write_offset, FLASH_SECTOR_SIZE);
    flash_range_program(write_offset, sector_buf, FLASH_SECTOR_SIZE);
}

bool hap_port_store_save(const void *buf, size_t len) {
    if (sizeof(store_hdr_t) + len > FLASH_SECTOR_SIZE) return false;
    const store_hdr_t *cur;
    uint32_t cur_off = current_sector(len, &cur);
    write_offset = cur_off == STORE_SECTOR_A ? STORE_SECTOR_B : STORE_SECTOR_A;

    memset(sector_buf, 0xFF, sizeof(sector_buf));
    store_hdr_t *h = (store_hdr_t *)sector_buf;
    h->magic = STORE_MAGIC;
    h->seq = cur ? cur->seq + 1 : 1;
    h->len = (uint32_t)len;
    memcpy(h + 1, buf, len);
    h->crc = crc32((const uint8_t *)(h + 1), len);

    int rc = flash_safe_execute(flash_write_cb, NULL, 500);
    if (rc != PICO_OK) { printf("hap: flash write failed (%d)\n", rc); return false; }
    return sector_valid(write_offset, len) != NULL;
}

// ---------------------------------------------------------------------------
// TCP server

typedef struct {
    bool used;
    bool closing;
    struct tcp_pcb *pcb;
    hap_conn_t *hap;
} pconn_t;

static pconn_t pconns[HAP_MAX_CONNECTIONS];
static struct tcp_pcb *listener;

static void pconn_release(pconn_t *pc) {
    hap_conn_free(pc->hap);
    memset(pc, 0, sizeof(*pc));
}

static void pconn_close_now(pconn_t *pc) {
    if (pc->pcb) {
        tcp_arg(pc->pcb, NULL);
        tcp_recv(pc->pcb, NULL);
        tcp_err(pc->pcb, NULL);
        if (tcp_close(pc->pcb) != ERR_OK) tcp_abort(pc->pcb);
    }
    pconn_release(pc);
}

static void on_err(void *arg, err_t err) {
    pconn_t *pc = arg;
    (void)err;
    if (!pc) return;
    pc->pcb = NULL;  // already freed by lwIP
    pconn_release(pc);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    pconn_t *pc = arg;
    if (!pc) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p || err != ERR_OK) {  // peer closed
        if (p) pbuf_free(p);
        pconn_close_now(pc);
        return ERR_OK;
    }
    for (struct pbuf *q = p; q && !pc->closing; q = q->next) hap_conn_recv(pc->hap, q->payload, q->len);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    pconn_t *pc = NULL;
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++) if (!pconns[i].used) { pc = &pconns[i]; break; }
    if (!pc) {
        // All slots taken: make room by closing the oldest connection that never
        // completed Pair Verify, so idle LAN clients cannot lock controllers out.
        pc = hap_oldest_unverified();
        if (pc) {
            printf("hap: all slots in use, closing the oldest unverified connection\n");
            pconn_close_now(pc);
        }
    }
    hap_conn_t *h = pc ? hap_conn_new(pc) : NULL;
    if (!h) {
        printf("hap: connection refused, all %d slots in use\n", HAP_MAX_CONNECTIONS);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    pc->used = true;
    pc->pcb = pcb;
    pc->hap = h;
    tcp_arg(pcb, pc);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);
    // Detect controllers that vanished without closing (e.g. phone left home).
    pcb->so_options |= SOF_KEEPALIVE;
    pcb->keep_idle = 120000;
    pcb->keep_intvl = 10000;
    pcb->keep_cnt = 3;
    return ERR_OK;
}

bool hap_port_send(void *handle, const uint8_t *data, size_t len) {
    pconn_t *pc = handle;
    if (!pc->pcb || pc->closing) return false;
    if (tcp_sndbuf(pc->pcb) < len) {
        printf("hap: send buffer full (%u < %u), closing connection\n", (unsigned)tcp_sndbuf(pc->pcb), (unsigned)len);
        return false;
    }
    if (tcp_write(pc->pcb, data, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK) return false;
    tcp_output(pc->pcb);
    return true;
}

void hap_port_close(void *handle) { ((pconn_t *)handle)->closing = true; }

void hap_server_poll(void) {
    static uint32_t next_expire;
    if ((int32_t)(hap_port_millis() - next_expire) >= 0) {
        hap_expire_unverified();
        next_expire = hap_port_millis() + 1000;
    }
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++)
        if (pconns[i].used && pconns[i].closing) pconn_close_now(&pconns[i]);
}

// ---------------------------------------------------------------------------
// mDNS: _hap._tcp with the TXT record HomeKit needs

static const char *instance_name;
static struct netif *mdns_netif;

static void add_txt(struct mdns_service *service, const char *fmt, ...) {
    char item[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(item, sizeof(item), fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof(item)) mdns_resp_add_service_txtitem(service, item, (u8_t)n);
}

static void hap_txt(struct mdns_service *service, void *userdata) {
    (void)userdata;
    add_txt(service, "c#=%u", (unsigned)hap_config_number());
    add_txt(service, "ff=0");
    add_txt(service, "id=%s", hap_device_id());
    add_txt(service, "md=%s", instance_name);
    add_txt(service, "pv=1.1");
    add_txt(service, "s#=1");
    add_txt(service, "sf=%d", hap_is_paired() ? 0 : 1);
    add_txt(service, "ci=2");  // category: bridge
}

void hap_port_status_changed(void) {
    if (mdns_netif) mdns_resp_announce(mdns_netif);
}

void hap_server_start(const char *name) {
    if (listener) return;
    instance_name = name;

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb || tcp_bind(pcb, IP_ANY_TYPE, HAP_TCP_PORT) != ERR_OK) {
        printf("hap: cannot bind port %d\n", HAP_TCP_PORT);
        return;
    }
    listener = tcp_listen_with_backlog(pcb, HAP_MAX_CONNECTIONS);
    tcp_accept(listener, on_accept);

    // Host name like pico2homekit-a1b2.local, from the end of the device id.
    const char *id = hap_device_id();
    char host[32];
    snprintf(host, sizeof(host), "pico2homekit-%c%c%c%c", id[12], id[13], id[15], id[16]);
    for (char *c = host; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');

    mdns_netif = &cyw43_state.netif[CYW43_ITF_STA];
    mdns_resp_init();
    mdns_resp_add_netif(mdns_netif, host);
    if (mdns_resp_add_service(mdns_netif, name, "_hap", DNSSD_PROTO_TCP, HAP_TCP_PORT, hap_txt, NULL) < 0)
        printf("hap: mDNS service registration failed\n");
    mdns_resp_announce(mdns_netif);
    printf("hap: listening on port %d, advertising '%s' as %s.local\n", HAP_TCP_PORT, name, host);
}
