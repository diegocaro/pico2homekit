#!/usr/bin/env python3
"""Independent HomeKit (HAP over IP) controller used to test the accessory.

Implements Pair Setup (SRP-6a), Pair Verify, encrypted sessions, pairings
management and the /accessories and /characteristics endpoints with the
`cryptography` package (OpenSSL) and Python big integers, i.e. without sharing
any code with the firmware.

  # against the host build (started and stopped automatically):
  python tools/hap_client.py --spawn build-host/hap_host
  # against a real accessory that is NOT yet paired (pairs, tests, unpairs):
  python tools/hap_client.py --host 192.168.0.141 --port 5001 --code 123-45-679
"""
import argparse
import hashlib
import json
import os
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import time

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

# RFC 5054 3072-bit group, g = 5
N = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF", 16)
G = 5
NLEN = 384

T_METHOD, T_IDENTIFIER, T_SALT, T_PUBLIC_KEY, T_PROOF, T_ENCRYPTED, T_STATE, T_ERROR = 0, 1, 2, 3, 4, 5, 6, 7
T_SIGNATURE, T_PERMISSIONS, T_SEPARATOR = 0x0A, 0x0B, 0xFF

passed = 0


def check(cond, what):
    global passed
    if not cond:
        raise AssertionError("FAILED: " + what)
    passed += 1
    print("  ok  " + what)


def H(*parts):
    h = hashlib.sha512()
    for p in parts:
        h.update(p)
    return h.digest()


def i2b(n, length=None):
    length = length or max(1, (n.bit_length() + 7) // 8)
    return n.to_bytes(length, "big")


def b2i(b):
    return int.from_bytes(b, "big")


def hkdf(ikm, salt, info):
    return HKDF(hashes.SHA512(), 32, salt.encode(), info.encode()).derive(ikm)


def tlv_encode(items):
    out = b""
    for t, v in items:
        if isinstance(v, int):
            v = bytes([v])
        if not v:
            out += bytes([t, 0])
        while v:
            out += bytes([t, min(255, len(v))]) + v[:255]
            v = v[255:]
    return out


def tlv_decode(data):
    """Returns a list of (type, value) with 255-byte fragments merged."""
    items, i = [], 0
    while i + 2 <= len(data):
        t, n = data[i], data[i + 1]
        v = data[i + 2:i + 2 + n]
        if items and items[-1][0] == t and len(items[-1][1]) % 255 == 0 and items[-1][1] and t != T_SEPARATOR:
            items[-1] = (t, items[-1][1] + v)
        else:
            items.append((t, v))
        i += 2 + n
    return items


def tlv_dict(data):
    return {t: v for t, v in tlv_decode(data)}


def raw_pub(key):
    return key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


class Controller:
    def __init__(self, name):
        self.id = name.encode()
        self.ltsk = Ed25519PrivateKey.generate()
        self.ltpk = raw_pub(self.ltsk.public_key())


class Session:
    """One TCP connection, plaintext until pair_verify() succeeds."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=60)
        self.host = host
        self.buf = b""
        self.enc = None

    def close(self):
        self.sock.close()

    # --- transport
    def _send(self, data):
        if not self.enc:
            self.sock.sendall(data)
            return
        while data:
            chunk, data = data[:1024], data[1024:]
            aad = len(chunk).to_bytes(2, "little")
            nonce = bytes(4) + self.enc["wctr"].to_bytes(8, "little")
            self.enc["wctr"] += 1
            self.sock.sendall(aad + ChaCha20Poly1305(self.enc["wkey"]).encrypt(nonce, chunk, aad))

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("connection closed by accessory")
        if not self.enc:
            self.buf += data
            return
        self.enc["raw"] += data
        raw = self.enc["raw"]
        while len(raw) >= 2:
            n = int.from_bytes(raw[:2], "little")
            if len(raw) < 2 + n + 16:
                break
            nonce = bytes(4) + self.enc["rctr"].to_bytes(8, "little")
            self.enc["rctr"] += 1
            self.buf += ChaCha20Poly1305(self.enc["rkey"]).decrypt(nonce, raw[2:2 + n + 16], raw[:2])
            raw = raw[2 + n + 16:]
        self.enc["raw"] = raw

    def read_message(self, timeout=60):
        """Returns (status_line, headers, body) of the next HTTP response or EVENT."""
        self.sock.settimeout(timeout)
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        headers = {k.lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:])}
        n = int(headers.get("content-length", "0"))
        while len(rest) < n:
            self._fill()
            rest = self.buf.split(b"\r\n\r\n", 1)[1]
        self.buf = rest[n:]
        return lines[0], headers, rest[:n]

    def request(self, method, path, body=b"", ctype="application/pairing+tlv8"):
        req = f"{method} {path} HTTP/1.1\r\nHost: {self.host}\r\n"
        if body or method in ("POST", "PUT"):
            req += f"Content-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
        self._send(req.encode() + b"\r\n" + body)
        while True:
            status, headers, data = self.read_message()
            if not status.startswith("EVENT/"):
                return int(status.split()[1]), headers, data

    def tlv(self, path, items):
        code, _, data = self.request("POST", path, tlv_encode(items))
        assert code == 200, f"{path}: HTTP {code}"
        return tlv_dict(data)

    # --- pairing
    def pair_setup(self, code, ctrl):
        """Returns (accessory_id, accessory_ltpk), or the TLV error code (int)."""
        r = self.tlv("/pair-setup", [(T_STATE, 1), (T_METHOD, 0)])
        if T_ERROR in r:
            return r[T_ERROR][0]
        salt, B = r[T_SALT], r[T_PUBLIC_KEY]
        assert len(B) == NLEN and len(salt) == 16

        a = b2i(secrets.token_bytes(32))
        A = i2b(pow(G, a, N), NLEN)
        Bi = b2i(B)
        k = b2i(H(i2b(N), i2b(G, NLEN)))
        u = b2i(H(A, i2b(Bi, NLEN)))
        x = b2i(H(salt, H(b"Pair-Setup:" + code.encode())))
        S = pow((Bi - k * pow(G, x, N)) % N, a + u * x, N)
        K = H(i2b(S, NLEN))
        hn, hg = H(i2b(N)), H(i2b(G))
        M1 = H(bytes(p ^ q for p, q in zip(hn, hg)), H(b"Pair-Setup"), salt, A, B, K)

        t0 = time.time()
        r = self.tlv("/pair-setup", [(T_STATE, 3), (T_PUBLIC_KEY, A), (T_PROOF, M1)])
        self.m3_seconds = time.time() - t0
        if T_ERROR in r:
            return r[T_ERROR][0]
        assert r[T_PROOF] == H(A, M1, K), "accessory proof M2 mismatch"

        key = hkdf(K, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info")
        x_ctrl = hkdf(K, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info")
        sig = ctrl.ltsk.sign(x_ctrl + ctrl.id + ctrl.ltpk)
        sub = tlv_encode([(T_IDENTIFIER, ctrl.id), (T_PUBLIC_KEY, ctrl.ltpk), (T_SIGNATURE, sig)])
        enc = ChaCha20Poly1305(key).encrypt(bytes(4) + b"PS-Msg05", sub, None)
        r = self.tlv("/pair-setup", [(T_STATE, 5), (T_ENCRYPTED, enc)])
        if T_ERROR in r:
            return r[T_ERROR][0]
        sub = tlv_dict(ChaCha20Poly1305(key).decrypt(bytes(4) + b"PS-Msg06", r[T_ENCRYPTED], None))
        acc_id, acc_ltpk = sub[T_IDENTIFIER], sub[T_PUBLIC_KEY]
        x_acc = hkdf(K, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info")
        Ed25519PublicKey.from_public_bytes(acc_ltpk).verify(sub[T_SIGNATURE], x_acc + acc_id + acc_ltpk)
        return acc_id, acc_ltpk

    def pair_verify(self, ctrl, acc_ltpk):
        """Returns True on success, or the TLV error code."""
        sk = X25519PrivateKey.generate()
        pk = raw_pub(sk.public_key())
        r = self.tlv("/pair-verify", [(T_STATE, 1), (T_PUBLIC_KEY, pk)])
        acc_pk = r[T_PUBLIC_KEY]
        shared = sk.exchange(X25519PublicKey.from_public_bytes(acc_pk))
        key = hkdf(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info")
        sub = tlv_dict(ChaCha20Poly1305(key).decrypt(bytes(4) + b"PV-Msg02", r[T_ENCRYPTED], None))
        Ed25519PublicKey.from_public_bytes(acc_ltpk).verify(sub[T_SIGNATURE], acc_pk + sub[T_IDENTIFIER] + pk)

        sig = ctrl.ltsk.sign(pk + ctrl.id + acc_pk)
        enc = ChaCha20Poly1305(key).encrypt(bytes(4) + b"PV-Msg03",
                                            tlv_encode([(T_IDENTIFIER, ctrl.id), (T_SIGNATURE, sig)]), None)
        r = self.tlv("/pair-verify", [(T_STATE, 3), (T_ENCRYPTED, enc)])
        if T_ERROR in r:
            return r[T_ERROR][0]
        self.enc = {
            "wkey": hkdf(shared, "Control-Salt", "Control-Write-Encryption-Key"),
            "rkey": hkdf(shared, "Control-Salt", "Control-Read-Encryption-Key"),
            "wctr": 0, "rctr": 0, "raw": b"",
        }
        return True

    def json(self, method, path, obj=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        code, _, data = self.request(method, path, body, "application/hap+json")
        return code, (json.loads(data) if data else None)


def find_char(accs, aid, ctype):
    for acc in accs["accessories"]:
        if acc["aid"] == aid:
            for svc in acc["services"]:
                for ch in svc["characteristics"]:
                    if ch["type"] == ctype:
                        return ch
    return None


def run(host, port, code, real_device=False):
    admin = Controller("11111111-2222-3333-4444-555555555555")

    print("Pair Setup")
    s = Session(host, port)
    if not real_device:
        check(s.pair_setup("111-22-333", admin) == 2, "wrong setup code is rejected (Authentication error)")
        s.close()
        s = Session(host, port)
    res = s.pair_setup(code, admin)
    check(isinstance(res, tuple), f"pair setup with the right code succeeds (M3 took {s.m3_seconds:.1f} s)")
    acc_id, acc_ltpk = res
    print(f"       accessory id {acc_id.decode()}")
    s.close()

    s2 = Session(host, port)
    check(s2.pair_setup(code, Controller("someone-else")) == 6, "second pair setup is refused (Unavailable)")
    s2.close()

    print("Pair Verify and session")
    s = Session(host, port)
    code_, _, _ = s.request("GET", "/accessories")
    check(code_ == 470, "/accessories without a session -> 470")
    check(s.pair_verify(admin, acc_ltpk) is True, "pair verify succeeds")

    code_, accs = s.json("GET", "/accessories")
    check(code_ == 200 and len(accs["accessories"]) >= 2, f"/accessories lists {len(accs['accessories'])} accessories")
    check(accs["accessories"][0]["aid"] == 1, "aid 1 is the bridge")
    missing = [(a["aid"], ch["iid"]) for a in accs["accessories"] for s_ in a["services"]
               for ch in s_["characteristics"] if "pr" in ch["perms"] and "value" not in ch]
    check(not missing, f"every readable characteristic has a value (missing: {missing})")
    temp = find_char(accs, 2, "11")
    check(temp is not None and temp["iid"] == 9 and "ev" in temp["perms"], "aid 2 has CurrentTemperature (iid 9, events)")

    if real_device:
        code_, r = s.json("GET", "/characteristics?id=2.9,2.11,2.13")
        check(code_ in (200, 207), f"read sensor values: {r}")
    else:
        code_, r = s.json("GET", "/characteristics?id=2.9,2.11,2.13,2.15,3.9")
        vals = {(c["aid"], c["iid"]): c for c in r["characteristics"]}
        check(code_ == 207, "mixed read returns 207 Multi-Status")
        check(vals[(2, 13)]["value"] == 12 and vals[(2, 15)]["value"] == 1, "battery 12 % -> low battery flag")
        check(vals[(3, 9)]["status"] == -70402, "sensor that never reported -> -70402 (No Response)")

        code_, r = s.json("PUT", "/characteristics", {"characteristics": [{"aid": 2, "iid": 9, "value": 30}]})
        check(code_ == 207 and r["characteristics"][0]["status"] == -70404, "writing a read-only value -> -70404")

        code_, r = s.json("PUT", "/characteristics", {"characteristics": [{"aid": 2, "iid": 9, "ev": True}]})
        check(code_ == 204, "subscribe to temperature events -> 204")
        status, _, data = s.read_message(timeout=6)
        ev = json.loads(data)["characteristics"][0]
        check(status.startswith("EVENT/1.0") and (ev["aid"], ev["iid"]) == (2, 9), f"received EVENT {ev}")

        code_, _ = s.json("PUT", "/characteristics", {"characteristics": [{"aid": 1, "iid": 2, "value": True}]})
        check(code_ == 204, "identify write -> 204")

    print("Pairings")
    r = tlv_decode(s.request("POST", "/pairings", tlv_encode([(T_STATE, 1), (T_METHOD, 5)]))[2])
    ids = [v for t, v in r if t == T_IDENTIFIER]
    check(ids == [admin.id], "list pairings shows the admin controller")

    guest = Controller("guest-controller")
    r = s.tlv("/pairings", [(T_STATE, 1), (T_METHOD, 3), (T_IDENTIFIER, guest.id),
                            (T_PUBLIC_KEY, guest.ltpk), (T_PERMISSIONS, 0)])
    check(T_ERROR not in r, "add a second (non-admin) pairing")

    g = Session(host, port)
    check(g.pair_verify(guest, acc_ltpk) is True, "the added controller can pair-verify")
    r = g.tlv("/pairings", [(T_STATE, 1), (T_METHOD, 5)])
    check(r.get(T_ERROR) == b"\x02", "non-admin cannot manage pairings")

    r = s.tlv("/pairings", [(T_STATE, 1), (T_METHOD, 4), (T_IDENTIFIER, guest.id)])
    check(T_ERROR not in r, "remove the second pairing")
    try:
        g.request("GET", "/accessories")
        closed = False
    except (ConnectionError, OSError):
        closed = True
    check(closed, "removed controller's session is closed")

    print("Unpair")
    r = s.tlv("/pairings", [(T_STATE, 1), (T_METHOD, 4), (T_IDENTIFIER, admin.id)])
    check(T_ERROR not in r, "remove the admin pairing")
    s.close()
    s = Session(host, port)
    r = s.tlv("/pair-setup", [(T_STATE, 1), (T_METHOD, 0)])
    check(T_ERROR not in r, "accessory is unpaired again (pair setup accepted)")
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spawn", help="host server binary to start")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--code", default="482-91-375")
    args = ap.parse_args()

    proc = None
    try:
        if args.spawn:
            port = args.port or 50000 + secrets.randbelow(10000)
            state = os.path.join(tempfile.mkdtemp(), "hap_state.bin")
            proc = subprocess.Popen([args.spawn, str(port), state], stdout=subprocess.PIPE, text=True)
            for line in proc.stdout:
                print("  [host] " + line.rstrip())
                if "listening" in line:
                    break
            run("127.0.0.1", port, args.code)
        else:
            run(args.host, args.port or 5001, args.code, real_device=True)
    finally:
        if proc:
            proc.terminate()
            out = proc.communicate(timeout=5)[0]
            for line in out.splitlines():
                print("  [host] " + line)
    print(f"\nall {passed} checks passed")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print(e)
        sys.exit(1)
