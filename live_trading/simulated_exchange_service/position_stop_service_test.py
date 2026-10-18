#!/usr/bin/env python3
"""Isolated local NATS/PostgreSQL acceptance for PAPER stop recovery. No venue access."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / 'build/live_trading/simulated_exchange_service/src/algotrading_simulated_exchange_service'


def docker(*args):
    return subprocess.run(['docker', *args], check=True, capture_output=True, text=True, timeout=30).stdout.strip()


def wait_for(check, description):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        try:
            if check():
                return
        except (OSError, subprocess.SubprocessError):
            pass
        time.sleep(0.1)
    raise AssertionError('Timed out: ' + description)


def stop_process(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    endpoint = json.loads(docker('context', 'inspect'))[0]['Endpoints']['docker']['Host']
    if not os.environ.get('DOCKER_CONTEXT'):
        endpoint = os.environ.get('DOCKER_HOST', endpoint)
    assert endpoint.startswith(('unix://', 'npipe://')), 'Only local Docker is allowed'
    # Require cached images; this acceptance never pulls or contacts a venue.
    docker('image', 'inspect', 'postgres:16-alpine', 'nats:2.10-alpine')
    prefix = 'position-stop-service-' + uuid.uuid4().hex[:10]
    postgres, nats = prefix + '-postgres', prefix + '-nats'
    process = None
    with tempfile.TemporaryDirectory(prefix='position-stop-service-') as folder:
        folder = Path(folder)
        market = folder / 'market.sqlite'
        with sqlite3.connect(market) as db:
            db.executescript('CREATE TABLE market_volume_rank_daily(date INTEGER);'
                             'CREATE TABLE ohlcv_data(pair TEXT,date INTEGER,open REAL,high REAL,low REAL,close REAL,volume REAL);')
            db.executemany('INSERT INTO ohlcv_data VALUES(?,?,?,?,?,?,?)', [
                ('BTC', 20200101, 100, 110, 90, 100, 1000),
                ('BTC', 20200102, 100, 110, 90, 100, 1000),
                ('BTC', 20200103, 80, 90, 70, 80, 1000)])
        try:
            docker('run', '-d', '--name', postgres, '-p', '127.0.0.1::5432',
                   '-e', 'POSTGRES_HOST_AUTH_METHOD=trust', 'postgres:16-alpine')
            docker('run', '-d', '--name', nats, '-p', '127.0.0.1::4222', 'nats:2.10-alpine', '-js')
            pg_port = docker('port', postgres, '5432/tcp').rsplit(':', 1)[1]
            nats_port = int(docker('port', nats, '4222/tcp').rsplit(':', 1)[1])

            def sql(statement):
                return docker('exec', postgres, 'psql', '-U', 'postgres', '-d', 'postgres',
                              '-v', 'ON_ERROR_STOP=1', '-Atc', statement)

            wait_for(lambda: 'accepting connections' in docker('exec', postgres, 'pg_isready', '-h', '127.0.0.1', '-U', 'postgres'),
                     'PostgreSQL TCP startup')
            # Exercise additive migration from the actual pre-stop service schema.
            sql('CREATE TABLE simulated_exchange_state(state_key TEXT PRIMARY KEY,cash DOUBLE PRECISION NOT NULL,'
                'latest_timestamp BIGINT NOT NULL,next_fill_id NUMERIC(20,0) NOT NULL);'
                "INSERT INTO simulated_exchange_state VALUES('simulated-exchange',10000,0,1);"
                'CREATE TABLE simulated_exchange_known_orders(state_key TEXT NOT NULL,order_id BIGINT NOT NULL,'
                'strategy_id BIGINT NOT NULL,created_at BIGINT NOT NULL,active_from BIGINT NOT NULL,coin TEXT NOT NULL,'
                'side SMALLINT NOT NULL,quantity DOUBLE PRECISION NOT NULL,PRIMARY KEY(state_key,order_id));')

            def start():
                log = (folder / 'service.log').open('ab')
                child = subprocess.Popen([str(BINARY), '--nats-url', f'nats://127.0.0.1:{nats_port}',
                    '--postgres', f'host=127.0.0.1 port={pg_port} dbname=postgres user=postgres',
                    '--market-data-db', str(market), '--initial-cash', '10000',
                    '--commission-rate', '0.001', '--poll-timeout-ms', '10'], stdout=log, stderr=log)
                log.close()
                try:
                    wait_for(lambda: child.poll() is not None or b'Simulated exchange ready' in
                             (folder / 'service.log').read_bytes(), 'service startup')
                    assert child.poll() is None, (folder / 'service.log').read_text()[-4000:]
                except BaseException:
                    stop_process(child)
                    raise
                return child

            def publish(subject, document):
                payload = json.dumps(document).encode()
                with socket.create_connection(('127.0.0.1', nats_port), timeout=5) as connection:
                    connection.recv(4096)  # INFO
                    connection.sendall(b'CONNECT {"verbose":false}\r\nPUB ' + subject.encode() + b' ' +
                                       str(len(payload)).encode() + b'\r\n' + payload + b'\r\nPING\r\n')
                    reply = b''
                    while b'PONG' not in reply:
                        reply += connection.recv(4096)
                    assert b'-ERR' not in reply

            def metadata(message_id, day=20200101):
                return {'schema_version': 1, 'message_id': message_id, 'correlation_id': message_id, 'produced_at': day}

            def submit(order_id, stop_price=0, day=20200101):
                order = {'order_id': order_id, 'strategy_id': 1, 'created_at': day, 'active_from': day,
                         'coin': 'BTC', 'side': 1 if stop_price else 0, 'quantity': 12 if stop_price else 10}
                if stop_price:
                    order['position_stop_price'] = stop_price
                publish('gateway.backend.command.submit.v1',
                        {'metadata': metadata('submit-' + uuid.uuid4().hex, day), 'order': order})

            def update(day):
                publish('market.data.updated.v1', {'metadata': metadata('market-' + uuid.uuid4().hex, day),
                    'completed_through': day, 'source': 'fixture', 'timeframe': '1d',
                    'ranked_symbols': 1, 'active_top_n': 1, 'tracked_symbols': 1,
                    'maintained_symbols': 1, 'requested_symbols': 1, 'downloaded_rows': 1})

            def cursor(day):
                wait_for(lambda: sql('SELECT last_position_stop_bar FROM simulated_exchange_state') == str(day),
                         'closed bar checkpoint')

            process = start()
            publish('execution.prices.v1', {'metadata': metadata('open-1'), 'timestamp': 20200101,
                                           'decision_timestamp': 0, 'prices': {'BTC': 100}})
            submit(1)
            wait_for(lambda: sql('SELECT quantity FROM simulated_exchange_positions WHERE coin=\'BTC\'') == '10', 'opening fill')
            submit(2, 70)
            submit(3, 75)
            wait_for(lambda: sql('SELECT count(*) FROM simulated_exchange_active_orders') == '2', 'overlapping stops')
            publish('gateway.backend.command.cancel.v1', {'metadata': metadata('cancel-2'), 'order_id': 2, 'requested_at': 20200101})
            wait_for(lambda: sql('SELECT order_id FROM simulated_exchange_active_orders') == '3', 'old stop cancel')
            update(20200102)
            cursor(20200102)
            assert sql('SELECT position_stop_price FROM simulated_exchange_known_orders WHERE order_id=3') == '75'
            stop_process(process)
            (folder / 'service.log').write_bytes(b'')
            process = start()
            before = sql('SELECT count(*) FROM simulated_exchange_outbox')
            submit(3, 75)
            submit(3, 76)  # Same id with changed stop terms must not replace the durable order.
            time.sleep(0.5)
            assert sql('SELECT count(*) FROM simulated_exchange_outbox') == before
            assert sql('SELECT position_stop_price FROM simulated_exchange_known_orders WHERE order_id=3') == '75'
            publish('execution.prices.v1', {'metadata': metadata('open-4', 20200104), 'timestamp': 20200104,
                                           'decision_timestamp': 20200103, 'prices': {'BTC': 50}})
            time.sleep(0.3)
            assert sql('SELECT count(*) FROM simulated_exchange_execution_prices WHERE timestamp=20200104') == '0'
            update(20200103)
            cursor(20200103)
            wait_for(lambda: sql('SELECT count(*) FROM simulated_exchange_execution_prices WHERE timestamp=20200104') == '1',
                     'open released after the preceding stop bar')
            assert sql('SELECT count(*) FROM simulated_exchange_positions') == '0'
            assert sql('SELECT count(*) FROM simulated_exchange_active_orders') == '0'
            assert abs(float(sql('SELECT cash FROM simulated_exchange_state')) - 9748.25) < 1e-9
            assert sql('SELECT next_fill_id FROM simulated_exchange_state') == '3'
            # A new stop after the close must survive redelivery of that completed bar.
            submit(4, 75, 20200103)
            wait_for(lambda: sql('SELECT order_id FROM simulated_exchange_active_orders') == '4', 'new resting stop')
            stop_process(process)
            (folder / 'service.log').write_bytes(b'')
            process = start()
            update(20200103)
            time.sleep(0.3)
            assert sql('SELECT order_id FROM simulated_exchange_active_orders') == '4'
            publish('market.slice.snapshot.v1', {'metadata': metadata('closed-4', 20200104), 'timestamp': 20200104,
                    'bars': [{'coin': 'BTC', 'open': 80, 'high': 90, 'low': 70, 'close': 80, 'volume': 1000}]})
            cursor(20200104)
            assert sql('SELECT count(*) FROM simulated_exchange_active_orders') == '0'
            assert sql('SELECT next_fill_id FROM simulated_exchange_state') == '3'
            assert abs(float(sql('SELECT cash FROM simulated_exchange_state')) - 9748.25) < 1e-9
            print('POSITION-STOP-SERVICE: PASS: old-schema migration, overlap/cancel, persisted stop terms, '
                  'duplicate/conflict, process restart, capped fill, close-before-open, OHLCV cursor and snapshot bar input')
        finally:
            stop_process(process)
            for name in (postgres, nats):
                subprocess.run(['docker', 'rm', '-f', '-v', name], capture_output=True, timeout=30)


if __name__ == '__main__':
    main()
