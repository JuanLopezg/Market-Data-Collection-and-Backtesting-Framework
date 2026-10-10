#!/usr/bin/env python3
"""Observe completed PAPER cycles and run an independent CURRENT in-process baseline."""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / 'config/strategies/pure_rsi_paper_activity.json'
PORTFOLIO = ROOT / 'config/portfolio/pure_rsi_equal_weight.json'
NOTE = ('Live means PAPER with virtual USD and simulated fills, not private Kraken. '
        'Fast baseline uses CURRENT PureRSI, the same frozen 100-day indicator windows, '
        'daily candidate rankings, thresholds, sizing, fees and observed next-open prices. '
        'PAPER fixes quantity from close(T); fast resolves quantity at open(T+1), so gaps '
        'can cause cash/exposure differences. Equity uses common execution-open marks. '
        'Funding/slippage are not modeled. History contains captured cycles only.')


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def docker(*args):
    return subprocess.check_output(['docker', *args], text=True, stderr=subprocess.DEVNULL, timeout=45)


def query(project):
    # Local socket inside PostgreSQL; no credential-file reads or environment dumps.
    sql = """SELECT json_build_object(
      'snapshot', (SELECT snapshot FROM trading_runtime_state LIMIT 1),
      'backendCash', (SELECT cash FROM simulated_exchange_state LIMIT 1),
      'backendPositions', (SELECT COALESCE(json_object_agg(coin,quantity), '{}'::json) FROM simulated_exchange_positions),
      'prices', (SELECT json_agg(row_to_json(p)) FROM
        (SELECT timestamp, coin, price FROM simulated_exchange_execution_prices ORDER BY timestamp, coin) p),
      'plan', (SELECT plan_payload::json FROM order_planner_live_notional_checkpoint ORDER BY timestamp DESC LIMIT 1),
      'intent', (SELECT intent_payload::json FROM strategy_market_update_checkpoint ORDER BY timestamp DESC LIMIT 1),
      'riskIdentity', (SELECT portfolio_config FROM portfolio_risk_service_metadata LIMIT 1),
      'strategyIdentity', (SELECT strategy_config FROM strategy_service_metadata LIMIT 1),
      'initialCash', (SELECT initial_cash FROM simulated_exchange_metadata LIMIT 1),
      'commissionRate', (SELECT commission_rate FROM simulated_exchange_metadata LIMIT 1)
    );"""
    return json.loads(docker('exec', project + '-postgres-1', 'psql', '-XAt', '-U',
                             'algotrading', '-d', 'algotrading_paper', '-v', 'ON_ERROR_STOP=1', '-c', sql))


def mark_equity(cash, positions, prices):
    return cash + sum(quantity * prices[asset] for asset, quantity in positions.items() if abs(quantity) > 1e-12)


def compare(cycles, baseline):
    rows, positions = [], []
    for cycle, expected in zip(cycles, baseline['rows'], strict=True):
        if expected['decisionDate'] != cycle['decision_date'] or expected['executionDate'] != cycle['execution_date']:
            raise ValueError('Baseline dates do not match observed cycle')
        live = cycle['live']
        prices = cycle['open_prices']
        actual_signals = cycle['signals']
        expected_signals = expected['signals']
        differences = sum(abs(actual_signals.get(asset, 0) - expected_signals.get(asset, 0)) > 1e-9
                          for asset in set(actual_signals) | set(expected_signals))
        rows.append({'date': datetime.strptime(str(cycle['execution_date']), '%Y%m%d').strftime('%Y-%m-%d'),
                     'liveEquity': mark_equity(live['account_cash'], live['account_positions'], prices),
                     'expectedEquity': expected['equity'], 'liveCash': live['account_cash'],
                     'expectedCash': expected['cash'], 'signalDifferences': differences})
        positions = []
        for asset in sorted(set(live['account_positions']) | set(expected['positions'])):
            actual = live['account_positions'].get(asset, 0)
            target = expected['positions'].get(asset, 0)
            if abs(actual) + abs(target) <= 1e-12:
                continue
            positions.append({'asset': asset, 'liveQuantity': actual, 'expectedQuantity': target,
                              'liveUsd': actual * prices[asset], 'expectedUsd': target * prices[asset]})
    return rows, positions


def collect(project, output, binary, image=None):
    observed = query(project)
    decoder = json.JSONDecoder()
    if decoder.raw_decode(observed['strategyIdentity'])[0] != json.loads(PROFILE.read_text()):
        raise ValueError('Observed strategy profile does not match the baseline')
    if decoder.raw_decode(observed['riskIdentity'])[0] != json.loads(PORTFOLIO.read_text()):
        raise ValueError('Observed portfolio profile does not match the baseline')
    live, plan, intent = observed['snapshot'], observed['plan'], observed['intent']
    if not live or not plan or not intent:
        raise ValueError('Waiting for durable strategy/plan/account evidence')
    decision, execution = plan['decision_timestamp'], live['last_execution_timestamp']
    if live['last_bar_close_timestamp'] != decision or intent['timestamp'] != decision or execution <= decision:
        raise ValueError('Cycle is not fully joined/applied yet')
    if any(row['status'] in (0, 1, 2, 3) for row in live['orders']):
        raise ValueError('Waiting for terminal simulated order lifecycle')
    if abs(live['account_cash'] - observed['backendCash']) > 1e-7:
        raise ValueError('Waiting for cash reconciliation')
    backend_positions = observed['backendPositions']
    if any(abs(live['account_positions'].get(asset, 0) - backend_positions.get(asset, 0)) > 1e-9
           for asset in set(live['account_positions']) | set(backend_positions)):
        raise ValueError('Waiting for position reconciliation')
    prices = {row['coin']: row['price'] for row in observed['prices'] or [] if row['timestamp'] == execution}
    if not prices:
        raise ValueError('Execution-open prices unavailable')
    journal = output / 'study.json'
    identity = hashlib.sha256(PROFILE.read_bytes() + PORTFOLIO.read_bytes() +
        json.dumps([observed['initialCash'], observed['commissionRate'], 100]).encode()).hexdigest()
    study = json.loads(journal.read_text()) if journal.exists() else {'fingerprint': identity, 'cycles': []}
    if study['fingerprint'] != identity:
        raise ValueError('Study configuration changed; use a separate output directory')
    cycles = study['cycles']
    if not cycles or decision > cycles[-1]['decision_date']:
        if cycles and decision != cycles[-1]['execution_date']:
            raise ValueError('A forward daily observation is missing; do not invent intervening history')
        mounts = json.loads(docker('inspect', '--format', '{{json .Mounts}}', project + '-market-data-1'))
        source = next(Path(row['Source']) / 'database.db' for row in mounts if row['Destination'] == '/data/market')
        frozen = output / ('market-' + str(decision) + '.sqlite')
        with sqlite3.connect(source.as_uri() + '?mode=ro', uri=True) as reader, sqlite3.connect(frozen) as writer:
            reader.backup(writer)
        # Reject a crossing-cycle capture rather than attach market evidence to the wrong account.
        if query(project)['snapshot'] != live:
            raise ValueError('Account changed during capture; retry')
        signals = next(row['signals'] for row in intent['strategies'] if row['strategy_id'] == 1)
        cycles.append({'decision_date': decision, 'execution_date': execution, 'database': str(frozen),
                       'open_prices': prices, 'live': live, 'signals': signals, 'plan': plan})
        write_json(journal, study)
    elif decision != cycles[-1]['decision_date'] or live != cycles[-1]['live']:
        raise ValueError('Previously captured account cycle changed; retained evidence preserved')
    manifest = {'strategy_config': str(PROFILE), 'portfolio_config': str(PORTFOLIO),
                'initial_cash': observed['initialCash'], 'commission_rate': observed['commissionRate'],
                'warmup_days': 100, 'cycles': cycles, 'output': str(output / 'fast.json')}
    write_json(output / 'manifest.json', manifest)
    env = dict(os.environ, ALGOTRADING_PAPER_BASELINE_MANIFEST=str(output / 'manifest.json'))
    fast_output = output / 'fast.json'
    if not fast_output.exists() or len(json.loads(fast_output.read_text())['rows']) != len(cycles):
        command = [str(binary)]
        if image:
            command = ['docker', 'run', '--rm', '--network', 'none', '--read-only', '--cap-drop', 'ALL',
                       '--security-opt', 'no-new-privileges', '--user', str(os.getuid()),
                       '--memory', '384m', '--cpus', '0.4', '--pids-limit', '64',
                       '-v', str(output) + ':' + str(output),
                       '-v', str(ROOT / 'config') + ':' + str(ROOT / 'config') + ':ro',
                       '-e', 'ALGOTRADING_PAPER_BASELINE_MANIFEST=' + str(output / 'manifest.json'),
                       '--entrypoint', 'algotrading_research', image]
        subprocess.run(command, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=120)
    rows, positions = compare(cycles, json.loads((output / 'fast.json').read_text()))
    shadow = ROOT / 'storage/kraken_shadow' / (project + '-activity-forward')
    marker = output / ('kraken-' + str(decision) + '.json')
    shadow_note = ' Kraken public plan observation: retained.'
    if not marker.exists():
        exported = output / 'producer-input.json'
        write_json(exported, [{'plan': plan}])
        try:
            subprocess.run(['python3', str(ROOT / 'live_trading/kraken_shadow.py'), '--plans', str(exported),
                            '--configuration-fingerprint', hashlib.sha256(observed['riskIdentity'].encode()).hexdigest(),
                            '--output', str(shadow)], check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
            public_report = json.loads((shadow / 'report.json').read_text())
            if any(row['state'] == 'UNAVAILABLE' for row in public_report['plans']):
                raise ValueError('Public plan observation is not complete')
            write_json(marker, {'decisionDate': decision, 'report': str(shadow / 'report.json')})
        except (subprocess.SubprocessError, OSError, ValueError):
            shadow_note = ' Kraken public plan observation: unavailable; retrying, no private orders.'
    write_json(output / 'comparison.json', {'contractVersion': 'paper-comparison-v1', 'status': 'OBSERVING',
        'checkedAt': datetime.now(timezone.utc).isoformat(), 'profile': 'PureRSI(7): entry > 50, exit < 40 · virtual USD',
        'fingerprint': identity, 'note': NOTE + shadow_note, 'rows': rows, 'positions': positions})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', default='algotrading-paper')
    parser.add_argument('--output', type=Path, default=ROOT / 'storage/paper_trading/comparison')
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/research/src/legacy/algotrading_research')
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--image', help='Run the packaged fast binary without host build dependencies')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'collector.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        running = True
        def stop(*_):
            nonlocal running
            running = False
        signal.signal(signal.SIGTERM, stop)
        while running:
            try:
                collect(args.project, output, args.binary.resolve(), args.image)
                print('PAPER comparison refreshed', flush=True)
            except (ValueError, KeyError, sqlite3.Error, subprocess.SubprocessError, OSError):
                # No raw subprocess/authentication output in persistent diagnostics.
                print('PAPER comparison unavailable; retained evidence preserved', flush=True)
                if args.once:
                    raise SystemExit(1)
            if args.once:
                break
            for _ in range(60):
                if not running: break
                time.sleep(1)


if __name__ == '__main__':
    main()
