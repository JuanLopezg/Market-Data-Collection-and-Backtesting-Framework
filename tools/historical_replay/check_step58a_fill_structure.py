#!/usr/bin/env python3
from __future__ import annotations
import argparse,csv,json,math
from pathlib import Path

KEYS=("order_id","strategy_id","timestamp","coin","side")


def load(path:Path,end_date:int|None=None):
    with path.open(newline='') as f:
        rows=list(csv.DictReader(f))
    required={"fill_id","order_id","strategy_id","timestamp","coin","side","quantity","price","commission"}
    if rows and set(rows[0].keys()) != required:
        raise SystemExit(f"[FAIL] unexpected fill CSV schema: {path}")
    if end_date is not None:
        rows=[r for r in rows if int(r["timestamp"])<=end_date]
    return rows


def main()->int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--candidate',required=True)
    ap.add_argument('--reference',required=True)
    ap.add_argument('--end-date',type=int)
    ap.add_argument('--summary')
    args=ap.parse_args()
    cand=load(Path(args.candidate))
    ref=load(Path(args.reference),args.end_date)
    if len(cand)!=len(ref):
        raise SystemExit(f"[FAIL] structural fill count changed: candidate={len(cand)} reference={len(ref)}")
    price_diffs=[];qty_diffs=[]
    for i,(a,b) in enumerate(zip(cand,ref),1):
        for key in KEYS:
            if a[key]!=b[key]:
                raise SystemExit(f"[FAIL] structural fill #{i} {key}: candidate={a[key]!r} reference={b[key]!r}")
        apx=float(a['price']); bpx=float(b['price']); aq=float(a['quantity']); bq=float(b['quantity'])
        if not all(map(math.isfinite,(apx,bpx,aq,bq))) or apx<=0 or bpx<=0 or aq<=0 or bq<=0:
            raise SystemExit(f"[FAIL] non-finite/non-positive economics at fill #{i}")
        price_diffs.append(abs(apx-bpx)/abs(bpx)*100.0)
        qty_diffs.append(abs(aq-bq)/abs(bq)*100.0)
    summary={
        'result':'PASS','candidateFillCount':len(cand),'referenceFillCount':len(ref),
        'structuralFields':list(KEYS),'maxPriceDiffPct':max(price_diffs,default=0.0),
        'maxQuantityDiffPct':max(qty_diffs,default=0.0),
        'economicMagnitudesInformational':True,
    }
    if args.summary:
        p=Path(args.summary);p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n')
    print('PASS: STEP58A fill structure matches frozen T19 sequence '
          f"fills={len(cand)} max_price_diff_pct={summary['maxPriceDiffPct']:.9f} "
          f"max_qty_diff_pct={summary['maxQuantityDiffPct']:.9f}")
    return 0

if __name__=='__main__':
    raise SystemExit(main())
