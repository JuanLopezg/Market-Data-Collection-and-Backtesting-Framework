"""Isolated local PostgreSQL + authenticated real-provider HTTP acceptance."""
from pathlib import Path
import http.cookiejar,json,os,secrets,subprocess,time,urllib.request,urllib.error
from datetime import datetime,timedelta,timezone
root=Path(__file__).resolve().parents[1]
name='algotrading-risk-read-'+secrets.token_hex(4)
run=root/'storage/pipeline_risk_validation'/name;run.mkdir(parents=True)
pg=name+'-pg';api=name+'-api';password=secrets.token_hex(16)
def command(*args,input=None):return subprocess.check_output(args,input=input,text=True,stderr=subprocess.DEVNULL).strip()
def sql(query):return command('docker','exec','-i',pg,'psql','-X','-U','postgres','-d','fixture','-v','ON_ERROR_STOP=1','-Atq',input=query)
def literal(v):return "'"+v.replace("'","''")+"'"
def payload(v):return literal(json.dumps(v))
def meta(key):return {'schema_version':1,'message_id':key,'correlation_id':'fixture-cycle','produced_at':20261007}
# Refuse remote Docker contexts; this fixture never targets the running VPS.
context=os.environ.get('DOCKER_CONTEXT')
endpoint=None if context else os.environ.get('DOCKER_HOST')
if not endpoint:
 endpoint=json.loads(command('docker','context','inspect',*([context] if context else [])))[0]['Endpoints']['docker']['Host']
if not endpoint.startswith(('unix://','npipe://')):
 raise ValueError('Only a local Docker endpoint is allowed')
try:
 (run/'postgres.env').write_text('POSTGRES_PASSWORD='+password+'\nPOSTGRES_DB=fixture\n');(run/'postgres.env').chmod(0o600)
 command('docker','run','-d','--name',pg,'--network','bridge','--env-file',str(run/'postgres.env'),'postgres:16-alpine')
 for _ in range(60):
  try:sql('SELECT 1;');break
  except subprocess.CalledProcessError:time.sleep(.5)
 else:raise RuntimeError('Fixture PostgreSQL did not start')
 sql('''CREATE TABLE portfolio_risk_service_metadata(state_key text,portfolio_config text);
 CREATE TABLE portfolio_risk_live_decision_checkpoint(state_key text,timestamp bigint,signals_payload text,account_payload text,decision_payload text);
 CREATE TABLE strategy_market_update_checkpoint(state_key text,timestamp bigint,update_payload text);
 CREATE TABLE order_planner_live_notional_checkpoint(state_key text,timestamp bigint,request_payload text,plan_payload text);
 CREATE TABLE trading_runtime_state(singleton bool,updated_at timestamptz,snapshot jsonb,schema_version int DEFAULT 1);''')
 identity=(root/'config/portfolio/pure_rsi_equal_weight.json').read_text()+'\nmarket_data_mode=canonical-sqlite-v1'
 sql('INSERT INTO portfolio_risk_service_metadata VALUES (\'portfolio-risk-live-sqlite-v1\','+literal(identity)+');')
 account={'metadata':meta('account'),'timestamp':20261007,'cash':100000,'positions':{},'strategy_positions':{'1':{}}}
 signals={'metadata':meta('signals'),'timestamp':20261007,'strategies':[{'strategy_id':1,'strategy_name':'Pure_RSI','signals':{'BTC':0}}]}
 decision={'metadata':meta('decision'),'decision_timestamp':20261007,'strategies':[]}
 sql('INSERT INTO portfolio_risk_live_decision_checkpoint VALUES (\'portfolio-risk-live-sqlite-v1\',20261007,'+','.join(payload(v) for v in [signals,account,decision])+');')
 update={'metadata':meta('market'),'completed_through':20261007,'source':'fixture','timeframe':'1d','active_top_n':2}
 sql('INSERT INTO strategy_market_update_checkpoint VALUES (\'strategy-service-market-db-v1\',20261007,'+payload(update)+');')
 request={'metadata':meta('request'),'decision_timestamp':20261007,'decisions':decision,'reference_closes':{'date':20261007,'closes':{'BTC':50000}},'state':{'state_revision':1,'strategy_ids':[1],'strategy_positions':[],'orders':[]}}
 plan={'metadata':meta('plan'),'decision_timestamp':20261007,'state_revision':1,'global_target_notional_usd':{},'submit_orders':[],'cancel_order_ids':[]}
 sql('INSERT INTO order_planner_live_notional_checkpoint VALUES (\'order-planner-live-notional-v1\',20261007,'+payload(request)+','+payload(plan)+');')
 today=datetime.now(timezone.utc).date()
 snapshot={'schema_version':1,'account_cash':100000,'account_positions':{},'orders':[],
           'last_bar_close_timestamp':int((today-timedelta(days=1)).strftime('%Y%m%d')),
           'last_execution_timestamp':int(today.strftime('%Y%m%d'))}
 sql('INSERT INTO trading_runtime_state(singleton,updated_at,snapshot) VALUES (TRUE,now(),'+payload(snapshot)+');')
 telemetry={'schemaVersion':1,'project':name,'observedAt':datetime.now(timezone.utc).isoformat(),'scope':'Isolated test host observations',
            'vps':{'state':'HEALTHY','cpuPct':12,'ramPct':25,'diskPct':30,'clockObserved':True,'clockSynced':True,'clockOffsetLabel':'Fixture systemd NTP status; offset not measured'},
            'containers':[{'name':'strategy','state':'RUNNING','health':'HEALTHY','cpuPct':-1,'ramMb':-1}],
            'processes':[{'service':'strategy','processRunning':True,'processState':'RUNNING','detail':'Fixture executable observation'}]}
 def write_telemetry():
  telemetry['observedAt']=datetime.now(timezone.utc).isoformat()
  (run/'host.json').write_text(json.dumps(telemetry))
 write_telemetry()
 ip=command('docker','inspect','-f','{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}',pg)
 (run/'api.env').write_text('DASHBOARD_AUTH_ALLOW_DEMO=true\nDASHBOARD_DATA_PROVIDER=real\nDASHBOARD_POSTGRES_DSN=host='+ip+' dbname=fixture user=postgres password='+password+' sslmode=disable\nDASHBOARD_RUNTIME_MODE=PAPER\nDASHBOARD_EXECUTION_VENUE=SIMULATED\nDASHBOARD_VENUE_TARGET_ENVIRONMENT=PAPER\nDASHBOARD_EXCHANGE_GATEWAY_MODE=backend\n')
 (run/'api.env').chmod(0o600)
 with (run/'api.env').open('a') as file:file.write('DASHBOARD_HOST_METRICS_FILE=/telemetry/host.json\nDASHBOARD_HOST_METRICS_PROJECT='+name+'\n')
 command('docker','run','-d','--name',api,'--network','bridge','--env-file',str(run/'api.env'),'-v',str(run)+':/telemetry:ro','-p','127.0.0.1::8080',os.environ.get('DASHBOARD_TEST_API_IMAGE','algotrading-pipeline-risk-api'))
 port=json.loads(command('docker','inspect',api))[0]['NetworkSettings']['Ports']['8080/tcp'][0]['HostPort']
 base='http://127.0.0.1:'+port
 client=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
 login=urllib.request.Request(base+'/api/auth/login',data=json.dumps({'username':'viewer','password':'viewer-demo'}).encode(),headers={'Content-Type':'application/json'})
 for _ in range(60):
  try:client.open(login,timeout=2).close();break
  except (urllib.error.URLError,TimeoutError):time.sleep(.5)
 else:raise RuntimeError('Fixture API did not start')
 def read(path):return json.load(client.open(base+'/api/'+path,timeout=15))
 risk=read('risk');pipeline=read('pipeline')
 infrastructure=read('infrastructure')
 assert infrastructure['tradingProgress']['state']=='HEALTHY'
 assert infrastructure['vps']['clockObserved'] and infrastructure['vps']['clockSynced']
 process=next(row for row in infrastructure['services'] if row['service']=='strategy')
 assert process['processState']=='RUNNING' and process['processRunning'] and not process['ready']
 telemetry['processes'][0].update(processState='MISSING',processRunning=False)
 telemetry['vps']['clockSynced']=False
 write_telemetry();time.sleep(2.2)
 infrastructure=read('infrastructure')
 assert next(row for row in infrastructure['dependencies'] if row['component']=='Trading service liveness')['state']=='CRITICAL'
 assert next(row for row in infrastructure['dependencies'] if row['component']=='Clock sync')['state']=='CRITICAL'
 snapshot['last_execution_timestamp']=int((today-timedelta(days=2)).strftime('%Y%m%d'))
 sql('UPDATE trading_runtime_state SET updated_at=now(),snapshot='+payload(snapshot)+';')
 time.sleep(2.2)
 assert read('infrastructure')['tradingProgress']['state']=='CRITICAL'
 telemetry['observedAt']=(datetime.now(timezone.utc)-timedelta(seconds=40)).isoformat()
 (run/'host.json').write_text(json.dumps(telemetry));time.sleep(2.2)
 infrastructure=read('infrastructure')
 assert infrastructure['vps']['state']=='UNKNOWN' and not infrastructure.get('telemetryObservedAt')
 assert risk['riskState']=='NO NEW TARGETS' and risk['accountCashLabel']=='$100000.00' and risk['activeLimitsAvailable'] and risk['policies'][0]['grossLimitLabel']=='150.0%'
 assert pipeline['evidenceState']=='OBSERVED' and pipeline['rows'][0]['signal']=='FLAT' and pipeline['rows'][0]['approvedTargetLabel']=='HOLD / no new target'
 # Old schemas/cycles remain readable. New reports must use the same risk anchor/configuration.
 assert not risk.get('riskEvaluations')
 sql('ALTER TABLE portfolio_risk_live_decision_checkpoint ADD COLUMN risk_diagnostics_payload text;')
 report={'schema_version':1,'timestamp':20261007,'configuration_identity':identity,'strategies':[{
  'strategy_id':1,'name':'Pure_RSI','sizer':'EqualWeight','reference_capital':100000,'sizing_available':True,
  'max_gross_leverage':1.5,'max_asset_weight':1.5,'gross_after_asset_cap':0,'gross_scale':1,
  'volatility_state':'NOT_APPLICABLE','volatility':None,
  'assets':{'BTC':{'sized_weight':0,'asset_capped_weight':0,'approved_weight':0,'current_quantity':0,'action':'HOLD','reductions':[]}}}]}
 sql('UPDATE portfolio_risk_live_decision_checkpoint SET risk_diagnostics_payload='+payload(report)+';')
 evaluated=read('risk')
 assert len(evaluated['configurationFingerprint'])==64 and evaluated['riskEvaluations'][0]['assets'][0]['action']=='HOLD'
 assert evaluated['riskEvaluations'][0]['volatilityTarget']=='Not applicable'
 report['timestamp']=20261008
 sql('UPDATE portfolio_risk_live_decision_checkpoint SET risk_diagnostics_payload='+payload(report)+';')
 try:read('risk');raise AssertionError('Cross-cycle risk diagnostics accepted')
 except urllib.error.HTTPError as error:assert error.code>=500
 report['timestamp']=20261007
 sql('UPDATE portfolio_risk_live_decision_checkpoint SET risk_diagnostics_payload='+payload(report)+';')
 sql('DELETE FROM order_planner_live_notional_checkpoint;')
 pipeline=read('pipeline');assert pipeline['evidenceState']=='PENDING' and pipeline['rows'][0]['plannedAction']=='Planner pending'
 account['timestamp']=20261008
 sql('UPDATE portfolio_risk_live_decision_checkpoint SET account_payload='+payload(account)+';')
 try:read('risk');raise AssertionError('Mismatched timestamp accepted')
 except urllib.error.HTTPError as error:assert error.code>=500
 sql('DELETE FROM portfolio_risk_live_decision_checkpoint;')
 assert read('risk')['evidenceKind']=='PENDING' and read('pipeline')['evidenceState']=='PENDING'
 assert sql('SELECT snapshot->>\'account_cash\' FROM trading_runtime_state;')=='100000'
 (run/'accepted.json').write_text(json.dumps({'quiet_cycle':True,'persisted_policy':True,'missing_planner_pending':True,'mismatched_timestamp_rejected':True,'absent_decision_pending':True,'account_cash_preserved':100000},indent=2)+'\n')
 print('PIPELINE-RISK-SQL-HTTP: PASS: old-schema compatibility, durable same-cycle/config Risk evaluation and rejected cross-cycle diagnostics; quiet/HOLD, planner/decision evidence, Infrastructure observations and preserved account')
finally:
 for container in [api,pg]:
  subprocess.run(['docker','rm','-f','-v',container],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=False)
