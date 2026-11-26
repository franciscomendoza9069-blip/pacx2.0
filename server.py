import configparser
import html
import json
import os
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BASE = os.path.dirname(os.path.abspath(__file__))

panel = configparser.ConfigParser()
panel.read(os.path.join(BASE, "panel.cfg"), encoding="utf-8")

PORT = panel.getint("panel", "port", fallback=8765)
HOST = panel.get("panel", "host", fallback="0.0.0.0")
WINDOW = panel.getint("panel", "active_window", fallback=20)
COIN = panel.get("display", "coin", fallback="DGB")
COIN_NAME = panel.get("display", "coin_name", fallback="DigiByte")
WALLET_TOTAL = panel.getfloat("wallet", "total_dgb", fallback=0.0)
BALANCE_API = panel.get("wallet", "balance_api", fallback="").strip()
FIAT = panel.get("prices", "fiat", fallback="EUR").strip().upper() or "EUR"
PRICE_API = panel.get("prices", "price_api", fallback="").strip()
PRICE_TTL = max(30, panel.getint("prices", "cache_seconds", fallback=120))

RIGS = {}
LOCK = threading.Lock()
BALANCE_CACHE = {"value": None, "ts": 0.0}
PRICE_CACHE = {"value": None, "ts": 0.0, "set": False}


def number(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def fmt_hr(value):
    v = number(value)
    for unit in ("H/s", "kH/s", "MH/s", "GH/s", "TH/s"):
        if v < 1000:
            return f"{v:.2f} {unit}"
        v /= 1000
    return f"{v:.2f} PH/s"


def fmt_time(seconds):
    s = int(number(seconds))
    h, rem = divmod(s, 3600)
    m, sec = divmod(rem, 60)
    if h:
        return f"{h}h {m}m {sec}s"
    if m:
        return f"{m}m {sec}s"
    return f"{sec}s"


def wallet_balance():
    if not BALANCE_API:
        return WALLET_TOTAL
    now = time.time()
    if BALANCE_CACHE["value"] is not None and now - BALANCE_CACHE["ts"] < 60:
        return BALANCE_CACHE["value"]
    try:
        with urllib.request.urlopen(BALANCE_API, timeout=5) as resp:
            raw = resp.read().decode("utf-8", "replace").strip()
        try:
            data = json.loads(raw)
        except ValueError:
            data = raw
        found = None
        if isinstance(data, (int, float)) and not isinstance(data, bool):
            found = number(data, None)
        elif isinstance(data, str):
            found = number(data, None)
        elif isinstance(data, dict):
            for key in ("balance", "total", "amount", "total_dgb", "balance_dgb"):
                if key in data:
                    found = number(data[key], None)
                    if found is not None:
                        break
        value = found if found is not None else WALLET_TOTAL
    except Exception:
        value = WALLET_TOTAL
    BALANCE_CACHE["value"] = value
    BALANCE_CACHE["ts"] = now
    return value


def dgb_price():
    if not PRICE_API:
        return None
    now = time.time()
    if PRICE_CACHE["set"] and now - PRICE_CACHE["ts"] < PRICE_TTL:
        return PRICE_CACHE["value"]
    value = None
    try:
        with urllib.request.urlopen(PRICE_API, timeout=5) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
        if isinstance(data, dict):
            node = data
            for key in ("digibyte", "dgb", COIN.lower()):
                nested = data.get(key)
                if isinstance(nested, dict):
                    node = nested
                    break
            for key in (FIAT.lower(), "price", "value", "last"):
                if key in node:
                    value = number(node[key], None)
                    if value is not None:
                        break
    except Exception:
        value = None
    PRICE_CACHE["value"] = value
    PRICE_CACHE["ts"] = now
    PRICE_CACHE["set"] = True
    return value


def collect():
    now = time.time()
    with LOCK:
        rigs = [dict(r) for r in RIGS.values() if now - number(r.get("ts")) <= WINDOW]
    active = len(rigs)
    hashrate = sum(number(r.get("hashrate")) for r in rigs)
    hashes = sum(number(r.get("hashes")) for r in rigs)
    shares = sum(number(r.get("shares")) for r in rigs)
    mined = sum(number(r.get("mined_dgb")) for r in rigs)
    uptime = max([number(r.get("uptime")) for r in rigs] or [0.0])
    rig_time = sum(number(r.get("uptime")) for r in rigs)
    wallet_total = wallet_balance()
    price = dgb_price()
    return {
        "coin": COIN,
        "coin_name": COIN_NAME,
        "fiat": FIAT,
        "price": price,
        "active_computers": active,
        "hashrate": hashrate,
        "hashrate_text": fmt_hr(hashrate),
        "total_hashes": hashes,
        "total_shares": shares,
        "mined_dgb": mined,
        "mined_fiat": mined * price if price is not None else None,
        "wallet_total_dgb": wallet_total,
        "wallet_fiat": wallet_total * price if price is not None else None,
        "active_time_seconds": uptime,
        "active_time": fmt_time(uptime),
        "total_rig_time_seconds": rig_time,
        "total_rig_time": fmt_time(rig_time),
        "rigs": sorted(rigs, key=lambda r: r.get("rig", "")),
        "updated": int(now),
    }


PAGE = """<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Panel %(coin)s</title>
<style>
:root{background:#0b0d12;color:#e8eefc;font-family:system-ui,sans-serif}
body{margin:0;padding:28px}
h1{margin:0 0 4px;font-size:26px}
.sub{color:#8fa2c0;margin-bottom:22px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:14px}
.card{background:#141926;border:1px solid #263049;border-radius:12px;padding:18px}
.label{color:#8fa2c0;font-size:13px;text-transform:uppercase;letter-spacing:.06em}
.value{font-size:28px;font-weight:700;margin-top:8px;word-break:break-all}
.subvalue{font-size:16px;color:#9fb3d1;margin-top:6px}
table{width:100%%;border-collapse:collapse;margin-top:24px;background:#141926;border-radius:12px;overflow:hidden}
th,td{text-align:left;padding:11px 14px;border-bottom:1px solid #263049;font-size:14px}
th{color:#8fa2c0;font-weight:600}
tr:last-child td{border-bottom:0}
.foot{color:#64748b;margin-top:20px;font-size:12px}
</style>
<script>
async function refresh(){
  const r = await fetch('/api/stats', {cache:'no-store'});
  const d = await r.json();
  document.getElementById('coin-name').textContent = d.coin_name;
  document.getElementById('rigs').textContent = d.active_computers;
  document.getElementById('hashrate').textContent = d.hashrate_text;
  document.getElementById('mined').textContent = d.mined_dgb.toFixed(6) + ' %(coin)s';
  document.getElementById('wallet').textContent = d.wallet_total_dgb.toFixed(6) + ' %(coin)s';
  document.getElementById('mined-fiat').textContent = fmtFiat(d.mined_fiat, d.fiat);
  document.getElementById('wallet-fiat').textContent = fmtFiat(d.wallet_fiat, d.fiat);
  document.getElementById('price').textContent = d.price === null ? 'n/d'
    : Number(d.price).toLocaleString('es-ES',{minimumFractionDigits:6,maximumFractionDigits:8}) + ' ' + d.fiat + '/' + d.coin;
  document.getElementById('uptime').textContent = d.active_time;
  document.getElementById('hashes').textContent = Math.round(d.total_hashes).toLocaleString();
  document.getElementById('shares').textContent = Math.round(d.total_shares).toLocaleString();
  const body = document.getElementById('rig-body');
  body.innerHTML = '';
  for (const rig of d.rigs) {
    const tr = document.createElement('tr');
    const euros = d.price === null ? 'n/d' : (rig.mined_dgb || 0) * d.price;
    for (const text of [rig.rig || '-', rig.hashrate_text || fmt(rig.hashrate),
                        Math.round(rig.shares || 0), fmtTime(rig.uptime),
                        (rig.mined_dgb || 0).toFixed(6), fmtFiat(euros, d.fiat)]) {
      const td = document.createElement('td');
      td.textContent = text;
      tr.appendChild(td);
    }
    body.appendChild(tr);
  }
}
function fmtFiat(v, fiat){
  if (v === null || v === undefined) return 'n/d';
  return Number(v).toLocaleString('es-ES',{minimumFractionDigits:2,maximumFractionDigits:2}) + ' ' + fiat;
}
function fmt(v){
  let x = Number(v) || 0;
  const u = ['H/s','kH/s','MH/s','GH/s','TH/s'];
  let i = 0;
  while (x >= 1000 && i < u.length - 1) { x /= 1000; i++; }
  return x.toFixed(2) + ' ' + u[i];
}
function fmtTime(s){
  s = Math.max(0, Math.floor(Number(s) || 0));
  const h = Math.floor(s / 3600), m = Math.floor((s %% 3600) / 60), sec = s %% 60;
  return h ? h + 'h ' + m + 'm ' + sec + 's' : (m ? m + 'm ' + sec + 's' : sec + 's');
}
setInterval(refresh, 5000);
refresh();
</script>
</head>
<body>
<h1>Panel de minado <span id="coin-name">%(coin_name)s</span></h1>
<div class="sub">Estadisticas locales enviadas por cada equipo. Actualizacion automatica cada 5 segundos.</div>
<div class="cards">
  <div class="card"><div class="label">Equipos activos</div><div class="value" id="rigs">0</div></div>
  <div class="card"><div class="label">Hashrate total</div><div class="value" id="hashrate">0 H/s</div></div>
  <div class="card"><div class="label">%(coin)s minados</div><div class="value" id="mined">0 %(coin)s</div><div class="subvalue" id="mined-fiat">n/d</div></div>
  <div class="card"><div class="label">Total en wallet</div><div class="value" id="wallet">0 %(coin)s</div><div class="subvalue" id="wallet-fiat">n/d</div></div>
  <div class="card"><div class="label">Precio %(coin)s</div><div class="value" id="price">n/d</div></div>
  <div class="card"><div class="label">Tiempo activo</div><div class="value" id="uptime">0s</div></div>
  <div class="card"><div class="label">Hashes totales</div><div class="value" id="hashes">0</div></div>
  <div class="card"><div class="label">Shares aceptadas</div><div class="value" id="shares">0</div></div>
</div>
<table>
<thead><tr><th>Equipo</th><th>Hashrate</th><th>Shares</th><th>Tiempo</th><th>%(coin)s minados</th><th>%(fiat)s</th></tr></thead>
<tbody id="rig-body"></tbody>
</table>
<div class="foot">Datos reportados por cada minero. Precio %(coin)s/%(fiat)s de prices.price_api (cache %(cache)ds). Si el precio no responde, los importes salen n/d.</div>
</body>
</html>
""" % {"coin": html.escape(COIN), "coin_name": html.escape(COIN_NAME),
       "fiat": html.escape(FIAT), "cache": PRICE_TTL}


class Handler(BaseHTTPRequestHandler):
    server_version = "OjbsDgbPanel/1.0"

    def send_json(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.split("?", 1)[0] == "/api/stats":
            self.send_json(200, collect())
            return
        if self.path.split("?", 1)[0] in ("/", "/index.html"):
            body = PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_json(404, {"error": "not found"})

    def do_POST(self):
        if self.path.split("?", 1)[0] != "/report":
            self.send_json(404, {"error": "not found"})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_json(400, {"error": "bad length"})
            return
        if size < 2 or size > 65536:
            self.send_json(400, {"error": "bad length"})
            return
        try:
            data = json.loads(self.rfile.read(size).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            self.send_json(400, {"error": "bad json"})
            return
        if not isinstance(data, dict):
            self.send_json(400, {"error": "bad json"})
            return

        rig = str(data.get("rig") or "pc")[:128]
        now = time.time()
        record = {
            "rig": rig,
            "hashrate": number(data.get("hashrate")),
            "hashrate_text": fmt_hr(data.get("hashrate")),
            "hashes": number(data.get("hashes")),
            "shares": number(data.get("shares")),
            "uptime": number(data.get("uptime")),
            "mined_dgb": number(data.get("mined_dgb")),
            "wallet": str(data.get("wallet") or "")[:256],
            "coin": str(data.get("coin") or COIN)[:16],
            "ts": now,
        }
        with LOCK:
            RIGS[rig] = record
            if len(RIGS) > 256:
                oldest = sorted(RIGS, key=lambda k: number(RIGS[k].get("ts")))[:len(RIGS) - 256]
                for key in oldest:
                    RIGS.pop(key, None)
        self.send_json(200, {"ok": True})

    def log_message(self, fmt, *args):
        pass


def main():
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"Panel {COIN_NAME} en http://{HOST}:{PORT}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
