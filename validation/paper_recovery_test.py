"""Recovery acceptance used by PAPER_RECOVERY_TEST=1 paper_trading_test.py.

All faults, backups and fresh-volume restoration belong to that random local
fixture. This is a maintenance-stop restore study, not a production backup daemon.
"""
import hashlib
import http.cookiejar
import json
import subprocess
import time
import urllib.error
import urllib.request

import yaml


DASHBOARD = ('dashboard-web', 'dashboard-api', 'dashboard-watchdog', 'dashboard-alert-notifier')
TRADING = ('strategy', 'portfolio-risk', 'execution-state', 'order-planner', 'exchange-gateway', 'simulated-exchange')


class PaperRecoveryStudy:
    def __init__(self, directory, project, document, dc, compose, sql, evidence):
        self.directory, self.project, self.document = directory, project, document
        self.dc, self.compose, self.sql, self.evidence = dc, compose, sql, evidence
        self.results = {}
        self.backup = directory / 'backup'
        self.backup.mkdir(mode=0o700)
        if not project.startswith('algotrading-paper-') or document['name'] != project:
            raise ValueError('Recovery requires an owned isolated PAPER fixture')

    def command(self, arguments, *, destination=None, source=None):
        # Binary dumps must never go through the fixture's text operations log.
        with destination.open('wb') if destination else open('/dev/null', 'wb') as output:
            with source.open('rb') if source else open('/dev/null', 'rb') as input_file:
                result = subprocess.run(arguments, stdin=input_file, stdout=output if destination else subprocess.PIPE,
                                        stderr=subprocess.PIPE, timeout=120)
        if result.returncode:
            raise RuntimeError('Recovery command failed; no secrets or binary output are printed')
        if destination:
            destination.chmod(0o600)
        return result.stdout.decode().strip() if not destination else None

    def wait(self, probe, description, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if probe():
                    return
            except (RuntimeError, ValueError, json.JSONDecodeError, urllib.error.URLError):
                pass
            time.sleep(.5)
        raise RuntimeError('Recovery timed out: ' + description)

    def assert_failed_ingestion(self, expected_rows=0):
        def exited():
            state = json.loads(self.compose('ps', '-a', '--format', 'json', 'market-data'))
            if state['State'] != 'exited':
                return False
            assert state['ExitCode'] == 1, 'Disconnected ingestion must fail'
            return True
        self.wait(exited, 'failed ingestion', timeout=45)
        rows = self.command(['docker', 'run', '--rm', '--network', 'none', '--user', '0:0',
            '--volumes-from', self.project + '-strategy-1:ro', '--entrypoint', 'sqlite3',
            self.document['services']['dashboard-api']['image'], '-readonly', '/data/market/database.db',
            'SELECT count(*) FROM ohlcv_data;'])
        assert rows == str(expected_rows), 'Disconnected ingestion retained unexpected completed data'
        assert self.sql('SELECT count(*) FROM trading_fills') == '0'

    def before_ingestion(self):
        self.compose('stop', *DASHBOARD)
        # Stop the source on the same internal network: Docker DNS/HTTP becomes
        # unavailable, rather than returning fabricated error JSON.
        self.compose('stop', 'fixture')
        self.compose('up', '-d', 'market-data')
        self.assert_failed_ingestion()
        self.results['market_http_outage_no_commit'] = True
        self.compose('start', 'fixture')
        self.compose('stop', 'nats')
        # --no-deps is essential: Compose must not repair the fault before testing.
        self.compose('up', '-d', '--no-deps', 'market-data')
        # SQLite commits completed bars before publication. Broker failure retains
        # those bars; retry must publish them once without changing their meaning.
        self.assert_failed_ingestion(expected_rows=200)
        self.results['nats_outage_completed_bars_retained_no_fills'] = True
        self.compose('start', 'nats')

    def confirm_trading_without_dashboard(self, state):
        running = self.compose('ps', '--status', 'running', '--services').splitlines()
        assert not set(DASHBOARD).intersection(running)
        assert len(state['fills']) == 2 and state['pending'] == 0
        assert state['account']['account_cash'] == state['backend']['cash']
        assert state['account']['account_positions'] == state['positions']
        self.results['two_fills_with_all_dashboard_services_stopped'] = True
        self.compose('start', *DASHBOARD)

    def economic_state(self, state):
        account = state['account']
        return {'fills':state['fills'], 'cash':account['account_cash'], 'positions':state['positions'],
                'processed_fill_ids':sorted(account['processed_fill_ids']),
                'next_order_id':account['next_order_id'], 'next_fill_id':state['backend']['next_fill_id'],
                'last_bar_close_timestamp':account['last_bar_close_timestamp'],
                'last_execution_timestamp':account['last_execution_timestamp'], 'pending':state['pending']}

    def volume_digest(self, volume):
        info = json.loads(self.command(['docker', 'volume', 'inspect', volume]))[0]
        assert info['Labels']['com.docker.compose.project'] in (self.project, self.project + '-restore')
        manifest = self.command(['docker', 'run', '--rm', '--network', 'none',
            '--mount', f'type=volume,src={volume},dst=/source,readonly', 'alpine:3.20',
            'sh', '-c', 'cd /source && find . -type f -exec sha256sum {} + | sort'])
        return hashlib.sha256(manifest.encode()).hexdigest()

    def database_digest(self, sql):
        tables = json.loads(sql("SELECT coalesce(json_agg(tablename ORDER BY tablename),'[]'::json) FROM pg_tables WHERE schemaname='public'"))
        digests = {}
        for table in tables:
            name = '"' + table.replace('"', '""') + '"'
            rows = sql(f"SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text),'[]'::jsonb)::text FROM public.{name} t")
            digests[table] = hashlib.sha256(rows.encode()).hexdigest()
        assert 'trading_fills' in digests and 'trading_runtime_state' in digests
        return digests

    def run(self, client, base):
        baseline = self.economic_state(self.evidence())
        self.compose('stop', 'postgres')
        time.sleep(2.5)
        try:
            client.open(base + '/api/risk', timeout=20).close()
        except urllib.error.HTTPError as error:
            assert error.code == 503, 'Database failure must be explicit, not empty successful data'
        else:
            raise AssertionError('Risk endpoint must reject unavailable database evidence')
        self.compose('restart', *TRADING)
        self.compose('start', 'postgres')
        self.wait(lambda: self.sql('SELECT 1') == '1', 'PostgreSQL recovery')
        # Explicit recovery restart is part of this test; it does not prove that
        # every worker reconnects automatically without operator intervention.
        self.compose('restart', *TRADING)
        self.wait(lambda: self.economic_state(self.evidence()) == baseline, 'unchanged economic state after database outage')
        self.wait(lambda: len(json.load(client.open(base + '/api/risk', timeout=15))['riskEvaluations']) == 1,
                  'dashboard data recovery')
        source_risk = json.load(client.open(base + '/api/risk', timeout=15))
        self.results['postgres_outage_explicit_503_and_recovery_no_duplicates'] = True

        # Quiesce ALL writers before taking the cross-store maintenance snapshot.
        writers = [name for name in self.document['services'] if name not in ('postgres', 'fixture')]
        self.compose('stop', *writers)
        assert self.evidence()['pending'] == 0
        sql_digests = self.database_digest(self.sql)
        dump = self.backup / 'postgres.dump'
        self.command([*self.dc, 'exec', '-T', 'postgres', 'pg_dump', '-U', 'algotrading',
                      '-d', 'algotrading_paper', '--format=custom', '--no-owner', '--no-privileges'], destination=dump)
        assert dump.read_bytes().startswith(b'PGDMP')
        archives, volume_digests = {}, {}
        for key in self.document['volumes']:
            if key == 'postgres-data':
                continue  # Never restore a running PostgreSQL data-directory copy.
            volume = self.project + '_' + key
            volume_digests[key] = self.volume_digest(volume)
            archive = key + '.tar.gz'
            self.command(['docker', 'run', '--rm', '--network', 'none',
                '--mount', f'type=volume,src={volume},dst=/source,readonly',
                '--mount', f'type=bind,src={self.backup},dst=/backup', 'alpine:3.20',
                'tar', '-czf', '/backup/' + archive, '-C', '/source', '.'])
            (self.backup / archive).chmod(0o600)
            archives[key] = archive
        files = {name:hashlib.sha256((self.backup / name).read_bytes()).hexdigest()
                 for name in ['postgres.dump', *archives.values()]}
        (self.backup / 'manifest.json').write_text(json.dumps({'files':files, 'sql_tables':sql_digests,
            'volume_contents':volume_digests, 'mode':'quiesced maintenance snapshot; credentials stored separately'}, indent=2))
        def verify(checksums):
            for name, checksum in checksums.items():
                if hashlib.sha256((self.backup / name).read_bytes()).hexdigest() != checksum:
                    raise ValueError('Backup checksum mismatch')
        bad = dict(files)
        bad['postgres.dump'] = '0' * 64
        try:
            verify(bad)
        except ValueError:
            self.results['bad_checksum_rejected_before_restore'] = True
        else:
            raise AssertionError('Restore must reject inconsistent checksums')
        verify(json.loads((self.backup / 'manifest.json').read_text())['files'])

        restored = self.project + '-restore'
        env = self.directory / 'restore.env'
        env.write_text((self.directory / '.env').read_text().replace('PAPER_PROJECT=' + self.project + '\n',
                                                                  'PAPER_PROJECT=' + restored + '\n'))
        env.chmod(0o600)
        config = self.directory / 'restore-compose.yml'
        config.write_text(yaml.safe_dump(self.document, sort_keys=False))
        restore_dc = ['docker', 'compose', '--project-name', restored, '--env-file', str(env), '-f', str(config)]
        def compose(*args):
            return self.command([*restore_dc, *args])
        def sql(statement):
            return compose('exec', '-T', 'postgres', 'psql', '-U', 'algotrading', '-d', 'algotrading_paper', '-Atq', '-c', statement)
        # Free only this fixture's networks; original volumes remain untouched.
        self.compose('down')
        try:
            for key, archive in archives.items():
                volume = restored + '_' + key
                self.command(['docker', 'volume', 'create', '--label', 'com.docker.compose.project=' + restored,
                              '--label', 'com.docker.compose.volume=' + key, volume])
                self.command(['docker', 'run', '--rm', '--network', 'none',
                    '--mount', f'type=volume,src={volume},dst=/destination',
                    '--mount', f'type=bind,src={self.backup},dst=/backup,readonly', 'alpine:3.20',
                    'tar', '-xzf', '/backup/' + archive, '-C', '/destination'])
                assert self.volume_digest(volume) == volume_digests[key]
            compose('up', '-d', 'postgres')
            self.wait(lambda: sql('SELECT 1') == '1', 'fresh restore database')
            self.command([*restore_dc, 'exec', '-T', 'postgres', 'pg_restore', '-U', 'algotrading',
                '-d', 'algotrading_paper', '--exit-on-error', '--no-owner', '--no-privileges'], source=dump)
            assert self.database_digest(sql) == sql_digests
            self.results['fresh_volumes_all_sql_tables_and_archived_files_identical'] = True
            services = [name for name in self.document['services'] if name != 'market-data']
            compose('up', '-d', *services)
            # A repeated completed-day ingestion must not repeat orders/fills.
            compose('up', '-d', 'market-data')
            def ingestion_completed():
                state = json.loads(compose('ps', '-a', '--format', 'json', 'market-data'))
                if state['State'] != 'exited':
                    return False
                assert state['ExitCode'] == 0, 'Restored ingestion must complete successfully'
                return True
            self.wait(ingestion_completed, 'successful restored ingestion', timeout=45)
            def recovered():
                fills = json.loads(sql("SELECT coalesce(json_agg(r ORDER BY fill_id),'[]'::json) FROM trading_fills r"))
                account = json.loads(sql('SELECT snapshot::text FROM trading_runtime_state WHERE singleton=TRUE'))
                backend = json.loads(sql('SELECT row_to_json(r) FROM simulated_exchange_state r'))
                positions = json.loads(sql("SELECT coalesce(json_object_agg(coin,quantity),'{}'::json) FROM simulated_exchange_positions"))
                pending = int(sql('SELECT count(*) FROM simulated_exchange_outbox WHERE NOT published'))
                return self.economic_state(dict(fills=fills, account=account, backend=backend, positions=positions, pending=pending)) == baseline
            self.wait(recovered, 'restored trading economic state')
            time.sleep(8)
            assert recovered()
            assert set(TRADING).issubset(compose('ps', '--status', 'running', '--services').splitlines())
            self.results['restored_ingestion_completed_and_trading_workers_running'] = True
            port = json.loads(compose('ps', '--format', 'json', 'dashboard-web'))['Publishers'][0]['PublishedPort']
            restore_client = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
            credentials = dict(line.split('=', 1) for line in env.read_text().splitlines() if '=' in line)
            request = urllib.request.Request(f'http://127.0.0.1:{port}/api/auth/login',
                data=json.dumps({'username':'viewer', 'password':credentials['DASHBOARD_VIEWER_PASSWORD']}).encode(),
                headers={'Content-Type':'application/json'})
            restore_client.open(request, timeout=10).close()
            restored_risk = json.load(restore_client.open(f'http://127.0.0.1:{port}/api/risk', timeout=15))
            assert restored_risk['configurationFingerprint'] == source_risk['configurationFingerprint']
            assert restored_risk['riskEvaluations'] == source_risk['riskEvaluations']
            self.results['restored_authenticated_dashboard_risk_identical'] = True
            self.results['restored_same_day_ingestion_no_duplicate_orders_or_fills'] = True
            (self.directory / 'recovery-accepted.json').write_text(json.dumps({'project':self.project,
                'restored_project':restored, 'checks':self.results, 'economic_state':baseline,
                'backup_files':files, 'limits':'Maintenance-stop local restore, same images/settings; explicit worker restart after database outage; no VPS, live capital, unattended reconnect or off-host retention acceptance.'}, indent=2) + '\n')
            print('PAPER-RECOVERY: PASS: HTTP/NATS/PostgreSQL outages, trading without dashboard, complete maintenance backup and fresh-volume restore')
        finally:
            compose('down')
