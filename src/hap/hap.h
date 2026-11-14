// Minimal HomeKit Accessory Protocol (HAP over IP) server core: HTTP framing,
// Pair Setup (SRP), Pair Verify, encrypted sessions, pairings management,
// /accessories and /characteristics with event notifications.
// Transport-agnostic: see hap_port.h.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAP_MAX_CONNECTIONS 8
#define HAP_MAX_PAIRINGS 16

typedef struct hap_conn hap_conn_t;

// Loads or creates the accessory identity. setup_code is "XXX-XX-XXX".
// May take a few seconds when the SRP verifier must be (re)computed.
bool hap_init(const char *setup_code);

hap_conn_t *hap_conn_new(void *handle);   // NULL when all slots are in use

// Call every second or so: closes connections that have not completed Pair Verify
// within this time (they would otherwise let anyone on the LAN hold every slot).
#define HAP_UNVERIFIED_TIMEOUT_MS 60000u
void hap_expire_unverified(void);
// When all slots are taken: the port handle of the oldest connection that has
// not completed Pair Verify (and is not mid Pair Setup), to close for a newcomer.
void *hap_oldest_unverified(void);
void hap_conn_recv(hap_conn_t *c, const uint8_t *data, size_t len);
void hap_conn_free(hap_conn_t *c);

// Value of aid/iid changed: send events to subscribed controllers.
void hap_notify(unsigned aid, unsigned iid);

bool hap_is_paired(void);
const char *hap_device_id(void);          // "AA:BB:CC:DD:EE:FF"
uint32_t hap_config_number(void);
int hap_pairing_count(void);
int hap_secure_connection_count(void);

void hap_remove_all_pairings(void);
void hap_factory_reset(void);             // new identity, no pairings

bool hap_setup_code_valid(const char *code);

// Verbose protocol logging (every request line, pairing TLV items, status codes).
// Off by default; toggled with the `debug` serial command.
void hap_set_debug(bool on);
bool hap_debug_enabled(void);
