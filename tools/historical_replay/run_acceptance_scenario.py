#!/usr/bin/env python3
"""Reusable T18/T19/T20/T21/T22/T24 historical replay scenario runner.

Uses the T17 execution bridge and the isolated deploy/historical_replay topology.
No shared logical clock is introduced. Business time is TimeHandler env only.
"""
from __future__ import annotations
import argparse, csv, datetime as dt, hashlib, json, math, os, pathlib, re, shlex, subprocess, sys, time
from typing import Dict, Iterable, List, Optional, Tuple

RUNTIME = ["simulated-exchange","exchange-gateway","execution-state","order-planner","portfolio-risk","strategy"]
ONE_SHOT = {"market-data-volume-init","market-data-db-init"}
SHORT_START = "2020-04-10T12:00:00Z"
SHORT_TARGET = 20200415
FULL_START = "2020-01-01T12:00:00Z"
FULL_TARGET = 20251012
FULL_HISTORY_STRUCTURAL_BASELINE = {
    "strategy_days": 2112,
    "closed_trades": 630,
    "open_campaigns": 1,
}

def die(msg: str) -> None: raise SystemExit("ERROR: "+msg)
def cmd_text(cmd): return " ".join(shlex.quote(str(x)) for x in cmd)
def run(cmd, cwd: pathlib.Path, capture=False, check=True, env=None) -> str:
    print("+ "+cmd_text(cmd), flush=True)
    p=subprocess.run([str(x) for x in cmd],cwd=str(cwd),text=True,stdout=subprocess.PIPE if capture else None,stderr=subprocess.STDOUT if capture else None,check=False,env=env)
    if check and p.returncode:
        if capture and p.stdout: print(p.stdout,file=sys.stderr)
        die(f"command failed ({p.returncode}): {cmd_text(cmd)}")
    return p.stdout or ""

def envfile(path: pathlib.Path)->Dict[str,str]:
    out={}
    if not path.exists(): return out
    for raw in path.read_text().splitlines():
        s=raw.strip()
        if not s or s.startswith('#') or '=' not in s: continue
        k,v=s.split('=',1); out[k.strip()]=v.strip().strip('"').strip("'")
    return out

def compose_base(root): return ["docker","compose","--env-file",str(root/"deploy/historical_replay/.env"),"-f",str(root/"deploy/historical_replay/docker-compose.yml")]
def compose(root,args,capture=False,check=True): return run([*compose_base(root),*args],root,capture,check)
def pg_rows(root,sql):
    e=envfile(root/"deploy/historical_replay/.env"); u=e.get("POSTGRES_USER","algotrading"); d=e.get("POSTGRES_DB","algotrading_historical_replay")
    out=compose(root,["exec","-T","postgres","psql","-U",u,"-d",d,"-Atqc",sql],capture=True,check=False)
    return [x.strip() for x in out.splitlines() if x.strip()]
def pg_scalar(root,sql):
    r=pg_rows(root,sql); return r[-1] if r else ""
def pg_int(root,sql):
    try:return int(pg_scalar(root,sql) or 0)
    except:return 0

def parse_utc(v): return dt.datetime.strptime(v,"%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
def parse_day(v): return dt.datetime.strptime(str(v),"%Y%m%d").date()
def ymd(d): return int(d.strftime("%Y%m%d"))
def next_day(v): return ymd(parse_day(v)+dt.timedelta(days=1))
def day_gap(newer, older):
    if not newer or not older: return None
    return (parse_day(newer)-parse_day(older)).days
def expected_wall(start,target,speed):
    visible=dt.datetime.combine(parse_day(target)+dt.timedelta(days=1),dt.time(),tzinfo=dt.timezone.utc)
    return (visible-parse_utc(start)).total_seconds()/speed

def timehandler_completed_day(root: pathlib.Path)->int:
    """Return the latest fully completed simulated UTC day from the immutable TimeHandler env.

    This is the capacity reference. It intentionally does not scrape feeder logs: at high
    speed their bounded log tail can lag several business days behind the actual simulated
    clock and produce bogus negative/positive capacity measurements.
    """
    e=envfile(root/"deploy/historical_replay/run/time.env")
    try:
        speed=float(e['ALGOTRADING_TIME_SPEED'])
        real_ref=parse_utc(e['ALGOTRADING_TIME_REAL_REFERENCE_UTC'])
        sim_ref=parse_utc(e['ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC'])
    except (KeyError,ValueError) as exc:
        die(f"invalid TimeHandler env for capacity measurement: {exc}")
    real_now=dt.datetime.now(dt.timezone.utc)
    sim_now=sim_ref+(real_now-real_ref)*speed
    return ymd(sim_now.date()-dt.timedelta(days=1))

def capacity_observation(canonical:int,cp:dict,observation_start:int):
    """Build one steady-state capacity sample, or None while bootstrap is still catching up."""
    if not canonical or (observation_start and canonical<observation_start):
        return None
    stage_latest=[
      int(cp.get('strategy_latest',0) or 0),
      int(cp.get('risk_latest',0) or 0),
      int(cp.get('planner_latest',0) or 0),
      int(cp.get('execution_last_decision',0) or 0),
    ]
    decision_latest=min(stage_latest)
    # Bootstrap is not a capacity sample. Arm the gate only once every decision stage
    # has reached the requested observation date. If that never happens, min-samples/span
    # remain unsatisfied and the scenario still fails.
    if decision_latest<=0 or (observation_start and decision_latest<observation_start):
        return None
    raw_lag=day_gap(canonical,decision_latest)
    if raw_lag is None:
        return None
    # A negative value means the durable pipeline is already ahead of this sampling
    # instant. Capacity lag cannot be negative, so treat that as zero lag.
    return {
      'canonical':canonical,
      'pipeline_latest':decision_latest,
      'raw_lag_days':raw_lag,
      'lag_days':max(0,raw_lag),
    }

def normalized_dataset(root: pathlib.Path)->Tuple[pathlib.Path,dict]:
    e=envfile(root/"deploy/historical_replay/.env")
    src=pathlib.Path(e.get("HISTORICAL_DATA_PATH","../../storage/databases/1d_cmc.csv"))
    if not src.is_absolute(): src=(root/"deploy/historical_replay"/src).resolve()
    if not src.exists(): die(f"dataset not found: {src}")
    dst=root/"deploy/historical_replay/run/1d_cmc_by_date.csv"
    out=run([sys.executable,str(root/"tools/historical_replay/normalize_historical_csv.py"),"--input",str(src),"--output",str(dst)],root,capture=True).strip()
    print(out)
    return dst,json.loads(out.splitlines()[-1])

def create_time_env(root,speed,start):
    p=root/"deploy/historical_replay/run/time.env"; p.parent.mkdir(parents=True,exist_ok=True)
    run([sys.executable,str(root/"tools/historical_replay/create_time_env.py"),"--speed",format(speed,'.17g'),"--simulated-reference",start,"--output",str(p)],root)

def container_failures(root, allowed_stopped=frozenset()):
    out=compose(root,["ps","--all"],capture=True,check=False); bad=[]
    for line in out.splitlines():
        low=line.lower()
        if any(x in low for x in ONE_SHOT) and "exited (0)" in low: continue
        if any(s in low for s in allowed_stopped) and "exited (0)" in low: continue
        if any(x in low for x in ("restarting","unhealthy","dead","exited")): bad.append(line)
    return "\n".join(bad)

def wait_runtime(root, seconds=10, allowed_stopped=frozenset()):
    end=time.monotonic()+seconds
    last=""
    while time.monotonic()<end:
        last=container_failures(root,allowed_stopped)
        if not last: return
        time.sleep(1)
    if last: die("runtime did not recover:\n"+last)

def feeder_logs(root,tail=5000): return compose(root,["logs","--no-color",f"--tail={tail}","historical-market-data"],capture=True,check=False)
def latest_committed(root):
    vals=[int(x) for x in re.findall(r"event=historical_day_committed\s+date=(\d{8})(?:\s|$)",feeder_logs(root,tail=5000))]
    return max(vals) if vals else 0

def committed(root,date):
    """Return whether the sequential canonical feeder has progressed through date.

    Do not wait for one exact log line to remain in a bounded Docker log tail. At replay
    speeds a specific day's line can disappear between polling iterations. The historical
    feeder is sequential, so observing any later committed day proves progression through
    the requested fault-trigger boundary.
    """
    return latest_committed(root) >= int(date)

def snapshot(root)->dict:
    raw=pg_scalar(root,"SELECT COALESCE(snapshot::text,'{}') FROM trading_runtime_state WHERE singleton=TRUE;")
    try:return json.loads(raw or '{}')
    except:return {}
def checkpoints(root)->dict:
    s=snapshot(root)
    return {
      "strategy_latest":pg_int(root,"SELECT COALESCE(MAX(timestamp),0) FROM strategy_market_update_checkpoint;"),
      "strategy_days":pg_int(root,"SELECT COUNT(*) FROM strategy_market_update_checkpoint;"),
      "risk_latest":pg_int(root,"SELECT COALESCE(MAX(timestamp),0) FROM portfolio_risk_live_decision_checkpoint;"),
      "risk_days":pg_int(root,"SELECT COUNT(*) FROM portfolio_risk_live_decision_checkpoint;"),
      "planner_latest":pg_int(root,"SELECT COALESCE(MAX(timestamp),0) FROM order_planner_live_notional_checkpoint;"),
      "planner_days":pg_int(root,"SELECT COUNT(*) FROM order_planner_live_notional_checkpoint;"),
      "execution_last_decision":int(s.get("last_bar_close_timestamp",0) or 0),
      "execution_last_execution":int(s.get("last_execution_timestamp",0) or 0),
      "cash":float(s.get("account_cash",0) or 0),
      "positions":s.get("account_positions",{}) or {},
      "tracked_orders":len(s.get("orders",[]) or []),
      "fills":pg_int(root,"SELECT COUNT(*) FROM trading_fills;"),
      "sim_exchange_latest":pg_int(root,"SELECT COALESCE(MAX(latest_timestamp),0) FROM simulated_exchange_state;"),
    }

def print_status(cp,target,label):
    print(f"STATUS label={label} target={target} strategy={cp['strategy_latest']} risk={cp['risk_latest']} planner={cp['planner_latest']} execution_decision={cp['execution_last_decision']} execution_at={cp['execution_last_execution']} fills={cp['fills']} positions={len(cp['positions'])}",flush=True)

def prepare(root,dataset,speed,start,reuse_image):
    os.environ["HISTORICAL_DATA_PATH"]=str(dataset)
    if not reuse_image:
        run([str(root/"deploy/historical_replay/build_runtime_bundle.sh")],root)
        # placeholder lets compose resolve env_file during image build
        te=root/"deploy/historical_replay/run/time.env"; te.parent.mkdir(parents=True,exist_ok=True); te.write_text("# build placeholder\n")
        compose(root,["--profile","runtime","build"])
    compose(root,["--profile","runtime","down","-v","--remove-orphans"],check=False)
    compose(root,["up","-d","nats","postgres"])
    compose(root,["run","--rm","market-data-volume-init"])
    create_time_env(root,speed,start)
    compose(root,["--profile","runtime","run","--rm","market-data-db-init"])
    compose(root,["--profile","runtime","up","-d","--no-build",*RUNTIME])
    time.sleep(8)
    wait_runtime(root,4)
    compose(root,["--profile","runtime","up","-d","--no-build","historical-market-data"])

def fault_action(root,profile,state):
    # Short scenarios are anchored to Apr 2020. Each action is deliberately shorter than one business day.
    if profile=="none": return
    if profile=="restart-all" and not state.get("restart") and committed(root,20200412):
        print("FAULT restart-all: restarting six runtime consumers",flush=True)
        compose(root,["restart",*RUNTIME]); time.sleep(8); wait_runtime(root,6); state["restart"]=True
    elif profile=="strategy-gap":
        if not state.get("stopped") and committed(root,20200411):
            print("FAULT strategy-gap: stopping strategy until canonical DB reaches 20200413",flush=True)
            compose(root,["stop","strategy"]); state["stopped"]=True
        if state.get("stopped") and not state.get("resumed") and committed(root,20200413):
            print("FAULT strategy-gap: starting strategy for persistent-state catch-up",flush=True)
            compose(root,["start","strategy"]); time.sleep(5); state["resumed"]=True
    elif profile=="chaos":
        if not state.get("nats") and committed(root,20200411):
            print("CHAOS: restart NATS JetStream",flush=True); compose(root,["restart","nats"]); time.sleep(6); state["nats"]=True
        if state.get("nats") and not state.get("pg") and committed(root,20200412):
            print("CHAOS: restart PostgreSQL",flush=True); compose(root,["restart","postgres"]); time.sleep(8); state["pg"]=True
        if state.get("pg") and not state.get("exec") and committed(root,20200413):
            cid=compose(root,["ps","-q","execution-state"],capture=True).strip()
            if cid: run(["docker","kill",cid],root,check=False)
            print("CHAOS: SIGKILL execution-state (restart policy must recover)",flush=True); time.sleep(8); state["exec"]=True
        if state.get("exec") and not state.get("sim") and committed(root,20200414):
            print("CHAOS: restart simulated-exchange",flush=True); compose(root,["restart","simulated-exchange"]); time.sleep(6); state["sim"]=True

def get_plans(root):
    rows=pg_rows(root,"SELECT timestamp,plan_payload FROM order_planner_live_notional_checkpoint ORDER BY timestamp;")
    out=[]
    for row in rows:
        try: ts,payload=row.split('|',1)
        except ValueError: continue
        obj=json.loads(payload)
        out.append({"timestamp":int(ts),"cancel_order_ids":obj.get("cancel_order_ids",[]),"submit_orders":obj.get("submit_orders",[]),"global_target_notional_usd":obj.get("global_target_notional_usd",{})})
    return out

def get_fills(root):
    rows=pg_rows(root,"SELECT fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission FROM trading_fills ORDER BY fill_id;")
    out=[]
    for row in rows:
        p=row.split('|')
        if len(p)!=9: continue
        out.append({"fill_id":int(p[0]),"order_id":int(p[1]),"strategy_id":int(p[2]),"timestamp":int(p[3]),"coin":p[4],"side":int(p[5]),"quantity":float(p[6]),"price":float(p[7]),"commission":float(p[8])})
    return out

def norm_float(x): return float(format(float(x),'.12g'))

def fill_cash_delta(fill):
    gross=float(fill['quantity'])*float(fill['price'])
    commission=float(fill.get('commission',0.0) or 0.0)
    return -(gross+commission) if int(fill['side'])==0 else (gross-commission)

def fill_position_delta(fill):
    qty=float(fill['quantity'])
    return qty if int(fill['side'])==0 else -qty

def bounded_economic_state(current_snapshot, all_fills, execution_cutoff):
    """Reconstruct account state exactly at execution_cutoff.

    Scenario runs can overshoot by a few business days before the feeder is stopped.
    Infer the fresh-run initial state by reversing every persisted fill from the current
    durable snapshot, then replay only fills visible through the requested cutoff.
    """
    current_cash=float(current_snapshot.get('account_cash',0) or 0)
    current_positions={k:float(v) for k,v in (current_snapshot.get('account_positions',{}) or {}).items()}

    initial_cash=current_cash-sum(fill_cash_delta(f) for f in all_fills)
    initial_positions=dict(current_positions)
    for f in all_fills:
        coin=f['coin']
        initial_positions[coin]=initial_positions.get(coin,0.0)-fill_position_delta(f)

    bounded=[f for f in all_fills if int(f['timestamp'])<=int(execution_cutoff)]
    cash=initial_cash+sum(fill_cash_delta(f) for f in bounded)
    positions=dict(initial_positions)
    for f in bounded:
        coin=f['coin']
        positions[coin]=positions.get(coin,0.0)+fill_position_delta(f)
    positions={k:v for k,v in positions.items() if abs(v)>1e-12}
    return cash,positions,bounded

def normalize_economic_plans(plans,target_completed=None):
    out=[]
    for p in plans:
        if target_completed is not None and int(p['timestamp'])>int(target_completed):
            continue
        cancels=list(p.get("cancel_order_ids",[]) or [])
        submits=[]
        for o in (p.get("submit_orders",[]) or []):
            submits.append({k:(norm_float(v) if isinstance(v,float) else v) for k,v in sorted(o.items()) if k not in {"metadata"}})
        # A durable no-op checkpoint is operationally useful, but it is not an
        # economic action. Startup scheduling may create one at one speed and
        # not another, so exclude it from the speed-invariance fingerprint.
        if not cancels and not submits:
            continue
        out.append({"timestamp":int(p["timestamp"]),"cancel_order_ids":cancels,"submit_orders":submits})
    return out

def normalize_external_fingerprint(obj):
    obj=dict(obj or {})
    horizon=obj.get("horizon",{}) or {}
    target_completed=horizon.get("target_completed")
    normalized={
      "horizon":horizon,
      "plans":normalize_economic_plans(obj.get("plans",[]) or [],target_completed),
      "fills":obj.get("fills",[]) or [],
      "cash":obj.get("cash"),
      "positions":obj.get("positions",{}) or {},
    }
    return normalized

def fingerprint_hash(obj):
    raw=json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
    return hashlib.sha256(raw).hexdigest()

def fingerprint(root,target_completed=None,target_execution_day=None):
    plans=normalize_economic_plans(get_plans(root),target_completed)

    all_fills=get_fills(root)
    s=snapshot(root)
    if target_execution_day is None:
        bounded_fills=all_fills
        cash=float(s.get('account_cash',0) or 0)
        positions={k:float(v) for k,v in (s.get('account_positions',{}) or {}).items()}
    else:
        cash,positions,bounded_fills=bounded_economic_state(s,all_fills,target_execution_day)

    fills=[{k:(norm_float(v) if isinstance(v,float) else v) for k,v in f.items()} for f in bounded_fills]
    obj={
      "horizon":{"target_completed":target_completed,"target_execution_day":target_execution_day},
      "plans":plans,
      "fills":fills,
      "cash":norm_float(cash),
      "positions":{k:norm_float(v) for k,v in sorted(positions.items())},
    }
    return obj,fingerprint_hash(obj)

def trade_metrics(fills):
    # Exact Python translation of TradeRecorder's long/short campaign accounting.
    open_pos={}; closed=0; pnl=0.0
    for f in sorted(fills,key=lambda x:(x['timestamp'],x['fill_id'])):
        key=(f['strategy_id'],f['coin']); signed=f['quantity'] if f['side']==0 else -f['quantity']; rem=signed; rem_comm=f['commission']
        while abs(rem)>1e-15:
            st=open_pos.get(key)
            if st is None:
                open_pos[key]={"net":rem,"cash_flow":-rem*f['price'],"commission":rem_comm}
                break
            if (st['net']>0 and rem>0) or (st['net']<0 and rem<0):
                st['cash_flow']-=rem*f['price']; st['commission']+=rem_comm; st['net']+=rem; break
            close_qty=min(abs(rem),abs(st['net'])); fill_qty=abs(signed); close_comm=f['commission']*(close_qty/fill_qty) if fill_qty else 0.0; close_signed=close_qty if rem>0 else -close_qty
            st['cash_flow']-=close_signed*f['price']; st['commission']+=close_comm; st['net']+=close_signed; rem-=close_signed; rem_comm=max(0.0,rem_comm-close_comm)
            if abs(st['net'])<=1e-15:
                pnl+=st['cash_flow']-st['commission']; closed+=1; del open_pos[key]
    return {"closed_trades":closed,"closed_pnl":pnl,"closed_pnl_rounded":round(pnl),"open_campaigns":len(open_pos)}

def full_baseline(root,cp):
    # T19b is the structural/full-history gate. Economic equivalence is enforced
    # separately and more strongly by T19c, which reconstructs 631 trades from
    # persisted distributed fills and compares them against research RealTest.
    #
    # Do NOT use planner checkpoint count as market cycles: planner legitimately
    # has no checkpoint on no-op/non-actionable days (e.g. 2020-01-01, 2020-03-27).
    # Likewise raw order/fill counts and this helper's aggregate PnL are execution-
    # mechanics diagnostics, not the economic reference after exact-FLAT sizing.
    plans=get_plans(root); fills=get_fills(root); orders=sum(len(p['submit_orders']) for p in plans); tm=trade_metrics(fills)
    actual={
        "strategy_days":cp['strategy_days'],
        "closed_trades":tm['closed_trades'],
        "open_campaigns":tm['open_campaigns'],
        # Informational diagnostics retained in evidence, but not used as the
        # economic equivalence gate. T19c owns that responsibility.
        "planner_days":cp['planner_days'],
        "orders":orders,
        "fills":len(fills),
        "closed_pnl":tm['closed_pnl'],
        "closed_pnl_rounded":tm['closed_pnl_rounded'],
    }
    errors=[]
    for k,v in FULL_HISTORY_STRUCTURAL_BASELINE.items():
        if actual[k]!=v: errors.append(f"{k}: expected {v} got {actual[k]}")
    return {
        "ok":not errors,
        "expected":FULL_HISTORY_STRUCTURAL_BASELINE,
        "actual":actual,
        "errors":errors,
        "economic_equivalence_gate":"T19c exact research RealTest policy",
    }

def save(root,label,report):
    out=root/"deploy/historical_replay/run"/f"{label}_summary.json"; out.parent.mkdir(parents=True,exist_ok=True); out.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    log=root/"deploy/historical_replay/run"/f"{label}_logs_tail.txt"; log.write_text(compose(root,["logs","--no-color","--tail=5000"],capture=True,check=False))
    return out

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--root',default='.')
    ap.add_argument('--label',required=True)
    ap.add_argument('--speed',type=float,required=True)
    ap.add_argument('--simulated-start',default=SHORT_START)
    ap.add_argument('--target-completed',type=int,default=SHORT_TARGET)
    ap.add_argument('--fault-profile',choices=['none','restart-all','strategy-gap','chaos'],default='none')
    ap.add_argument('--reuse-image',action='store_true')
    ap.add_argument('--max-wall-seconds',type=float,default=1800)
    ap.add_argument('--drain-seconds',type=float,default=180)
    ap.add_argument('--full-baseline',action='store_true')
    ap.add_argument('--expected-fingerprint')
    ap.add_argument('--capacity-check',action='store_true',help='require runtime checkpoints to keep pace with canonical committed days during the observation window')
    ap.add_argument('--capacity-observation-start',type=int,default=0,help='YYYYMMDD; ignore lag samples before this canonical day')
    ap.add_argument('--capacity-max-lag-days',type=int,default=2)
    ap.add_argument('--capacity-min-samples',type=int,default=6)
    ap.add_argument('--capacity-min-span-days',type=int,default=0,help='minimum simulated-day span covered by steady-state capacity samples')
    args=ap.parse_args(); root=pathlib.Path(args.root).resolve()
    data,meta=normalized_dataset(root); os.environ['HISTORICAL_DATA_PATH']=str(data)
    if meta['min_date']>parse_utc(args.simulated_start).date().isoformat(): die('dataset begins after simulated start')
    if meta['max_date']<parse_day(next_day(args.target_completed)).isoformat(): die('dataset lacks required T+1 open')
    exp=expected_wall(args.simulated_start,args.target_completed,args.speed)
    print(f"SCENARIO {args.label}: speed={args.speed:g} start={args.simulated_start} target={args.target_completed} expected_clock_wall={exp:.1f}s fault={args.fault_profile}")
    prepare(root,data,args.speed,args.simulated_start,args.reuse_image)
    started=time.monotonic(); next_status=started; next_capacity_sample=started; state={}; reached=False
    capacity_samples=[]; capacity_max_lag=0; capacity_armed_at=0
    while time.monotonic()-started<args.max_wall_seconds:
        fault_action(root,args.fault_profile,state)
        if committed(root,args.target_completed): reached=True; break
        now=time.monotonic()
        if args.capacity_check and now>=next_capacity_sample:
            canonical=timehandler_completed_day(root)
            ccp=checkpoints(root)
            sample=capacity_observation(canonical,ccp,args.capacity_observation_start)
            if sample is None:
                decision_latest=min(ccp['strategy_latest'],ccp['risk_latest'],ccp['planner_latest'],ccp['execution_last_decision'])
                print(f"CAPACITY_WARMUP label={args.label} canonical={canonical} pipeline_latest={decision_latest} observation_start={args.capacity_observation_start}",flush=True)
            else:
                if not capacity_armed_at:
                    capacity_armed_at=sample['canonical']
                    print(f"CAPACITY_ARMED label={args.label} canonical={sample['canonical']} pipeline_latest={sample['pipeline_latest']}",flush=True)
                capacity_max_lag=max(capacity_max_lag,sample['lag_days'])
                capacity_samples.append(sample)
                print(f"CAPACITY label={args.label} canonical={sample['canonical']} pipeline_latest={sample['pipeline_latest']} raw_lag_days={sample['raw_lag_days']} lag_days={sample['lag_days']} max_lag_days={capacity_max_lag}",flush=True)
            next_capacity_sample=now+5
        if now>=next_status:
            cp=checkpoints(root); print_status(cp,args.target_completed,args.label)
            allowed={'strategy'} if args.fault_profile=='strategy-gap' and state.get('stopped') and not state.get('resumed') else set()
            bad=container_failures(root,allowed)
            if bad and args.fault_profile not in {'chaos'}: die('container failure:\n'+bad)
            next_status=now+30
        time.sleep(2)
    if not reached:
        rep={"result":"FAIL","reason":"target_not_committed",**checkpoints(root)}; save(root,args.label,rep); return 1
    compose(root,["stop","historical-market-data"])
    exec_day=next_day(args.target_completed); end=time.monotonic()+args.drain_seconds; cp={}
    while time.monotonic()<end:
        cp=checkpoints(root); print_status(cp,args.target_completed,args.label)
        if cp['strategy_latest']>=args.target_completed and cp['risk_latest']>=args.target_completed and cp['planner_latest']>=args.target_completed and cp['execution_last_decision']>=args.target_completed and cp['execution_last_execution']>=exec_day and cp['sim_exchange_latest']>=exec_day and cp['fills']>0: break
        time.sleep(3)
    fp_obj,fp_hash=fingerprint(root,args.target_completed,exec_day)
    logs=compose(root,["logs","--no-color","--tail=5000"],capture=True,check=False)
    critical_tokens=['event=reconciliation_blocked','event=notional_plan_invalid','event=notional_plan_state_mismatch','event=submit_conflict','event=fill_failed']
    critical=[x for x in logs.splitlines() if any(t in x for t in critical_tokens)]
    pipeline=cp.get('strategy_latest',0)>=args.target_completed and cp.get('risk_latest',0)>=args.target_completed and cp.get('planner_latest',0)>=args.target_completed and cp.get('execution_last_decision',0)>=args.target_completed and cp.get('execution_last_execution',0)>=exec_day and cp.get('sim_exchange_latest',0)>=exec_day and cp.get('fills',0)>0
    gap_ok=True
    if args.fault_profile=='strategy-gap':
        # The aggregate --tail=5000 can evict Strategy's catch-up marker because
        # historical-market-data is intentionally very chatty. Observe the
        # service that owns the event instead of relying on a shared log tail.
        strategy_logs=compose(root,["logs","--no-color","--tail=5000","strategy"],capture=True,check=False)
        gap_ok=bool(state.get('stopped') and state.get('resumed') and 'signal_state_catchup_processed' in strategy_logs)
    baseline=None
    if args.full_baseline: baseline=full_baseline(root,cp)
    capacity_ok=True; capacity_span_days=0
    if args.capacity_check:
        if capacity_samples:
            capacity_span_days=max(0,day_gap(capacity_samples[-1]['canonical'],capacity_samples[0]['canonical']) or 0)
        capacity_ok=(len(capacity_samples)>=args.capacity_min_samples and capacity_span_days>=args.capacity_min_span_days and capacity_max_lag<=args.capacity_max_lag_days)
    fp_match=True; expected_hash=None
    if args.expected_fingerprint:
        expected=json.loads(pathlib.Path(args.expected_fingerprint).read_text())
        if isinstance(expected.get('fingerprint'),dict):
            expected_obj=normalize_external_fingerprint(expected['fingerprint'])
            expected_hash=fingerprint_hash(expected_obj)
        else:
            expected_hash=expected.get('fingerprint_sha256')
        fp_match=(expected_hash==fp_hash)
    result='PASS' if pipeline and not critical and gap_ok and capacity_ok and (baseline is None or baseline['ok']) and fp_match else 'FAIL'
    rep={"result":result,"label":args.label,"speed":args.speed,"simulated_start":args.simulated_start,"target_completed":args.target_completed,"target_execution_day":exec_day,"fault_profile":args.fault_profile,"fault_state":state,"wall_seconds":round(time.monotonic()-started,3),"expected_clock_wall_seconds":round(exp,3),"critical_log_lines":critical,"gap_catchup_observed":gap_ok,"capacity_check":args.capacity_check,"capacity_ok":capacity_ok,"capacity_observation_start":args.capacity_observation_start,"capacity_max_lag_days_allowed":args.capacity_max_lag_days,"capacity_max_lag_days_observed":capacity_max_lag,"capacity_min_samples_required":args.capacity_min_samples,"capacity_min_span_days_required":args.capacity_min_span_days,"capacity_span_days_observed":capacity_span_days,"capacity_armed_at":capacity_armed_at,"capacity_samples":capacity_samples,"fingerprint_sha256":fp_hash,"fingerprint":fp_obj,"expected_fingerprint_sha256":expected_hash,"fingerprint_match":fp_match,"full_baseline":baseline,**cp}
    out=save(root,args.label,rep)
    compose(root,["stop",*RUNTIME],check=False)
    print(json.dumps({k:v for k,v in rep.items() if k!='fingerprint'},indent=2,sort_keys=True))
    print('evidence='+str(out))
    if result!='PASS': print(f"FAIL: {args.label}",file=sys.stderr); return 1
    print(f"PASS: {args.label}")
    return 0
if __name__=='__main__': raise SystemExit(main())
