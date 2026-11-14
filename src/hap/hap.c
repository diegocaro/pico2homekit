#include "hap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hap_db.h"
#include "hap_port.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "srp.h"
#include "tlv8.h"

#define LOG(...) printf("hap: " __VA_ARGS__)

static bool debug_on;
#define DBG(...) do { if (debug_on) printf("hap[dbg]: " __VA_ARGS__); } while (0)

void hap_set_debug(bool on) { debug_on = on; }
bool hap_debug_enabled(void) { return debug_on; }

// Logs the items of a TLV8 body as "type:len" (and "=value" for 1-byte items).
static void dbg_tlv(const char *what, const uint8_t *in, size_t len) {
    if (!debug_on) return;
    printf("hap[dbg]: %s (%u bytes):", what, (unsigned)len);
    for (size_t i = 0; i + 2 <= len; i += 2 + in[i + 1]) {
        printf(" %02x:%u", in[i], in[i + 1]);
        if (in[i + 1] == 1 && i + 2 < len) printf("=%u", in[i + 2]);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Persistent state

#define STORE_MAGIC 0x48415031u  // "HAP1"
#define STORE_VERSION 1

typedef struct {
    uint8_t used;
    uint8_t perms;     // bit 0: admin
    uint8_t id_len;
    uint8_t id[36];
    uint8_t ltpk[32];
} pairing_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char device_id[18];
    uint8_t ltsk[64];  // Ed25519 secret key (seed | public key)
    uint8_t ltpk[32];
    uint32_t config_number;
    uint32_t layout_hash;
    uint32_t code_hash;
    uint8_t srp_salt[SRP_SALT_LEN];
    uint8_t srp_verifier[SRP_LEN];
    pairing_t pairings[HAP_MAX_PAIRINGS];
} store_t;

static store_t store;

static void store_save(void) {
    if (!hap_port_store_save(&store, sizeof(store))) LOG("failed to save state!\n");
}

// ---------------------------------------------------------------------------
// Connections

#define REQ_CAP 2048
#define FRAME_MAX 1024
#define RAW_CAP (2 + FRAME_MAX + 16)

struct hap_conn {
    bool used;
    bool closing;
    void *handle;
    // encrypted session (after Pair Verify)
    bool secure;
    int pairing;              // index into store.pairings
    uint8_t read_key[32];     // controller -> accessory
    uint8_t write_key[32];    // accessory -> controller
    uint64_t read_ctr, write_ctr;
    // Pair Verify in progress
    uint8_t pv_state;
    uint8_t pv_secret[32], pv_public[32], pv_peer[32], pv_shared[32];
    uint64_t events;          // subscribed notification slots
    size_t raw_len, req_len;
    uint8_t raw[RAW_CAP];     // undecrypted frames
    uint8_t req[REQ_CAP];     // plaintext HTTP
};

static hap_conn_t conns[HAP_MAX_CONNECTIONS];

// Pair Setup is one-at-a-time, accessory-wide.
static struct {
    hap_conn_t *owner;
    uint8_t state;
    uint8_t b[SRP_PRIV_LEN];
    uint8_t B[SRP_LEN];
    uint8_t K[64];
    uint32_t started_ms;
} ps;
static unsigned failed_setup_attempts;

static char setup_code[11];

// Shared output buffers (everything runs on one thread).
#define BODY_CAP 12288
static char body[BODY_CAP];
static uint8_t msg[BODY_CAP + 256];

// ---------------------------------------------------------------------------
// Crypto helpers

static void hkdf32(uint8_t out[32], const uint8_t *ikm, size_t ikm_len, const char *salt, const char *info) {
    crypto_sha512_hkdf(out, 32, ikm, ikm_len, (const uint8_t *)salt, strlen(salt),
                       (const uint8_t *)info, strlen(info));
}

static void nonce_label(uint8_t nonce[12], const char *label8) {
    memset(nonce, 0, 4);
    memcpy(nonce + 4, label8, 8);
}

static void nonce_counter(uint8_t nonce[12], uint64_t ctr) {
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(ctr >> (8 * i));
}

// ChaCha20-Poly1305 (RFC 8439). out receives ciphertext followed by the 16-byte tag.
static void aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                      const uint8_t *in, size_t len, uint8_t *out) {
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, out, out + len, aad, aad_len, in, len);
    crypto_wipe(&ctx, sizeof(ctx));
}

// in = ciphertext | tag (len includes the tag). Returns 0 if authentic.
static int aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t len, uint8_t *out) {
    if (len < 16) return -1;
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    int r = crypto_aead_read(&ctx, out, in + len - 16, aad, aad_len, in, len - 16);
    crypto_wipe(&ctx, sizeof(ctx));
    return r;
}

static uint32_t hash32(const void *data, size_t len) {
    uint8_t h[64];
    crypto_sha512(h, data, len);
    return (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
}

// ---------------------------------------------------------------------------
// Pairings

static int pairing_find(const uint8_t *id, size_t id_len) {
    for (int i = 0; i < HAP_MAX_PAIRINGS; i++) {
        pairing_t *p = &store.pairings[i];
        if (p->used && p->id_len == id_len && memcmp(p->id, id, id_len) == 0) return i;
    }
    return -1;
}

static int pairing_add(const uint8_t *id, size_t id_len, const uint8_t ltpk[32], uint8_t perms) {
    for (int i = 0; i < HAP_MAX_PAIRINGS; i++) {
        pairing_t *p = &store.pairings[i];
        if (!p->used) {
            p->used = 1;
            p->perms = perms;
            p->id_len = (uint8_t)id_len;
            memcpy(p->id, id, id_len);
            memcpy(p->ltpk, ltpk, 32);
            return i;
        }
    }
    return -1;
}

bool hap_is_paired(void) {
    for (int i = 0; i < HAP_MAX_PAIRINGS; i++)
        if (store.pairings[i].used) return true;
    return false;
}

int hap_pairing_count(void) {
    int n = 0;
    for (int i = 0; i < HAP_MAX_PAIRINGS; i++) n += store.pairings[i].used;
    return n;
}

static bool any_admin(void) {
    for (int i = 0; i < HAP_MAX_PAIRINGS; i++)
        if (store.pairings[i].used && (store.pairings[i].perms & 1)) return true;
    return false;
}

static void close_conn(hap_conn_t *c) {
    if (c->used && !c->closing) {
        c->closing = true;
        hap_port_close(c->handle);
    }
}

// ---------------------------------------------------------------------------
// Output

static void conn_send(hap_conn_t *c, const uint8_t *data, size_t len) {
    if (c->closing) return;
    if (!c->secure) {
        if (!hap_port_send(c->handle, data, len)) close_conn(c);
        return;
    }
    static uint8_t frame[RAW_CAP];
    while (len > 0) {
        size_t n = len > FRAME_MAX ? FRAME_MAX : len;
        uint8_t nonce[12];
        frame[0] = (uint8_t)n;
        frame[1] = (uint8_t)(n >> 8);
        nonce_counter(nonce, c->write_ctr++);
        aead_seal(c->write_key, nonce, frame, 2, data, n, frame + 2);
        if (!hap_port_send(c->handle, frame, 2 + n + 16)) { close_conn(c); return; }
        data += n;
        len -= n;
    }
}

static void send_response(hap_conn_t *c, int code, const char *reason, const char *ctype,
                          const void *data, size_t len) {
    int h;
    if (code == 204)
        h = snprintf((char *)msg, 128, "HTTP/1.1 204 No Content\r\n\r\n");
    else
        h = snprintf((char *)msg, 256, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n",
                     code, reason, ctype, (unsigned)len);
    if (len > sizeof(msg) - (size_t)h) { close_conn(c); return; }
    DBG("conn %d -> %d (%u bytes)\n", (int)(c - conns), code, (unsigned)len);
    if (debug_on && ctype && strstr(ctype, "tlv8")) dbg_tlv("response", data, len);
    if (len && data != msg + h) memmove(msg + h, data, len);
    conn_send(c, msg, (size_t)h + len);
}

static void send_tlv(hap_conn_t *c, const tlv_writer_t *w) {
    send_response(c, 200, "OK", "application/pairing+tlv8", w->buf, w->len);
}

static void send_tlv_error(hap_conn_t *c, uint8_t state, uint8_t err) {
    uint8_t buf[6];
    tlv_writer_t w;
    tlv_init(&w, buf, sizeof(buf));
    tlv_add_u8(&w, TLV_STATE, state);
    tlv_add_u8(&w, TLV_ERROR, err);
    send_tlv(c, &w);
}

static void send_json(hap_conn_t *c, int code, const char *reason, const char *json, size_t len) {
    send_response(c, code, reason, "application/hap+json", json, len);
}

static void send_status(hap_conn_t *c, int code, const char *reason, int status) {
    char j[32];
    int n = snprintf(j, sizeof(j), "{\"status\":%d}", status);
    send_json(c, code, reason, j, (size_t)n);
}

// ---------------------------------------------------------------------------
// Pair Setup (POST /pair-setup)

static void ps_reset(void) {
    crypto_wipe(&ps, sizeof(ps));
}

static void pair_setup(hap_conn_t *c, const uint8_t *in, size_t in_len) {
    int state = tlv_get_u8(in, in_len, TLV_STATE);
    static uint8_t out[1024];
    tlv_writer_t w;
    tlv_init(&w, out, sizeof(out));

    // An abandoned setup (controller went away) must not block others forever.
    if (ps.owner && hap_port_millis() - ps.started_ms > 60000) ps_reset();

    if (state == 1) {
        if (hap_is_paired()) { LOG("pair-setup refused: already paired\n"); send_tlv_error(c, 2, TLV_ERR_UNAVAILABLE); return; }
        if (failed_setup_attempts >= 100) { send_tlv_error(c, 2, TLV_ERR_MAX_TRIES); return; }
        if (ps.owner && ps.owner != c) { send_tlv_error(c, 2, TLV_ERR_BUSY); return; }
        ps_reset();
        ps.owner = c;
        ps.started_ms = hap_port_millis();
        hap_port_random(ps.b, sizeof(ps.b));
        LOG("pair-setup M1: computing SRP public key...\n");
        uint32_t t0 = hap_port_millis();
        if (srp_server_public(store.srp_verifier, ps.b, ps.B) != 0) { ps_reset(); send_tlv_error(c, 2, TLV_ERR_UNKNOWN); return; }
        LOG("pair-setup M2 (%u ms)\n", (unsigned)(hap_port_millis() - t0));
        tlv_add_u8(&w, TLV_STATE, 2);
        tlv_add(&w, TLV_PUBLIC_KEY, ps.B, SRP_LEN);
        tlv_add(&w, TLV_SALT, store.srp_salt, SRP_SALT_LEN);
        ps.state = 2;
        send_tlv(c, &w);
        return;
    }

    if (ps.owner != c) { send_tlv_error(c, (uint8_t)(state + 1), TLV_ERR_UNKNOWN); return; }

    if (state == 3 && ps.state == 2) {
        static uint8_t A[SRP_LEN];
        static uint8_t M1[64], M2[64];
        int A_len = tlv_get(in, in_len, TLV_PUBLIC_KEY, A, sizeof(A));
        if (A_len <= 0 || tlv_get(in, in_len, TLV_PROOF, M1, sizeof(M1)) != 64) {
            ps_reset(); send_tlv_error(c, 4, TLV_ERR_UNKNOWN); return;
        }
        LOG("pair-setup M3: verifying proof...\n");
        uint32_t t0 = hap_port_millis();
        if (srp_server_verify(store.srp_verifier, ps.b, ps.B, A, (size_t)A_len, store.srp_salt, M1, ps.K, M2) != 0) {
            failed_setup_attempts++;
            LOG("pair-setup failed: wrong setup code\n");
            ps_reset();
            send_tlv_error(c, 4, TLV_ERR_AUTHENTICATION);
            return;
        }
        LOG("pair-setup M4 (%u ms)\n", (unsigned)(hap_port_millis() - t0));
        tlv_add_u8(&w, TLV_STATE, 4);
        tlv_add(&w, TLV_PROOF, M2, 64);
        ps.state = 4;
        send_tlv(c, &w);
        return;
    }

    if (state == 5 && ps.state == 4) {
        static uint8_t enc[512], plain[512];
        uint8_t key[32], nonce[12];
        int enc_len = tlv_get(in, in_len, TLV_ENCRYPTED_DATA, enc, sizeof(enc));
        hkdf32(key, ps.K, 64, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");
        nonce_label(nonce, "PS-Msg05");
        if (enc_len < 16 || aead_open(key, nonce, NULL, 0, enc, (size_t)enc_len, plain) != 0) {
            LOG("pair-setup M5: decryption failed\n");
            ps_reset(); send_tlv_error(c, 6, TLV_ERR_AUTHENTICATION); return;
        }
        size_t plain_len = (size_t)enc_len - 16;

        uint8_t id[36], ltpk[32], sig[64];
        int id_len = tlv_get(plain, plain_len, TLV_IDENTIFIER, id, sizeof(id));
        if (id_len <= 0 || tlv_get(plain, plain_len, TLV_PUBLIC_KEY, ltpk, 32) != 32 ||
            tlv_get(plain, plain_len, TLV_SIGNATURE, sig, 64) != 64) {
            ps_reset(); send_tlv_error(c, 6, TLV_ERR_UNKNOWN); return;
        }
        // Verify the controller's signature over iOSDeviceX | id | LTPK.
        static uint8_t info[32 + 36 + 32];
        hkdf32(info, ps.K, 64, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info");
        memcpy(info + 32, id, (size_t)id_len);
        memcpy(info + 32 + id_len, ltpk, 32);
        if (crypto_ed25519_check(sig, ltpk, info, 32 + (size_t)id_len + 32) != 0) {
            LOG("pair-setup M5: bad controller signature\n");
            ps_reset(); send_tlv_error(c, 6, TLV_ERR_AUTHENTICATION); return;
        }

        int slot = pairing_find(id, (size_t)id_len);
        if (slot < 0) slot = pairing_add(id, (size_t)id_len, ltpk, 1);
        if (slot < 0) { ps_reset(); send_tlv_error(c, 6, TLV_ERR_MAX_PEERS); return; }
        store.pairings[slot].perms = 1;
        memcpy(store.pairings[slot].ltpk, ltpk, 32);

        // M6: our own signed identity, encrypted.
        size_t dlen = strlen(store.device_id);
        static uint8_t ainfo[32 + 17 + 32];
        hkdf32(ainfo, ps.K, 64, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info");
        memcpy(ainfo + 32, store.device_id, dlen);
        memcpy(ainfo + 32 + dlen, store.ltpk, 32);
        uint8_t asig[64];
        crypto_ed25519_sign(asig, store.ltsk, ainfo, 32 + dlen + 32);

        uint8_t sub[160];
        tlv_writer_t sw;
        tlv_init(&sw, sub, sizeof(sub));
        tlv_add(&sw, TLV_IDENTIFIER, store.device_id, dlen);
        tlv_add(&sw, TLV_PUBLIC_KEY, store.ltpk, 32);
        tlv_add(&sw, TLV_SIGNATURE, asig, 64);
        nonce_label(nonce, "PS-Msg06");
        aead_seal(key, nonce, NULL, 0, sub, sw.len, enc);

        tlv_add_u8(&w, TLV_STATE, 6);
        tlv_add(&w, TLV_ENCRYPTED_DATA, enc, sw.len + 16);
        send_tlv(c, &w);
        crypto_wipe(key, sizeof(key));
        ps_reset();
        failed_setup_attempts = 0;
        store_save();
        LOG("paired with controller %.*s\n", id_len, id);
        hap_port_status_changed();
        return;
    }

    ps_reset();
    send_tlv_error(c, (uint8_t)(state + 1), TLV_ERR_UNKNOWN);
}

// ---------------------------------------------------------------------------
// Pair Verify (POST /pair-verify)

static void pair_verify(hap_conn_t *c, const uint8_t *in, size_t in_len) {
    int state = tlv_get_u8(in, in_len, TLV_STATE);
    static uint8_t out[256], sub[160], enc[160 + 16];
    tlv_writer_t w;
    tlv_init(&w, out, sizeof(out));
    uint8_t key[32], nonce[12];

    if (state == 1) {
        if (tlv_get(in, in_len, TLV_PUBLIC_KEY, c->pv_peer, 32) != 32) { send_tlv_error(c, 2, TLV_ERR_UNKNOWN); return; }
        c->secure = false;
        hap_port_random(c->pv_secret, 32);
        crypto_x25519_public_key(c->pv_public, c->pv_secret);
        crypto_x25519(c->pv_shared, c->pv_secret, c->pv_peer);

        size_t dlen = strlen(store.device_id);
        uint8_t info[32 + 17 + 32], sig[64];
        memcpy(info, c->pv_public, 32);
        memcpy(info + 32, store.device_id, dlen);
        memcpy(info + 32 + dlen, c->pv_peer, 32);
        crypto_ed25519_sign(sig, store.ltsk, info, 32 + dlen + 32);

        tlv_writer_t sw;
        tlv_init(&sw, sub, sizeof(sub));
        tlv_add(&sw, TLV_IDENTIFIER, store.device_id, dlen);
        tlv_add(&sw, TLV_SIGNATURE, sig, 64);
        hkdf32(key, c->pv_shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
        nonce_label(nonce, "PV-Msg02");
        aead_seal(key, nonce, NULL, 0, sub, sw.len, enc);

        tlv_add_u8(&w, TLV_STATE, 2);
        tlv_add(&w, TLV_PUBLIC_KEY, c->pv_public, 32);
        tlv_add(&w, TLV_ENCRYPTED_DATA, enc, sw.len + 16);
        c->pv_state = 2;
        send_tlv(c, &w);
        return;
    }

    if (state == 3 && c->pv_state == 2) {
        c->pv_state = 0;
        int enc_len = tlv_get(in, in_len, TLV_ENCRYPTED_DATA, enc, sizeof(enc));
        hkdf32(key, c->pv_shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
        nonce_label(nonce, "PV-Msg03");
        if (enc_len < 16 || aead_open(key, nonce, NULL, 0, enc, (size_t)enc_len, sub) != 0) {
            send_tlv_error(c, 4, TLV_ERR_AUTHENTICATION); return;
        }
        size_t sub_len = (size_t)enc_len - 16;
        uint8_t id[36], sig[64];
        int id_len = tlv_get(sub, sub_len, TLV_IDENTIFIER, id, sizeof(id));
        if (id_len <= 0 || tlv_get(sub, sub_len, TLV_SIGNATURE, sig, 64) != 64) {
            send_tlv_error(c, 4, TLV_ERR_AUTHENTICATION); return;
        }
        int slot = pairing_find(id, (size_t)id_len);
        if (slot < 0) {
            LOG("pair-verify: unknown controller %.*s\n", id_len, id);
            send_tlv_error(c, 4, TLV_ERR_AUTHENTICATION); return;
        }
        uint8_t info[32 + 36 + 32];
        memcpy(info, c->pv_peer, 32);
        memcpy(info + 32, id, (size_t)id_len);
        memcpy(info + 32 + id_len, c->pv_public, 32);
        if (crypto_ed25519_check(sig, store.pairings[slot].ltpk, info, 32 + (size_t)id_len + 32) != 0) {
            LOG("pair-verify: bad signature\n");
            send_tlv_error(c, 4, TLV_ERR_AUTHENTICATION); return;
        }

        tlv_add_u8(&w, TLV_STATE, 4);
        send_tlv(c, &w);   // last plaintext message

        hkdf32(c->read_key, c->pv_shared, 32, "Control-Salt", "Control-Write-Encryption-Key");
        hkdf32(c->write_key, c->pv_shared, 32, "Control-Salt", "Control-Read-Encryption-Key");
        c->read_ctr = c->write_ctr = 0;
        c->secure = true;
        c->pairing = slot;
        c->events = 0;
        crypto_wipe(c->pv_secret, 32);
        crypto_wipe(c->pv_shared, 32);
        LOG("session opened for %.*s\n", id_len, id);
        return;
    }

    c->pv_state = 0;
    send_tlv_error(c, (uint8_t)(state + 1), TLV_ERR_UNKNOWN);
}

// ---------------------------------------------------------------------------
// Pairings management (POST /pairings, admin only)

static void remove_pairing_slot(int slot) {
    crypto_wipe(&store.pairings[slot], sizeof(pairing_t));
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++)
        if (conns[i].used && conns[i].secure && conns[i].pairing == slot) close_conn(&conns[i]);
}

static void pairings(hap_conn_t *c, const uint8_t *in, size_t in_len) {
    int method = tlv_get_u8(in, in_len, TLV_METHOD);
    if (!(store.pairings[c->pairing].perms & 1)) { send_tlv_error(c, 2, TLV_ERR_AUTHENTICATION); return; }

    static uint8_t out[HAP_MAX_PAIRINGS * (2 + 36 + 2 + 32 + 3 + 2) + 8];
    tlv_writer_t w;
    tlv_init(&w, out, sizeof(out));
    uint8_t id[36], ltpk[32];
    int id_len = tlv_get(in, in_len, TLV_IDENTIFIER, id, sizeof(id));

    if (method == 3) {  // Add Pairing
        int perms = tlv_get_u8(in, in_len, TLV_PERMISSIONS);
        if (id_len <= 0 || tlv_get(in, in_len, TLV_PUBLIC_KEY, ltpk, 32) != 32 || perms < 0) {
            send_tlv_error(c, 2, TLV_ERR_UNKNOWN); return;
        }
        int slot = pairing_find(id, (size_t)id_len);
        if (slot >= 0) {
            if (memcmp(store.pairings[slot].ltpk, ltpk, 32) != 0) { send_tlv_error(c, 2, TLV_ERR_UNKNOWN); return; }
            store.pairings[slot].perms = (uint8_t)perms;
        } else if (pairing_add(id, (size_t)id_len, ltpk, (uint8_t)perms) < 0) {
            send_tlv_error(c, 2, TLV_ERR_MAX_PEERS); return;
        }
        store_save();
        LOG("added pairing %.*s (perms %d)\n", id_len, id, perms);
        tlv_add_u8(&w, TLV_STATE, 2);
        send_tlv(c, &w);
        return;
    }

    if (method == 4) {  // Remove Pairing
        if (id_len <= 0) { send_tlv_error(c, 2, TLV_ERR_UNKNOWN); return; }
        tlv_add_u8(&w, TLV_STATE, 2);
        send_tlv(c, &w);   // respond before possibly closing this very connection
        int slot = pairing_find(id, (size_t)id_len);
        if (slot >= 0) {
            LOG("removed pairing %.*s\n", id_len, id);
            remove_pairing_slot(slot);
            if (!any_admin()) {
                LOG("no admin left: removing all pairings\n");
                for (int i = 0; i < HAP_MAX_PAIRINGS; i++)
                    if (store.pairings[i].used) remove_pairing_slot(i);
            }
            store_save();
            hap_port_status_changed();
        }
        return;
    }

    if (method == 5) {  // List Pairings
        tlv_add_u8(&w, TLV_STATE, 2);
        bool first = true;
        for (int i = 0; i < HAP_MAX_PAIRINGS; i++) {
            pairing_t *p = &store.pairings[i];
            if (!p->used) continue;
            if (!first) tlv_add(&w, TLV_SEPARATOR, NULL, 0);
            first = false;
            tlv_add(&w, TLV_IDENTIFIER, p->id, p->id_len);
            tlv_add(&w, TLV_PUBLIC_KEY, p->ltpk, 32);
            tlv_add_u8(&w, TLV_PERMISSIONS, p->perms);
        }
        send_tlv(c, &w);
        return;
    }

    send_tlv_error(c, 2, TLV_ERR_UNKNOWN);
}

// ---------------------------------------------------------------------------
// /characteristics

static bool parse_uint(const char **p, const char *end, unsigned *out) {
    const char *s = *p;
    unsigned v = 0;
    if (s >= end || *s < '0' || *s > '9') return false;
    while (s < end && *s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    *out = v;
    *p = s;
    return true;
}

// GET /characteristics?id=1.2,3.4[&meta=1...]
static void get_characteristics(hap_conn_t *c, const char *query) {
    const char *p = strstr(query, "id=");
    if (!p) { send_status(c, 400, "Bad Request", HAP_STATUS_INVALID_VALUE); return; }
    p += 3;
    const char *end = strchr(p, '&');
    if (!end) end = p + strlen(p);

    static struct { unsigned aid, iid; int status; } items[32];
    int n = 0;
    bool any_error = false;
    while (p < end && n < 32) {
        unsigned aid, iid;
        if (!parse_uint(&p, end, &aid) || p >= end || *p++ != '.' || !parse_uint(&p, end, &iid)) {
            send_status(c, 400, "Bad Request", HAP_STATUS_INVALID_VALUE); return;
        }
        items[n].aid = aid;
        items[n].iid = iid;
        n++;
        if (p < end && *p == ',') p++;
    }

    size_t len = 0;
    len += (size_t)snprintf(body + len, BODY_CAP - len, "{\"characteristics\":[");
    char val[64];
    for (int i = 0; i < n; i++) {
        items[i].status = hap_db_read(items[i].aid, items[i].iid, val, sizeof(val));
        if (items[i].status != HAP_STATUS_OK) any_error = true;
    }
    for (int i = 0; i < n && len < BODY_CAP - 128; i++) {
        len += (size_t)snprintf(body + len, BODY_CAP - len, "%s{\"aid\":%u,\"iid\":%u", i ? "," : "",
                                items[i].aid, items[i].iid);
        if (items[i].status == HAP_STATUS_OK) {
            hap_db_read(items[i].aid, items[i].iid, val, sizeof(val));
            len += (size_t)snprintf(body + len, BODY_CAP - len, ",\"value\":%s", val);
        }
        if (any_error) len += (size_t)snprintf(body + len, BODY_CAP - len, ",\"status\":%d", items[i].status);
        len += (size_t)snprintf(body + len, BODY_CAP - len, "}");
    }
    len += (size_t)snprintf(body + len, BODY_CAP - len, "]}");
    if (any_error) send_json(c, 207, "Multi-Status", body, len);
    else send_json(c, 200, "OK", body, len);
}

// Finds `"key":` inside [obj, end) and returns a pointer to its value.
static const char *json_value(const char *obj, const char *end, const char *key) {
    size_t kl = strlen(key);
    for (const char *s = obj; s + kl + 2 < end; s++) {
        if (*s == '"' && memcmp(s + 1, key, kl) == 0 && s[kl + 1] == '"') {
            s += kl + 2;
            while (s < end && (*s == ' ' || *s == ':')) s++;
            return s;
        }
    }
    return NULL;
}

static size_t json_token_len(const char *v, const char *end) {
    const char *s = v;
    if (s < end && *s == '"') {
        for (s++; s < end && *s != '"'; s++)
            if (*s == '\\') s++;
        return (size_t)(s + 1 - v);
    }
    while (s < end && *s != ',' && *s != '}' && *s != ']' && *s != ' ') s++;
    return (size_t)(s - v);
}

// PUT /characteristics {"characteristics":[{"aid":2,"iid":9,"ev":true}, ...]}
static void put_characteristics(hap_conn_t *c, const char *json, size_t len) {
    const char *end = json + len;
    const char *p = strstr(json, "\"characteristics\"");
    if (!p) { send_status(c, 400, "Bad Request", HAP_STATUS_INVALID_VALUE); return; }

    static struct { unsigned aid, iid; int status; } items[32];
    int n = 0;
    bool any_error = false;
    while (n < 32 && (p = memchr(p, '{', (size_t)(end - p))) != NULL) {
        const char *obj_end = memchr(p, '}', (size_t)(end - p));
        if (!obj_end) break;
        const char *a = json_value(p, obj_end, "aid"), *i = json_value(p, obj_end, "iid");
        unsigned aid = 0, iid = 0;
        if (!a || !i || !parse_uint(&a, obj_end, &aid) || !parse_uint(&i, obj_end, &iid)) {
            send_status(c, 400, "Bad Request", HAP_STATUS_INVALID_VALUE); return;
        }
        int status = HAP_STATUS_OK;
        const char *ev = json_value(p, obj_end, "ev");
        if (ev) {
            int slot = hap_db_event_slot(aid, iid);
            if (slot < 0) status = slot;
            else if (*ev == 't' || *ev == '1') c->events |= 1ull << slot;
            else c->events &= ~(1ull << slot);
        }
        const char *val = json_value(p, obj_end, "value");
        if (val && status == HAP_STATUS_OK)
            status = hap_db_write(aid, iid, val, json_token_len(val, obj_end));
        items[n].aid = aid;
        items[n].iid = iid;
        items[n].status = status;
        if (status != HAP_STATUS_OK) any_error = true;
        n++;
        p = obj_end + 1;
    }

    if (!any_error) { send_response(c, 204, NULL, NULL, NULL, 0); return; }
    size_t l = (size_t)snprintf(body, BODY_CAP, "{\"characteristics\":[");
    for (int k = 0; k < n; k++)
        l += (size_t)snprintf(body + l, BODY_CAP - l, "%s{\"aid\":%u,\"iid\":%u,\"status\":%d}", k ? "," : "",
                              items[k].aid, items[k].iid, items[k].status);
    l += (size_t)snprintf(body + l, BODY_CAP - l, "]}");
    send_json(c, 207, "Multi-Status", body, l);
}

void hap_notify(unsigned aid, unsigned iid) {
    int slot = hap_db_event_slot(aid, iid);
    if (slot < 0) return;
    char val[64];
    if (hap_db_read(aid, iid, val, sizeof(val)) != HAP_STATUS_OK) return;
    char json[128];
    int jl = snprintf(json, sizeof(json), "{\"characteristics\":[{\"aid\":%u,\"iid\":%u,\"value\":%s}]}", aid, iid, val);
    char ev[256];
    int el = snprintf(ev, sizeof(ev), "EVENT/1.0 200 OK\r\nContent-Type: application/hap+json\r\nContent-Length: %d\r\n\r\n%s",
                      jl, json);
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++) {
        hap_conn_t *c = &conns[i];
        if (c->used && !c->closing && c->secure && (c->events & (1ull << slot))) {
            DBG("conn %d <- event %s\n", i, json);
            conn_send(c, (const uint8_t *)ev, (size_t)el);
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP

static void handle_request(hap_conn_t *c, const char *method, char *path, const uint8_t *data, size_t len) {
    bool post = strcmp(method, "POST") == 0, get = strcmp(method, "GET") == 0, put = strcmp(method, "PUT") == 0;
    DBG("conn %d %s: %s %s (%u bytes)%s%.*s\n", (int)(c - conns), c->secure ? "secure" : "plain", method, path,
        (unsigned)len, put ? " " : "", put ? (int)(len > 200 ? 200 : len) : 0, (const char *)data);
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';

    if (post && (strcmp(path, "/pair-setup") == 0 || strcmp(path, "/pair-verify") == 0 ||
                 strcmp(path, "/pairings") == 0))
        dbg_tlv(path, data, len);
    if (post && strcmp(path, "/pair-setup") == 0) { pair_setup(c, data, len); return; }
    if (post && strcmp(path, "/pair-verify") == 0) { pair_verify(c, data, len); return; }
    if (post && strcmp(path, "/identify") == 0) {
        if (hap_is_paired()) { send_status(c, 400, "Bad Request", HAP_STATUS_INSUFFICIENT_PRIVILEGES); return; }
        hap_db_write(1, 2, "true", 4);
        send_response(c, 204, NULL, NULL, NULL, 0);
        return;
    }

    if (!c->secure) { send_status(c, 470, "Connection Authorization Required", HAP_STATUS_INSUFFICIENT_PRIVILEGES); return; }

    if (get && strcmp(path, "/accessories") == 0) {
        size_t n = hap_db_accessories_json(body, BODY_CAP);
        if (!n) { send_status(c, 500, "Internal Server Error", -70407); return; }
        send_json(c, 200, "OK", body, n);
        return;
    }
    if (get && strcmp(path, "/characteristics") == 0) { get_characteristics(c, query ? query : ""); return; }
    if (put && strcmp(path, "/characteristics") == 0) {
        // body is NUL-terminated by process_requests()
        put_characteristics(c, (const char *)data, len);
        return;
    }
    if (post && strcmp(path, "/pairings") == 0) { pairings(c, data, len); return; }

    send_status(c, 404, "Not Found", HAP_STATUS_NOT_FOUND);
}

static size_t header_end(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return i + 4;
    return 0;
}

static void process_requests(hap_conn_t *c) {
    while (!c->closing) {
        size_t hlen = header_end(c->req, c->req_len);
        if (!hlen) {
            if (c->req_len >= REQ_CAP - 1) { LOG("request header too large\n"); close_conn(c); }
            return;
        }
        // Request line: METHOD SP PATH SP VERSION
        char method[8], path[160];
        const char *line = (const char *)c->req;
        const char *sp1 = memchr(line, ' ', hlen);
        const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', hlen - (size_t)(sp1 + 1 - line)) : NULL;
        if (!sp1 || !sp2 || (size_t)(sp1 - line) >= sizeof(method) || (size_t)(sp2 - sp1 - 1) >= sizeof(path)) {
            close_conn(c);
            return;
        }
        memcpy(method, line, (size_t)(sp1 - line));
        method[sp1 - line] = '\0';
        memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
        path[sp2 - sp1 - 1] = '\0';

        size_t clen = 0;
        for (size_t i = 0; i + 15 < hlen; i++) {
            if ((line[i] == '\n') && strncasecmp(line + i + 1, "Content-Length:", 15) == 0) {
                clen = strtoul(line + i + 16, NULL, 10);
                break;
            }
        }
        if (hlen + clen >= REQ_CAP) { LOG("request body too large\n"); close_conn(c); return; }
        if (c->req_len < hlen + clen) return;  // wait for the rest of the body

        uint8_t *data = c->req + hlen;
        uint8_t saved = data[clen];
        data[clen] = '\0';  // lets JSON handlers use string functions
        handle_request(c, method, path, data, clen);
        data[clen] = saved;

        size_t used = hlen + clen;
        memmove(c->req, c->req + used, c->req_len - used);
        c->req_len -= used;
    }
}

static bool req_append(hap_conn_t *c, const uint8_t *data, size_t len) {
    if (c->req_len + len >= REQ_CAP) { LOG("request too large\n"); close_conn(c); return false; }
    memcpy(c->req + c->req_len, data, len);
    c->req_len += len;
    return true;
}

void hap_conn_recv(hap_conn_t *c, const uint8_t *data, size_t len) {
    while (len > 0 && !c->closing) {
        if (!c->secure) {
            // Plaintext until Pair Verify completes. Feed one byte past the
            // first complete request at most, so bytes that arrive encrypted
            // after M4 are not parsed as plaintext.
            if (!req_append(c, data, len)) return;
            len = 0;
            process_requests(c);
            if (c->secure && c->req_len > 0) {
                // Bytes after the Pair Verify M4 request are ciphertext.
                static uint8_t tmp[RAW_CAP];
                size_t n = c->req_len > sizeof(tmp) ? sizeof(tmp) : c->req_len;
                memcpy(tmp, c->req, n);
                c->req_len = 0;
                hap_conn_recv(c, tmp, n);
            }
            return;
        }

        size_t n = RAW_CAP - c->raw_len;
        if (n > len) n = len;
        memcpy(c->raw + c->raw_len, data, n);
        c->raw_len += n;
        data += n;
        len -= n;

        while (c->raw_len >= 2 && !c->closing) {
            size_t flen = (size_t)c->raw[0] | (size_t)c->raw[1] << 8;
            if (flen > FRAME_MAX) { LOG("bad frame length\n"); close_conn(c); return; }
            if (c->raw_len < 2 + flen + 16) break;
            uint8_t nonce[12];
            static uint8_t plain[FRAME_MAX];
            nonce_counter(nonce, c->read_ctr++);
            if (aead_open(c->read_key, nonce, c->raw, 2, c->raw + 2, flen + 16, plain) != 0) {
                LOG("frame decryption failed\n");
                close_conn(c);
                return;
            }
            size_t used = 2 + flen + 16;
            memmove(c->raw, c->raw + used, c->raw_len - used);
            c->raw_len -= used;
            if (!req_append(c, plain, flen)) return;
            process_requests(c);
        }
    }
}

hap_conn_t *hap_conn_new(void *handle) {
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++) {
        hap_conn_t *c = &conns[i];
        if (!c->used) {
            memset(c, 0, sizeof(*c));
            c->used = true;
            c->handle = handle;
            c->pairing = -1;
            return c;
        }
    }
    return NULL;
}

void hap_conn_free(hap_conn_t *c) {
    if (!c) return;
    if (ps.owner == c) ps_reset();
    crypto_wipe(c, sizeof(*c));
}

int hap_secure_connection_count(void) {
    int n = 0;
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++) n += conns[i].used && conns[i].secure;
    return n;
}

// ---------------------------------------------------------------------------
// Identity and setup

bool hap_setup_code_valid(const char *code) {
    static const char *const banned[] = {"000-00-000", "111-11-111", "222-22-222", "333-33-333",
                                         "444-44-444", "555-55-555", "666-66-666", "777-77-777",
                                         "888-88-888", "999-99-999", "123-45-678", "876-54-321"};
    if (strlen(code) != 10) return false;
    for (int i = 0; i < 10; i++) {
        bool dash = i == 3 || i == 6;
        if (dash ? code[i] != '-' : (code[i] < '0' || code[i] > '9')) return false;
    }
    for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++)
        if (strcmp(code, banned[i]) == 0) return false;
    return true;
}

static void new_identity(void) {
    memset(&store, 0, sizeof(store));
    store.magic = STORE_MAGIC;
    store.version = STORE_VERSION;
    uint8_t id[6], seed[32];
    hap_port_random(id, sizeof(id));
    snprintf(store.device_id, sizeof(store.device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             id[0], id[1], id[2], id[3], id[4], id[5]);
    hap_port_random(seed, sizeof(seed));
    LOG("generating Ed25519 key pair\n");
    crypto_ed25519_key_pair(store.ltsk, store.ltpk, seed);  // wipes seed
    store.config_number = 1;
    LOG("created new accessory identity %s\n", store.device_id);
}

static bool prepare_verifier(void) {
    // The prefix versions the SRP implementation: bumping it forces a recompute.
    char tagged[32];
    snprintf(tagged, sizeof(tagged), "srp2:%s", setup_code);
    uint32_t h = hash32(tagged, strlen(tagged));
    if (store.code_hash == h) return true;
    LOG("running SRP self-test...\n");
    uint32_t t0 = hap_port_millis();
    if (srp_selftest() != 0) LOG("SRP SELF-TEST FAILED: pairing will not work on this build\n");
    LOG("self-test took %u ms\n", (unsigned)(hap_port_millis() - t0));
    LOG("computing SRP verifier for the setup code...\n");
    t0 = hap_port_millis();
    hap_port_random(store.srp_salt, sizeof(store.srp_salt));
    if (srp_make_verifier(store.srp_salt, setup_code, store.srp_verifier) != 0) return false;
    store.code_hash = h;
    LOG("SRP verifier ready (%u ms)\n", (unsigned)(hap_port_millis() - t0));
    return true;
}

bool hap_init(const char *code) {
    if (!hap_setup_code_valid(code)) {
        LOG("invalid setup code '%s' (format XXX-XX-XXX, not trivial)\n", code);
        return false;
    }
    snprintf(setup_code, sizeof(setup_code), "%s", code);
    memset(conns, 0, sizeof(conns));
    ps_reset();

    LOG("loading state (%u bytes)\n", (unsigned)sizeof(store));
    bool dirty = false;
    if (!hap_port_store_load(&store, sizeof(store)) || store.magic != STORE_MAGIC || store.version != STORE_VERSION) {
        new_identity();
        dirty = true;
    }
    uint32_t code_hash = store.code_hash;
    if (!prepare_verifier()) return false;
    dirty |= store.code_hash != code_hash;

    uint32_t lh = hap_db_layout_hash();
    if (store.layout_hash != lh) {
        dirty = true;
        if (store.layout_hash != 0) {
            store.config_number = store.config_number >= 65535 ? 1 : store.config_number + 1;
            LOG("accessory layout changed, configuration number now %u\n", (unsigned)store.config_number);
        }
        store.layout_hash = lh;
    }
    if (dirty) {   // only write flash when something changed
        LOG("saving state\n");
        store_save();
    }
    LOG("ready\n");
    return true;
}

const char *hap_device_id(void) { return store.device_id; }
uint32_t hap_config_number(void) { return store.config_number; }

void hap_remove_all_pairings(void) {
    for (int i = 0; i < HAP_MAX_PAIRINGS; i++)
        if (store.pairings[i].used) remove_pairing_slot(i);
    store_save();
    hap_port_status_changed();
}

void hap_factory_reset(void) {
    for (int i = 0; i < HAP_MAX_CONNECTIONS; i++) close_conn(&conns[i]);
    new_identity();
    store.code_hash = 0;
    prepare_verifier();
    store.layout_hash = hap_db_layout_hash();
    store_save();
    hap_port_status_changed();
}
