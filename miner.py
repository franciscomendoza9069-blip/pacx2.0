#!/usr/bin/env python3
"""Minero DGB CPU (SHA-256d) sobre Stratum. Portatil: Windows, Linux, macOS."""

import configparser
import hashlib
import json
import os
import random
import select
import socket
import struct
import sys
import threading
import time
import urllib.request
from multiprocessing import Array, Event, Process, cpu_count

BASE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(BASE, "miner.cfg")

EMBEDDED_POOL = "stratum+tcp://bcmonster.com:9986"
EMBEDDED_WALLET = "DMQfxFM5MEU75Y1w1UdfYtW4wMaFx6dwRq"
EMBEDDED_SERVER = "https://dgb-panel.dgb-panel-worker.workers.dev/report"
EMBEDDED_THREADS = 0

DIFF1 = int.from_bytes(
    bytes.fromhex("00000000ffff0000000000000000000000000000000000000000000000000000"),
    "big",
)


def auto_threads():
    return max(1, min(64, cpu_count() or 1))


def default_worker():
    host = socket.gethostname() or "pc"
    clean = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in host)
    clean = clean.strip("_-")[:64] or "pc"
    return "pc_" + clean


def ensure_config():
    if os.path.exists(CFG):
        return
    cp = configparser.ConfigParser()
    cp["miner"] = {
        "pool": EMBEDDED_POOL,
        "wallet": EMBEDDED_WALLET,
        "worker": default_worker(),
        "threads": "auto",
        "server": EMBEDDED_SERVER,
    }
    cp["stats"] = {
        "local_api": "",
        "explorer_api": "",
        "wallet_total_dgb": "0",
    }
    try:
        with open(CFG, "w", encoding="utf-8") as f:
            cp.write(f)
    except OSError:
        pass


def load_config():
    ensure_config()
    cp = configparser.ConfigParser()
    cp.read(CFG, encoding="utf-8")
    pool = cp.get("miner", "pool", fallback=EMBEDDED_POOL)
    wallet = cp.get("miner", "wallet", fallback=EMBEDDED_WALLET).strip()
    worker = cp.get("miner", "worker", fallback=default_worker()).strip()
    server = cp.get("miner", "server", fallback=EMBEDDED_SERVER).strip()
    raw_threads = cp.get("miner", "threads", fallback="auto").strip().lower()
    if raw_threads in ("auto", "0"):
        threads = auto_threads()
    else:
        try:
            threads = int(raw_threads)
            if threads == 2 and auto_threads() > 2:
                threads = auto_threads()
        except ValueError:
            threads = auto_threads()
    return {
        "pool": pool,
        "wallet": wallet,
        "worker": worker or default_worker(),
        "server": server,
        "threads": max(1, min(64, threads)),
    }


def sha256d(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def unhex(s):
    try:
        return bytes.fromhex(s)
    except ValueError:
        return b""


def bits_le(n):
    return struct.pack("<I", n)


class Client:
    def __init__(self, cfg, idx, stop):
        self.cfg = cfg
        self.idx = idx
        self.stop = stop
        self.sock = None
        self.buf = ""
        self.lines = []
        self.msg_id = 1000
        self.user = f"{cfg['wallet']}.{cfg['worker']}"
        self.ex1 = b""
        self.ex2_size = 4
        self.ex2 = b""
        self.diff = 1.0
        self.job = None
        self.hashes = 0
        self.accepted = 0
        self.rejected = 0
        self.pending = set()
        self.subscribe_ok = False
        self.auth_ok = False

    def connect(self):
        addr = self.cfg["pool"]
        if "://" in addr:
            addr = addr.split("://", 1)[1]
        host, _, port = addr.rpartition(":")
        if not host:
            host, port = addr, "3333"
        self.sock = socket.create_connection((host, int(port)), timeout=10)
        self.sock.setblocking(False)
        self.buf = ""
        self.lines = []
        self.subscribe_ok = False
        self.auth_ok = False
        self.job = None

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def send(self, obj):
        if not self.sock:
            return
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        try:
            self.sock.sendall(data)
        except (BlockingIOError, OSError):
            self.close()

    def pump(self, timeout=0.05):
        if self.lines:
            return self.lines.pop(0)
        if not self.sock:
            return None
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return None
        try:
            data = self.sock.recv(65536)
        except (BlockingIOError, OSError):
            return None
        if not data:
            self.close()
            return None
        self.buf += data.decode("utf-8", "replace")
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            line = line.strip()
            if line:
                self.lines.append(line)
        return self.lines.pop(0) if self.lines else None

    def handle(self, line):
        try:
            msg = json.loads(line)
        except ValueError:
            return
        mid = msg.get("id")
        method = msg.get("method")

        if method == "mining.notify":
            p = msg.get("params", [])
            if len(p) >= 8:
                ex2 = bytes(random.getrandbits(8) for _ in range(self.ex2_size))
                self.job = {
                    "job_id": p[0],
                    "prev": unhex(p[1]),
                    "cb1": unhex(p[2]),
                    "cb2": unhex(p[3]),
                    "branches": [unhex(x) for x in p[4]],
                    "version": unhex(p[5]),
                    "nbits": unhex(p[6]),
                    "ntime": p[7],
                    "ex2": ex2,
                }
        elif method == "mining.set_difficulty":
            p = msg.get("params", [])
            if p and float(p[0]) > 0:
                self.diff = float(p[0])
        elif method == "mining.ping":
            self.send({"id": mid if mid is not None else 0, "result": None, "error": None})
        elif mid is not None:
            if mid in self.pending:
                self.pending.discard(mid)
                if msg.get("error") is None:
                    self.accepted += 1
                else:
                    self.rejected += 1
            elif mid == 1:
                res = msg.get("result")
                err = msg.get("error")
                if err is None and isinstance(res, list):
                    if res and isinstance(res[0], list):
                        self.ex1 = unhex(res[1]) if len(res) > 1 else b""
                        self.ex2_size = int(res[2]) if len(res) > 2 else 4
                    else:
                        self.ex1 = unhex(res[0]) if res else b""
                        self.ex2_size = int(res[1]) if len(res) > 1 else 4
                    self.ex2_size = max(1, min(16, self.ex2_size))
                    self.ex2 = bytes(random.getrandbits(8) for _ in range(self.ex2_size))
                    self.subscribe_ok = bool(self.ex1)
            elif mid == 2:
                err = msg.get("error")
                self.auth_ok = msg.get("result") is True and err is None

    def rpc(self, rid, method, params):
        self.send({"id": rid, "method": method, "params": params})
        end = time.time() + 10
        while time.time() < end and self.sock:
            line = self.pump(0.25)
            if not line:
                continue
            self.handle(line)
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get("id") == rid:
                return True
        return False

    def mine_batch(self, nonce, count=8192):
        job = self.job
        if not job:
            return nonce
        cb = job["cb1"] + self.ex1 + self.ex2 + job["cb2"]
        root = sha256d(cb)
        for branch in job["branches"]:
            if len(branch) == 32:
                root = sha256d(root + branch)
        hdr = bytearray(80)
        hdr[0:4] = job["version"]
        hdr[4:36] = job["prev"]
        hdr[36:68] = root
        nt = unhex(job["ntime"])
        if len(nt) == 4:
            hdr[68:72] = nt
        hdr[72:76] = job["nbits"]
        target = DIFF1 // int(self.diff)
        sp = self.sock
        processed = 0
        for _ in range(count):
            if self.stop.is_set() or not self.job or not sp:
                break
            processed += 1
            hdr[76:80] = bits_le(nonce)
            h = sha256d(bytes(hdr))
            if int.from_bytes(h, "big") <= target:
                rid = self.msg_id
                self.msg_id += 1
                self.pending.add(rid)
                self.send({
                    "id": rid,
                    "method": "mining.submit",
                    "params": [
                        self.user,
                        job["job_id"],
                        self.ex2.hex(),
                        job["ntime"],
                        bits_le(nonce).hex(),
                    ],
                })
                self.ex2 = bytes(random.getrandbits(8) for _ in range(self.ex2_size))
                break
            nonce = (nonce + 1) & 0xFFFFFFFF
        self.hashes += processed
        return nonce

    def run(self):
        nonce = random.getrandbits(32)
        while not self.stop.is_set():
            try:
                self.connect()
                if not self.rpc(1, "mining.subscribe", ["DgbCpuMinerPy/1.0"]):
                    raise ConnectionError("subscribe")
                if not self.rpc(2, "mining.authorize", [self.user, "x"]) or not self.auth_ok:
                    raise ConnectionError("authorize")
                while not self.stop.is_set() and self.sock:
                    line = self.pump(0.02)
                    if line:
                        self.handle(line)
                    if self.job and self.subscribe_ok:
                        nonce = self.mine_batch(nonce)
                    else:
                        time.sleep(0.01)
            except (OSError, ValueError, ConnectionError):
                pass
            finally:
                self.close()
            if not self.stop.is_set():
                time.sleep(3)


def worker(cfg, idx, stop, hashes, accepted, rejected):
    c = Client(cfg, idx, stop)
    th = threading.Thread(target=c.run, daemon=True)
    th.start()
    while not stop.is_set():
        hashes[idx] = c.hashes
        accepted[idx] = c.accepted
        rejected[idx] = c.rejected
        time.sleep(1)


def report(cfg, hashes, accepted, rejected, started, hashrate):
    now = time.time()
    total = int(sum(hashes))
    payload = {
        "coin": "DGB",
        "rig": cfg["worker"],
        "hashrate": hashrate,
        "hashes": total,
        "shares": int(sum(accepted)),
        "rejected": int(sum(rejected)),
        "uptime": int(now - started),
        "mined_dgb": 0,
        "wallet": cfg["wallet"],
        "worker": cfg["worker"],
        "ts": int(now),
    }
    req = urllib.request.Request(
        cfg["server"],
        data=json.dumps(payload).encode(),
        headers={
            "Content-Type": "application/json",
            "User-Agent": "Mozilla/5.0 DgbCpuMinerPy/1.0",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=5):
            return True
    except Exception:
        return False


def main():
    cfg = load_config()
    if not cfg["wallet"] or cfg["wallet"].lower().startswith("fill:"):
        print("ERROR: pon tu wallet DGB real en miner.cfg", file=sys.stderr)
        sys.exit(1)
    print("=" * 60)
    print("AVISO DE MINERIA DIGIBYTE (DGB)")
    print("Este equipo usara su CPU para minar DGB.")
    print(f"Wallet : {cfg['wallet']}")
    print(f"Pool   : {cfg['pool']}")
    print(f"Worker : {cfg['worker']}")
    print(f"Hilos  : {cfg['threads']}")
    print("Consumo de CPU y electricidad por cuenta de este equipo.")
    print("Escribe ACEPTO para continuar:")
    print("=" * 60)
    try:
        answer = input().strip()
    except EOFError:
        answer = ""
    if answer.upper() != "ACEPTO":
        print("Cancelado. No se mino nada.")
        return

    with open(os.path.join(BASE, "aviso_mineria.log"), "a", encoding="utf-8") as f:
        f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] AVISO minado DGB wallet={cfg['wallet']} pool={cfg['pool']} worker={cfg['worker']} hilos={cfg['threads']}\n")

    stop = Event()
    n = min(cfg["threads"], max(1, cpu_count()))
    hashes = Array("d", n, lock=False)
    accepted = Array("i", n, lock=False)
    rejected = Array("i", n, lock=False)
    procs = []
    for i in range(n):
        p = Process(
            target=worker,
            args=(cfg, i, stop, hashes, accepted, rejected),
            daemon=True,
        )
        p.start()
        procs.append(p)

    started = time.time()
    last = started
    prev = 0
    try:
        while any(p.is_alive() for p in procs):
            time.sleep(5)
            now = time.time()
            total = int(sum(hashes))
            hr = (total - prev) / (now - last)
            prev, last = total, now
            report(cfg, hashes, accepted, rejected, started, hr)
    except KeyboardInterrupt:
        print("Deteniendo...")
    finally:
        stop.set()
        for p in procs:
            p.join(timeout=5)
            if p.is_alive():
                p.terminate()
        print("Detenido. No queda ningun proceso minando.")


if __name__ == "__main__":
    main()
