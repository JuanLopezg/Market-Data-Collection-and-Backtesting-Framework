#!/usr/bin/env python3
"""Bounded real-ingestion -> simulated-fill -> dashboard/restart acceptance, without Internet data."""
from datetime import datetime, timedelta, timezone
import http.cookiejar
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import time
import urllib.request

import yaml

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('paper_monitor', ROOT / 'deploy/paper_trading/monitor.py')
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)

# The current candle deliberately contains poisoned unfinished OHLCV. Its open
# alone may reach execution; no current/future candle may reach strategy SQLite.
FIXTURE = r'''package main
import("encoding/json";"net/http";"strconv";"time";"os")
func main(){
 today:=time.Now().UTC().Truncate(24*time.Hour)
 http.HandleFunc("/",func(w http.ResponseWriter,r *http.Request){
  w.Header().Set("Content-Type","application/json")
  switch r.URL.Path {
  case "/fapi/v1/exchangeInfo":
   json.NewEncoder(w).Encode(map[string]any{"symbols":[]any{
    map[string]string{"symbol":"BTCUSDT","contractType":"PERPETUAL","status":"TRADING","quoteAsset":"USDT"},
    map[string]string{"symbol":"ETHUSDT","contractType":"PERPETUAL","status":"TRADING","quoteAsset":"USDT"}}})
  case "/fapi/v1/ticker/24hr":
   json.NewEncoder(w).Encode([]any{map[string]string{"symbol":"BTCUSDT","quoteVolume":"2000000"},map[string]string{"symbol":"ETHUSDT","quoteVolume":"1000000"}})
  case "/fapi/v1/klines":
   start,_:=strconv.ParseInt(r.URL.Query().Get("startTime"),10,64)
   end,_:=strconv.ParseInt(r.URL.Query().Get("endTime"),10,64)
   limit,_:=strconv.Atoi(r.URL.Query().Get("limit"))
   rows:=[]any{}
   for day:=today.AddDate(0,0,-100);!day.After(today);day=day.AddDate(0,0,1){
    stamp:=day.UnixMilli();if stamp<start||stamp>end||len(rows)>=limit{continue}
    close:=100+int(day.Sub(today.AddDate(0,0,-100)).Hours()/24)
    if day.Equal(today){
     if _,err:=os.Stat("/fixture/missing-price");err==nil{continue}
     rows=append(rows,[]any{stamp,"200","999999","1","999999","999999",stamp+86399999,"999999999999"})
    }else{
     rows=append(rows,[]any{stamp,strconv.Itoa(close-1),strconv.Itoa(close+1),strconv.Itoa(close-2),strconv.Itoa(close),"100000",stamp+86399999,strconv.Itoa(close*100000-12345)})
    }
   };json.NewEncoder(w).Encode(rows)
  default:http.NotFound(w,r)
  }
 });if err:=http.ListenAndServe(":8000",nil);err!=nil{os.Exit(1)}
}'''


def run(args, directory, timeout=180):
    result = subprocess.run(args, cwd=ROOT, text=True, capture_output=True, timeout=timeout)
    with (directory / 'operations.log').open('a') as output:
        for line in (result.stdout + result.stderr).splitlines():
            output.write(('[REDACTED]' if monitor.SECRET.search(line) else line) + '\n')
    if result.returncode:
        raise RuntimeError('Command failed; inspect retained redacted evidence: ' + str(directory))
    return result.stdout.strip()


def main():
    hold = int(os.environ.get('PAPER_BROWSER_HOLD_SECONDS', '0'))
    if not 0 <= hold <= 300:
        raise ValueError('Browser hold must be between 0 and 300 seconds')
    recovery_enabled = os.environ.get('PAPER_RECOVERY_TEST', '0') == '1'
    shadow_enabled = os.environ.get('PAPER_SHADOW_TEST', '0') == '1'
    if recovery_enabled and hold:
        raise ValueError('Recovery and browser hold are separate fixture runs')
    if shadow_enabled and (recovery_enabled or hold):
        raise ValueError('Shadow rehearsal uses its own bounded fixture run')
    # Reject remote Docker contexts before creating or touching any containers.
    context = os.environ.get('DOCKER_CONTEXT')
    endpoint = None if context else os.environ.get('DOCKER_HOST')
    if not endpoint:
        endpoint = json.loads(subprocess.check_output(['docker','context','inspect', *([context] if context else [])], text=True))[0]['Endpoints']['docker']['Host']
    if not endpoint.startswith(('unix://', 'npipe://')):
        raise ValueError('Only a local Docker endpoint is allowed')
    project = 'algotrading-paper-' + secrets.token_hex(6)
    directory = ROOT / 'storage/paper_validation' / project
    directory.mkdir(parents=True)
    (directory / 'telemetry').mkdir()
    (directory / 'fixture.go').write_text(FIXTURE)
    # Reuse only a self-contained helper with byte-identical source. This avoids
    # recompiling Go's standard library on small VPS instances during repeated studies.
    cached_fixture = os.environ.get('PAPER_FIXTURE_BINARY')
    if cached_fixture:
        binary = Path(cached_fixture).resolve()
        if binary.parent.joinpath('fixture.go').read_text() != FIXTURE or not binary.read_bytes().startswith(b'\x7fELF'):
            raise ValueError('Cached fixture must be a Linux binary with identical fixture.go source')
        shutil.copy2(binary, directory / 'server')
    else:
        run(['docker','run','--rm','-e','CGO_ENABLED=0','-v',str(directory)+':/fixture',
             '-w','/fixture','golang:1.23','go','build','-o','/fixture/server','/fixture/fixture.go'], directory, timeout=600)
    document = yaml.safe_load((ROOT / 'deploy/paper_trading/docker-compose.yml').read_text())
    document['name'] = project
    document['networks']['paper'] = {'internal': True}
    document['networks']['web'] = {}
    for name, service in document['services'].items():
        # Optional fresh tags allow packaged-binary acceptance without changing the local PAPER stack.
        if service.get('image') == 'algotrading-runtime:paper' and os.environ.get('PAPER_RUNTIME_IMAGE'):
            service['image'] = os.environ['PAPER_RUNTIME_IMAGE']
        if name in ('dashboard-api', 'dashboard-watchdog', 'dashboard-alert-notifier') and os.environ.get('PAPER_API_IMAGE'):
            service['image'] = os.environ['PAPER_API_IMAGE']
        if 'build' in service:
            service['build']['context'] = str((ROOT / 'deploy/paper_trading' / service['build']['context']).resolve())
        if name == 'dashboard-web':
            if os.environ.get('PAPER_WEB_IMAGE'):
                service['image'] = os.environ['PAPER_WEB_IMAGE']
            service['networks'] = ['paper','web']
            service['ports'] = ['127.0.0.1::80']
    document['services']['fixture'] = {'image':'alpine:3.20','entrypoint':['/fixture/server'],
                                      'volumes':[str(directory)+':/fixture:ro'],'networks':['paper']}
    # This fixture never sends Telegram, even if the caller exported real settings.
    notifier_environment = document['services']['dashboard-alert-notifier']['environment']
    notifier_environment['DASHBOARD_NOTIFIER_SINK'] = 'TEST_FILE'
    notifier_environment['DASHBOARD_TELEGRAM_BOT_TOKEN'] = ''
    notifier_environment['DASHBOARD_TELEGRAM_CHAT_ID'] = ''
    document['services']['market-data']['command'].append('--run-once')
    document['services']['market-data']['restart'] = 'no'
    strategy_command = document['services']['strategy']['command']
    strategy_command[strategy_command.index('--market-top-n')+1] = '2'
    (directory / 'compose.yml').write_text(yaml.safe_dump(document, sort_keys=False))
    password = secrets.token_hex(16)
    (directory / '.env').write_text(f'PAPER_PROJECT={project}\nPAPER_RUN_DIR={directory}\n'
                                   f'POSTGRES_PASSWORD={secrets.token_hex(16)}\nDASHBOARD_VIEWER_PASSWORD={password}\n'
                                   'INITIAL_CASH=100000\nCOMMISSION_RATE=0.001\nMARKET_TOP_N=2\n')
    (directory / '.env').chmod(0o600)
    merged = json.loads(run(['docker','compose','--project-name',project,'--env-file',str(directory/'.env'),
                            '-f',str(ROOT/'deploy/paper_trading/docker-compose.yml'),
                            '-f',str(ROOT/'deploy/paper_trading/docker-compose.logging.yml'),
                            'config','--format','json'], directory))
    assert len(merged['services']) == 15
    for service in merged['services'].values():
        assert service['logging']['driver']=='syslog'
        assert service['logging']['options']['cache-max-size']=='25m'
        assert service['logging']['options']['cache-max-file']=='2'
    config = json.loads((ROOT / 'deploy/live/market_data_config.json').read_text())
    config.update(binance_base_url='http://fixture:8000', active_top_n=2, ranking_size=2,
                  stream='ALGOTRADING_PAPER_RUNTIME', publish_paper_execution_prices=True)
    (directory / 'market_data_config.json').write_text(json.dumps(config))
    dc = ['docker','compose','--project-name',project,'--env-file',str(directory / '.env'),'-f',str(directory / 'compose.yml')]
    def compose(*args):return run([*dc,*args],directory)
    def sql(statement):return compose('exec','-T','postgres','psql','-U','algotrading','-d','algotrading_paper','-Atq','-c',statement)
    def evidence():
        return {'fills':json.loads(sql("SELECT coalesce(json_agg(r ORDER BY fill_id),'[]'::json) FROM trading_fills r")),
                'account':json.loads(sql('SELECT snapshot::text FROM trading_runtime_state WHERE singleton=TRUE')),
                'backend':json.loads(sql('SELECT row_to_json(r) FROM simulated_exchange_state r')),
                'positions':json.loads(sql("SELECT coalesce(json_object_agg(coin,quantity),'{}'::json) FROM simulated_exchange_positions")),
                'pending':int(sql('SELECT count(*) FROM simulated_exchange_outbox WHERE NOT published'))}
    recovery = None
    if recovery_enabled:
        from paper_recovery_test import PaperRecoveryStudy
        recovery = PaperRecoveryStudy(directory, project, document, dc, compose, sql, evidence)
    try:
        compose('up','-d',*[name for name in document['services'] if name != 'market-data'])
        (directory/'missing-price').touch()
        compose('up','-d','market-data')
        deadline = time.monotonic()+30
        while time.monotonic()<deadline:
            state=json.loads(compose('ps','-a','--format','json','market-data'))
            if state['State']=='exited':
                assert state['ExitCode']==1
                break
            time.sleep(.5)
        else:raise RuntimeError('Missing price did not fail the bounded ingestion cycle')
        assert compose('exec','-T','dashboard-api','sqlite3','-readonly','/data/market/database.db','SELECT count(*) FROM ohlcv_data;')=='0'
        (directory/'missing-price').unlink()
        if recovery:
            recovery.before_ingestion()
        compose('up','-d','market-data')
        deadline = time.monotonic()+120
        before = None
        while time.monotonic()<deadline:
            try:
                before = evidence()
                if len(before['fills']) == 2 and before['pending']==0:break
            except (RuntimeError,ValueError,json.JSONDecodeError):pass
            time.sleep(1)
        else:raise RuntimeError('Expected two simulated fills were not observed')
        if recovery:
            recovery.confirm_trading_without_dashboard(before)
        today = datetime.now(timezone.utc).date()
        execution_day = int(today.strftime('%Y%m%d'))
        decision_day = int((today-timedelta(days=1)).strftime('%Y%m%d'))
        assert all(row['timestamp']==execution_day and row['price']==200 for row in before['fills'])
        assert before['account']['account_cash']==before['backend']['cash']
        assert before['account']['account_positions']==before['positions']
        assert sorted(before['account']['processed_fill_ids'])==sorted(row['fill_id'] for row in before['fills'])
        bars=json.loads(compose('exec','-T','dashboard-api','sqlite3','-readonly','-json','/data/market/database.db',
                      'SELECT count(*) AS rows,max(date) AS latest,max(close) AS max_close FROM ohlcv_data;'))[0]
        assert bars == {'rows':200,'latest':decision_day,'max_close':199}
        quote_rows=json.loads(compose('exec','-T','dashboard-api','sqlite3','-readonly','-json','/data/market/database.db',
            'SELECT count(quote_volume) AS rows,max(quote_volume) AS max_quote FROM ohlcv_data;'))[0]
        assert quote_rows == {'rows':200,'max_quote':199*100000-12345}
        for table in ('strategy_market_update_checkpoint','portfolio_risk_live_decision_checkpoint','order_planner_live_notional_checkpoint'):
            assert sql(f'SELECT count(*) FROM {table} WHERE timestamp={decision_day}')=='1'
        previous=monitor.cpu_ticks();time.sleep(.25)
        telemetry,_=monitor.sample(project,directory,monitor.SERVICES,previous)
        (directory/'telemetry/host.json').write_text(json.dumps(telemetry))
        port=json.loads(compose('ps','--format','json','dashboard-web'))['Publishers'][0]['PublishedPort']
        cookies=http.cookiejar.CookieJar()
        client=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookies))
        base=f'http://127.0.0.1:{port}'
        request=urllib.request.Request(base+'/api/auth/login',data=json.dumps({'username':'viewer','password':password}).encode(),headers={'Content-Type':'application/json'})
        client.open(request,timeout=10).close()
        market_data=json.load(client.open(base+'/api/market-data',timeout=15))
        assert market_data['liquidityLabel']=='SMA Quote Volume 25 (USDT)'
        assert market_data['signalCycleAligned'] and len(market_data['universe'])==2
        infrastructure=json.load(client.open(base+'/api/infrastructure',timeout=15))
        assert infrastructure['sourceMode']=='PAPER' and infrastructure['vps']['state']!='UNKNOWN'
        assert len(infrastructure['containers'])==13 and infrastructure['telemetryObservedAt']==telemetry['observedAt']
        assert infrastructure['vps']['clockObserved']==telemetry['vps']['clockObserved'] and infrastructure['exchange']['venue']=='SIMULATED'
        assert infrastructure['vps']['clockSynced']==telemetry['vps']['clockSynced']
        assert infrastructure['tradingProgress']['state']=='HEALTHY'
        assert not any(row['ready'] for row in infrastructure['services'])
        risk=json.load(client.open(base+'/api/risk',timeout=15))
        assert len(risk['configurationFingerprint'])==64 and len(risk['riskEvaluations'])==1
        evaluation=risk['riskEvaluations'][0]
        assert evaluation['state']=='EVALUATED' and evaluation['volatilityState']=='NOT_APPLICABLE'
        assert len(evaluation['assets'])==2 and all(row['sizedWeight']=='+10.0%' for row in evaluation['assets'])
        assert all(row['action']=='TARGET_WEIGHT' and row['reduction']=='No cap reduction' for row in evaluation['assets'])
        # Stale or stopped collection must not leave apparently current CPU/RAM.
        telemetry['observedAt']=(datetime.now(timezone.utc)-timedelta(seconds=40)).isoformat()
        (directory/'telemetry/host.json').write_text(json.dumps(telemetry));time.sleep(2.2)
        stale=json.load(client.open(base+'/api/infrastructure',timeout=15))
        assert stale['vps']['state']=='UNKNOWN' and not stale.get('telemetryObservedAt')
        monitor.capture_logs(project,directory/'algotrading/services')
        compose('restart','strategy','portfolio-risk','execution-state','order-planner','exchange-gateway','simulated-exchange')
        compose('up','-d','market-data');time.sleep(8)
        after=evidence()
        assert before['fills']==after['fills'] and before['account']['account_cash']==after['account']['account_cash']
        assert before['positions']==after['positions'] and before['backend']['next_fill_id']==after['backend']['next_fill_id']
        assert after['pending']==0
        # Restart must retain the same persisted evaluation, not append new economic fills.
        restarted_risk=json.load(client.open(base+'/api/risk',timeout=15))
        assert risk['riskEvaluations']==restarted_risk['riskEvaluations'] and risk['configurationFingerprint']==restarted_risk['configurationFingerprint']
        (directory/'accepted.json').write_text(json.dumps({'project':project,'decision_day':decision_day,
            'execution_day':execution_day,'completed_rows':200,'fills':before['fills'],
            'account_cash':before['account']['account_cash'],'positions':before['positions'],
            'host_telemetry':infrastructure['vps'],'containers':infrastructure['containers'],
            'restart_no_duplicate_fills':True,'stale_telemetry_unavailable':True,'missing_price_no_commit':True},indent=2)+'\n')
        print('PAPER-TRADING: PASS: completed bars, isolated open prices, fills/accounting, dashboard telemetry, stale collector and restart')
        print('Evidence:',directory)
        if recovery:
            recovery.run(client, base)
        if shadow_enabled:
            from shadow_trading_test import run_shadow_study
            run_shadow_study(directory, sql, risk, before, after)
        # Optional bounded handoff to an authenticated browser test. Removing the
        # marker releases this fixture; the deadline always stops its services.
        if hold:
            ready = directory / 'browser-ready.json'
            ready.write_text(json.dumps({'url':base, 'project':project}))
            print('Browser fixture ready:',directory,flush=True)
            deadline = time.monotonic() + hold
            while ready.exists() and time.monotonic() < deadline:
                time.sleep(.5)
            ready.unlink(missing_ok=True)
    finally:
        # Stop only this random owned fixture project. Retain volumes and evidence.
        compose('stop')
        if shadow_enabled:
            # Remove only this random owned fixture's containers/networks. Retain
            # volumes and reports; do not touch the normal PAPER notifier or VPS.
            compose('down', '--remove-orphans')


if __name__=='__main__':main()
