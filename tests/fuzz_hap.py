#!/usr/bin/env python3
"""Throws malformed input at the HomeKit server (as a hostile device on the LAN would)
and checks it never crashes and keeps serving. Run it against a sanitizer build:

  make fuzz      (builds build-host/hap_host_asan and runs this for ~1 minute)
"""
import os
import random
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time

HOST = "127.0.0.1"


def tlv(*items):
    out = b""
    for t, v in items:
        if isinstance(v, int):
            v = bytes([v])
        out += bytes([t, len(v)]) + v
    return out


def post(path, body, extra=b""):
    return (f"POST {path} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n".encode() + extra + b"\r\n" + body)


def cases(rng):
    rand = lambda n: bytes(rng.getrandbits(8) for _ in range(n))
    yield "random bytes", rand(rng.randint(1, 3000))
    yield "no CRLF header", b"GET /accessories HTTP/1.1" + b"A" * 2100
    yield "huge content-length", b"POST /pair-setup HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n" + rand(100)
    yield "negative content-length", b"POST /pair-setup HTTP/1.1\r\nContent-Length: -5\r\n\r\n"
    yield "long path", b"GET /" + b"a" * 400 + b" HTTP/1.1\r\n\r\n"
    yield "long method", b"POSTPOSTPOST /pair-setup HTTP/1.1\r\n\r\n"
    yield "M3 without M1", post("/pair-setup", tlv((6, 3), (3, rand(255)), (3, rand(129)), (4, rand(64))))
    yield "M5 without M3", post("/pair-setup", tlv((5, rand(200)), (6, 5)))
    yield "TLV length past end", post("/pair-setup", bytes([6, 1, 1, 3, 255]) + rand(10))
    yield "oversized A (5 fragments)", post("/pair-setup", tlv((6, 1))) + post("/pair-setup", tlv((6, 3), *[(3, rand(255))] * 5, (4, rand(64))))
    yield "pair-verify bad key length", post("/pair-verify", tlv((6, 1), (3, rand(5))))
    yield "pair-verify M3 garbage", post("/pair-verify", tlv((6, 1), (3, rand(32)))) + post("/pair-verify", tlv((6, 3), (5, rand(120))))
    yield "pair-verify M3 short", post("/pair-verify", tlv((6, 1), (3, rand(32)))) + post("/pair-verify", tlv((6, 3), (5, rand(3))))
    yield "pairings unauthenticated", post("/pairings", tlv((6, 1), (0, 4), (1, b"x")))
    yield "characteristics unauthenticated", b"PUT /characteristics HTTP/1.1\r\nContent-Length: 40\r\n\r\n{\"characteristics\":[{\"aid\":1,\"iid\":2}]}"
    yield "pipelined junk", (post("/identify", b"") * 30)
    yield "random TLV soup", post("/pair-setup", bytes(rng.choice([0, 1, 3, 4, 5, 6, 0xff]) if i % 2 == 0 else rng.randint(0, 255) for i in range(400)))
    yield "random TLV soup verify", post("/pair-verify", bytes(rng.choice([1, 3, 5, 6, 10]) if i % 2 == 0 else rng.randint(0, 255) for i in range(300)))
    yield "slow drip", None


def alive(port):
    s = socket.create_connection((HOST, port), timeout=5)
    s.sendall(b"GET /accessories HTTP/1.1\r\n\r\n")
    data = s.recv(200)
    s.close()
    return b"470" in data


def main():
    binary = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    port = 40000 + secrets.randbelow(10000)
    state = os.path.join(tempfile.mkdtemp(), "state.bin")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    proc = subprocess.Popen([binary, str(port), state], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    for line in proc.stdout:
        if "listening" in line:
            break
    rng = random.Random(1234)
    sent = 0
    t_end = time.time() + seconds
    try:
        # Connection flood: more sockets than slots, then check it still serves.
        flood = [socket.create_connection((HOST, port)) for _ in range(20)]
        time.sleep(0.3)
        for s in flood:
            s.close()
        time.sleep(0.3)
        assert alive(port), "not serving after connection flood"
        while time.time() < t_end:
            for name, payload in cases(rng):
                s = socket.create_connection((HOST, port), timeout=5)
                if payload is None:
                    for b in post("/pair-setup", tlv((6, 1), (0, 0))):
                        s.send(bytes([b]))
                else:
                    s.sendall(payload)
                s.settimeout(0.2)
                try:
                    while s.recv(65536):
                        pass
                except OSError:
                    pass
                s.close()
                sent += 1
                if proc.poll() is not None:
                    raise AssertionError(f"server died after case '{name}'")
        assert alive(port), "not serving at the end"
    finally:
        proc.terminate()
        out = proc.communicate(timeout=5)[0]
    bad = [l for l in out.splitlines() if "Sanitizer" in l or "runtime error" in l]
    if bad or proc.returncode not in (0, -15):
        print("\n".join(out.splitlines()[-40:]))
        sys.exit(f"FAIL: sanitizer report or crash (exit {proc.returncode})")
    print(f"fuzz: {sent} malformed requests, server alive, no sanitizer reports")


if __name__ == "__main__":
    main()
