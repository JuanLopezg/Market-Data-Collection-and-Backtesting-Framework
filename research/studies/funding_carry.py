# Usage (repository root): python3 research/studies/funding_carry.py <output.html>
"""Funding carry (delta-neutral): short perpetual + long spot on top-20 coins whose funding is high.

Rules (proposal 5, 2026-10-22):
- Universe each day: top 20 by SMA(quote volume, 25) of Binance USDT perpetuals.
- Signal at the end of day t: average funding of the last 3 days, annualized
  (sum of the day's settlements x 365). Enter when > 20%/year, exit when < 5%/year.
- Up to 10 pairs, 10% of equity each (spot leg funded, perp leg margined from the same 10%).
- PnL: the funding the short perp receives (negative funding is paid), from day t+1.
- Costs: entry and exit of both legs, perp taker 0.05% + spot taker 0.10% per side.
- Not modelled: spot/perp basis changes (taken as zero), borrow, margin calls on the short.
"""
import csv, json, math, sys, time, urllib.parse, urllib.request
from collections import defaultdict
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

DATA = 'storage/databases/1d_binance_perp_quote_20211001_to20261021.csv'
CACHE = Path('storage/databases/binance_funding_20211001.json')
START, END = date(2022, 1, 1), date(2026, 10, 21)
ENTER, EXIT, MAX_PAIRS, SIZE = 0.20, 0.05, 10, 0.10
COST_PER_SIDE = 0.0005 + 0.0010
OUT = sys.argv[1]

volume = defaultdict(dict)
for r in csv.DictReader(open(DATA, encoding='utf-8')):
    volume[r['symbol']][date.fromisoformat(r['date'])] = float(r['volume'])

def sma25(sym, d):
    vals = [volume[sym].get(d - timedelta(i)) for i in range(25)]
    return None if any(v is None for v in vals) else sum(vals) / 25

universe = {}
day = START - timedelta(5)
while day <= END:
    ranked = sorted(((v, s) for s in volume if (v := sma25(s, day)) is not None), reverse=True)[:20]
    universe[day] = [s for _, s in ranked]
    day += timedelta(1)
symbols = sorted({s for members in universe.values() for s in members})

# Funding settlements per symbol (cached; public endpoint, paced well below its limit).
funding = json.loads(CACHE.read_text()) if CACHE.exists() else {}
start_ms = int(datetime(2021, 12, 1, tzinfo=timezone.utc).timestamp() * 1000)
for sym in symbols:
    if sym in funding:
        continue
    rows, since = [], start_ms
    while True:
        url = f'https://fapi.binance.com/fapi/v1/fundingRate?symbol={urllib.parse.quote(sym)}&startTime={since}&limit=1000'
        page = json.load(urllib.request.urlopen(url, timeout=30))
        rows += [(int(p['fundingTime']), float(p['fundingRate'])) for p in page]
        time.sleep(0.7)
        if len(page) < 1000:
            break
        since = int(page[-1]['fundingTime']) + 1
    funding[sym] = rows
    CACHE.write_text(json.dumps(funding))
print(f'{len(symbols)} symbols, funding cached in {CACHE}', file=sys.stderr)

daily = defaultdict(lambda: defaultdict(float))       # symbol -> date -> sum of the day's rates
for sym, rows in funding.items():
    for ms, rate in rows:
        daily[sym][datetime.fromtimestamp(ms / 1000, timezone.utc).date()] += rate

def annualized(sym, d):
    days = [daily[sym].get(d - timedelta(i)) for i in range(3)]
    return None if any(v is None for v in days) else sum(days) / 3 * 365

equity, held, curve, rows_by_year = 1.0, set(), [], {}
day = START
while day <= END:
    # Today's PnL from yesterday's holdings: the short receives today's funding.
    pnl = sum(SIZE * daily[s].get(day, 0.0) for s in held)
    equity *= 1 + pnl
    # End of day: exits, then entries ranked by funding.
    rates = {s: annualized(s, day) for s in set(universe.get(day, [])) | held}
    leaving = {s for s in held if rates.get(s) is None or rates[s] < EXIT or s not in universe.get(day, [])}
    candidates = sorted((r, s) for s in universe.get(day, []) if s not in held and (r := rates.get(s)) is not None and r > ENTER)
    entering = [s for _, s in reversed(candidates)][:max(0, MAX_PAIRS - (len(held) - len(leaving)))]
    equity *= 1 - 2 * COST_PER_SIDE * SIZE * (len(leaving) + len(entering))
    held = (held - leaving) | set(entering)
    curve.append((day.isoformat(), equity, len(held)))
    day += timedelta(1)

prev = 1.0
years = []
for y in sorted({d[:4] for d, _, _ in curve}):
    last = [v for d, v, _ in curve if d.startswith(y)][-1]
    years.append((y, last / prev - 1)); prev = last
rets = [curve[i][1] / curve[i - 1][1] - 1 for i in range(1, len(curve))]
m = sum(rets) / len(rets); sd = (sum((x - m) ** 2 for x in rets) / len(rets)) ** .5
peak, dd = 0, 0
for _, v, _ in curve:
    peak = max(peak, v); dd = max(dd, 1 - v / peak)
avg_pairs = sum(n for *_, n in curve) / len(curve)
summary = {'years': years, 'total': curve[-1][1] - 1, 'cagr': curve[-1][1] ** (365 / len(curve)) - 1,
           'max_dd': dd, 'sharpe': m / sd * math.sqrt(365) if sd else 0, 'avg_pairs': avg_pairs}
print(json.dumps(summary))
x = json.dumps([d for d, _, _ in curve]); y = json.dumps([round(v * 100000, 2) for _, v, _ in curve])
n = json.dumps([k for *_, k in curve])
Path(OUT).write_text(f'''<!doctype html><meta charset="utf-8"><title>Funding carry</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<h3 style="font-family:sans-serif">P5 Funding carry: short perp + long spot, top 20, enter funding &gt; 20%/y, exit &lt; 5%/y, max 10 x 10%</h3>
<p style="font-family:sans-serif">Total {summary["total"]*100:+.1f}% · CAGR {summary["cagr"]*100:.1f}% · Max DD {dd*100:.1f}% · Sharpe {summary["sharpe"]:.2f} · average pairs {avg_pairs:.1f} · {" · ".join(f"{a} {b*100:+.1f}%" for a, b in years)}<br>Basis changes, borrow and margin calls are not modelled.</p>
<div id="c" style="height:480px"></div><div id="n" style="height:220px"></div>
<script>Plotly.newPlot('c',[{{x:{x},y:{y},type:'scatter',name:'Equity'}}],{{title:'Equity (start 100,000)'}});
Plotly.newPlot('n',[{{x:{x},y:{n},type:'scatter',fill:'tozeroy',name:'pairs held'}}],{{title:'Pairs held'}});</script>''')
