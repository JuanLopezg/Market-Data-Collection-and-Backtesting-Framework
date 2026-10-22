# Usage (repository root): python3 research/studies/long_short_momentum.py <output.html> <rebalance_days>
"""Weekly cross-sectional 7-day momentum, long top 5 / short bottom 5 of the top-30 liquidity
universe. Signal at close(t), trades at open(t+1), held 7 days, open-to-open returns."""
import csv, json, math, sys
from collections import defaultdict
from datetime import date, timedelta

OLD = 'storage/databases/1d_binance_perp_quote_20191101.csv'      # to 2026-10-15
NEW = 'storage/databases/1d_binance_perp_quote_20211001_to20261021.csv'
FEE, N_SIDE, WEIGHT, UNIVERSE = 0.0005, 2, 0.25, 20
HOLD = int(sys.argv[2])
START, END = date(2021, 1, 1), date(2026, 10, 21)
OUT = sys.argv[1]

bars = defaultdict(dict)   # symbol -> date -> (open, close, quote volume)
for path, keep in ((OLD, lambda d: d < date(2021, 10, 1)), (NEW, lambda d: True)):
    for r in csv.DictReader(open(path, encoding='utf-8')):
        d = date.fromisoformat(r['date'])
        if keep(d):
            bars[r['symbol']][d] = (float(r['open']), float(r['close']), float(r['volume']))

def sma_volume(sym, d):
    vals = [bars[sym].get(d - timedelta(i)) for i in range(25)]
    return None if any(v is None for v in vals) else sum(v[2] for v in vals) / 25

equity, curve, weights, last_rebalance = 1.0, [], {}, None
day = START
while day <= END:
    nxt = day + timedelta(1)
    # Today's open-to-next-open return of the weights set at today's open.
    if weights:
        ret = 0.0
        for sym, w in weights.items():
            a, b = bars[sym].get(day), bars[sym].get(nxt)
            if a and b and a[0] > 0:
                ret += w * (b[0] / a[0] - 1)
        equity *= 1 + ret
        # Drift: weights move with prices until the next rebalance.
        weights = {s: w * (bars[s][nxt][0] / bars[s][day][0]) / (1 + ret)
                   if bars[s].get(day) and bars[s].get(nxt) else w for s, w in weights.items()}
    curve.append((nxt.isoformat(), equity))
    # Rebalance on the close of `nxt - 1` = today: decide at close(day), trade at open(nxt).
    if last_rebalance is None or (day - last_rebalance).days >= HOLD:
        ranked = []
        for sym, series in bars.items():
            today, past, tomorrow = series.get(day), series.get(day - timedelta(7)), series.get(nxt)
            vol = sma_volume(sym, day)
            if today and past and tomorrow and vol and past[1] > 0:
                ranked.append((vol, sym, today[1] / past[1] - 1))
        universe = sorted(ranked, reverse=True)[:UNIVERSE]
        if len(universe) >= 2 * N_SIDE:
            by_return = sorted(universe, key=lambda x: x[2])
            target = {s: -WEIGHT for _, s, _ in by_return[:N_SIDE]}
            target.update({s: WEIGHT for _, s, _ in by_return[-N_SIDE:]})
            turnover = sum(abs(target.get(s, 0) - weights.get(s, 0)) for s in set(target) | set(weights))
            equity *= 1 - FEE * turnover
            curve[-1] = (curve[-1][0], equity)
            weights, last_rebalance = target, day
    day = nxt

# Yearly table and summary.
by_year, prev = {}, 1.0
for d, v in curve:
    by_year[d[:4]] = v
rows = []
for y in sorted(by_year):
    rows.append((y, by_year[y] / prev - 1)); prev = by_year[y]
rets = [curve[i][1] / curve[i - 1][1] - 1 for i in range(1, len(curve))]
m = sum(rets) / len(rets); sd = (sum((x - m) ** 2 for x in rets) / len(rets)) ** .5
peak, dd = 0, 0
for _, v in curve:
    peak = max(peak, v); dd = max(dd, 1 - v / peak)
total = curve[-1][1] - 1
cagr = curve[-1][1] ** (365 / len(curve)) - 1
summary = {'years': rows, 'total': total, 'cagr': cagr, 'max_dd': dd, 'sharpe': m / sd * math.sqrt(365)}
print(json.dumps(summary))

labels = json.dumps([d for d, _ in curve]); values = json.dumps([round(v * 100000, 2) for _, v in curve])
open(OUT, 'w').write(f'''<!doctype html><meta charset="utf-8"><title>Long/short 7-day momentum</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<h3 style="font-family:sans-serif">Long top 2 / short bottom 2 by 7-day return, top-20 liquidity, rebalanced every {HOLD} day(s), 25% each, fee 0.05%</h3>
<p style="font-family:sans-serif">Total {total*100:+.1f}% · CAGR {cagr*100:.1f}% · Max DD {dd*100:.1f}% · Sharpe {summary["sharpe"]:.2f} · {" · ".join(f"{y} {r*100:+.1f}%" for y, r in rows)}</p>
<div id="c" style="height:520px"></div><div id="d" style="height:260px"></div>
<script>const x={labels},y={values};let p=0;const dd=y.map(v=>(p=Math.max(p,v),(v/p-1)*100));
Plotly.newPlot('c',[{{x,y,type:'scatter',name:'Equity (start 100,000)'}}],{{title:'Equity',yaxis:{{type:'log'}}}});
Plotly.newPlot('d',[{{x,y:dd,type:'scatter',fill:'tozeroy',name:'Drawdown %'}}],{{title:'Drawdown (%)'}});</script>''')
