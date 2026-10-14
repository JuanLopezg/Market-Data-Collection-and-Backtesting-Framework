#!/usr/bin/env python3
"""Observe completed PAPER or Kraken LIVE cycles and run an independent CURRENT in-process baseline.

Runs from the repository (deploy/chain/comparison.py, local chain observations)
or from an installed chain (<root>/chain/comparison.py, short container names).
The strategy and portfolio files come from STRATEGY_FILE / PORTFOLIO_FILE (chain.env).

When that configuration changes, the study so far is moved to archive-<fingerprint>/
and a new one starts from the account and strategy state at that moment (the fast
baseline is seeded with it instead of the initial cash). The comparison rows already
shown stay in comparison.json, each compared under the configuration of its day.

MODE=live (chain.env): the account trades on Kraken. The study starts from the first
observed cycle; the baseline gets the day's BTC collateral and USD capital flows so it
sizes like the account, and each row splits the difference into real fees, funding,
interest, fill prices against the Binance open and markets Kraken skipped.
"""
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
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent
IN_REPOSITORY = HERE.parent.name == 'deploy'
# Repository root, or the installation root ("/" on the VPS).
ROOT = HERE.parents[1] if IN_REPOSITORY else HERE.parent
CONFIG = ROOT / 'config'
PROFILE = CONFIG / 'strategies' / os.environ.get('STRATEGY_FILE', 'pure_rsi_paper_activity.json')
PORTFOLIO = CONFIG / 'portfolio' / os.environ.get('PORTFOLIO_FILE', 'pure_rsi_equal_weight.json')
STUDY_FILES = ('study.json', 'manifest.json', 'fast.json', 'fast-input.sha256', 'comparison.json', 'producer-input.json')
# Public Kraken plan observation tool (install copies it to chain/tools).
SHADOW_TOOL = (ROOT / 'live_trading' if IN_REPOSITORY else HERE / 'tools') / 'kraken_shadow.py'
NOTE =('Live means PAPER with virtual USD and simulated fills, not private Kraken. '
        'Fast baseline uses CURRENT PureRSI, the same frozen 100-day indicator windows, '
        'daily candidate rankings, thresholds, sizing, fees and observed next-open prices. '
        'PAPER fixes quantity from close(T); fast resolves quantity at open(T+1), so gaps '
        'can cause cash/exposure differences. Equity uses common execution-open marks. '
        'Funding/slippage are not modeled. History contains captured cycles only.')
MODE = os.environ.get('MODE', 'paper')
LIVE_NOTE = ('Live means the Kraken account (real fills, fees, funding and interest). Expected is '
             'CURRENT PureRSI on the same frozen market data and decisions, filled at the Binance '
             'open of the execution day with LIVE_COMMISSION_RATE, sized with the same BTC collateral and '
             'given the same USD deposits/withdrawals. Each row splits the difference into real fees '
             'versus expected, funding, interest, fill prices against the Binance open (slippage, '
             'Kraken versus Binance) and markets Kraken skipped; the rest is quantity rounding and '
             'close-versus-open sizing. Manual Control trades are not in the expected run. The study '
             'starts at its first observed cycle; the latest row updates until the next cycle.')
BINANCE_KLINES = 'https://fapi.binance.com/fapi/v1/klines'
# Orders the gateway skips on purpose (asset or size not tradable at Kraken).
SKIP_CODES = ('MARKET_UNAVAILABLE', 'MARKET_NOT_TRADING', 'BELOW_MINIMUM_QUANTITY')
ORDER_REJECTED = 6


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def docker(*args):
    return subprocess.check_output(['docker', *args], text=True, stderr=subprocess.DEVNULL, timeout=45)


def query(postgres_container):
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
    return json.loads(docker('exec', postgres_container, 'psql', '-XAt', '-U',
                             'algotrading', '-d', 'algotrading_paper', '-v', 'ON_ERROR_STOP=1', '-c', sql))


def normalized(live):
    """The account snapshot with its unordered lists sorted.

    After a restart the execution service writes orders and processed fill ids in another
    order; the content is the same, so it must not look like a changed account.
    """
    if not live:
        return live
    result = dict(live)
    if 'orders' in live:
        result['orders'] = sorted(live['orders'], key=lambda order: order['order_id'])
    if 'processed_fill_ids' in live:
        result['processed_fill_ids'] = sorted(live['processed_fill_ids'])
    return result


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


def profile_label(live=False):
    """E.g. 'PureRSI(7): entry > 50, exit < 40 · vol target 20% · virtual USD'."""
    strategy = json.loads(PROFILE.read_text())['strategies'][0]
    sizer = json.loads(PORTFOLIO.read_text())['strategies'][0]['sizer']
    parameters = strategy['parameters']
    sizing = (f"vol target {sizer['target_volatility']:.0%}" if sizer['type'] == 'volatility_target'
              else f"{sizer['weight_per_full_signal']:.0%} per signal")
    account = 'Kraken USD + BTC collateral' if live else 'virtual USD'
    return (f"PureRSI({parameters['rsi_length']}): entry > {parameters['rsi_entry']:g}, "
            f"exit < {parameters['rsi_exit']:g} · {sizing} · {account}")


def validate_seed_intent(seed, intent):
    """Signals belong to the strategy checkpoint of the seed's completed close."""
    if intent['timestamp'] != seed['decision_date']:
        raise ValueError('Seed strategy checkpoint date does not match the account')
    states = seed['live']['strategies']
    ids = [state['strategy_id'] for state in states]
    intent_ids = [state['strategy_id'] for state in intent['strategies']]
    if len(set(ids)) != len(ids) or len(set(intent_ids)) != len(intent_ids) or set(ids) != set(intent_ids):
        raise ValueError('Seed strategy checkpoint identities do not match the account')
    if any(not isinstance(state.get('signals'), dict) for state in intent['strategies']):
        raise ValueError('Seed strategy checkpoint signals are unavailable')


def restore_seed_intent(output, study, postgres_container, current_intent):
    seed = study.get('seed')
    if seed is None:
        return
    if 'intent' not in seed:
        if current_intent['timestamp'] == seed['decision_date']:
            intent = current_intent
        else:
            date = int(seed['decision_date'])
            sql = f'SELECT intent_payload FROM strategy_market_update_checkpoint WHERE timestamp = {date};'
            payload = docker('exec', postgres_container, 'psql', '-XAt', '-U', 'algotrading',
                             '-d', 'algotrading_paper', '-v', 'ON_ERROR_STOP=1', '-c', sql)
            if not payload.strip():
                raise ValueError('Historical strategy checkpoint for the seed is unavailable')
            intent = json.loads(payload)
        validate_seed_intent(seed, intent)
        # Retain the incorrect study/results once, before repairing an old journal.
        archive = output / 'before-seed-repair'
        archive.mkdir(exist_ok=True)
        for name in STUDY_FILES:
            source, retained = output / name, archive / name
            if source.exists() and not retained.exists():
                retained.write_bytes(source.read_bytes())
        seed['intent'] = intent
        write_json(output / 'study.json', study)
    validate_seed_intent(seed, seed['intent'])


def baseline_start(seed):
    validate_seed_intent(seed, seed['intent'])
    signals = {state['strategy_id']: state['signals'] for state in seed['intent']['strategies']}
    return {'cash': seed['live']['account_cash'], 'positions': seed['live']['account_positions'],
            'strategies': [dict(state, signals=signals[state['strategy_id']])
                           for state in seed['live']['strategies']]}


def restart_study(output, study, identity, live, intent):
    """Archive the study of the previous configuration; seed a new one with the current state."""
    cycles = study['cycles']
    if not cycles or normalized(cycles[-1]['live']) != live:
        raise ValueError('Configuration changed after an uncaptured cycle; the new study needs the last captured state')
    last = cycles[-1]
    seed = {'decision_date': last['decision_date'], 'execution_date': last['execution_date'],
            'live': live, 'intent': intent}
    validate_seed_intent(seed, intent)
    comparison = output / 'comparison.json'
    previous_rows = json.loads(comparison.read_text())['rows'] if comparison.exists() else []
    archive = output / ('archive-' + study['fingerprint'][:12])
    archive.mkdir(exist_ok=False)
    for path in [output / name for name in STUDY_FILES] + sorted(output.glob('market-*.sqlite*')) + sorted(output.glob('kraken-*.json')):
        if path.exists():
            path.replace(archive / path.name)
    restarted = {'fingerprint': identity, 'previousRows': previous_rows, 'cycles': [],
                 'seed': seed}
    write_json(output / 'study.json', restarted)
    print('Configuration changed: previous study archived in ' + archive.name + '; new study seeded', flush=True)
    return restarted


def check_profiles(observed):
    decoder = json.JSONDecoder()
    if decoder.raw_decode(observed['strategyIdentity'])[0] != json.loads(PROFILE.read_text()):
        raise ValueError('Observed strategy profile does not match the baseline')
    if decoder.raw_decode(observed['riskIdentity'])[0] != json.loads(PORTFOLIO.read_text()):
        raise ValueError('Observed portfolio profile does not match the baseline')


def freeze_market(market_container, output, decision):
    """Copy the market database as it is at this cycle; the baseline reads only this copy."""
    mounts = json.loads(docker('inspect', '--format', '{{json .Mounts}}', market_container))
    source = next(Path(row['Source']) / 'database.db' for row in mounts if row['Destination'] == '/data/market')
    frozen = output / ('market-' + str(decision) + '.sqlite')
    with sqlite3.connect(source.as_uri() + '?mode=ro', uri=True) as reader, sqlite3.connect(frozen) as writer:
        reader.backup(writer)
    return frozen


def run_baseline(output, manifest, binary, image, cycle_count):
    """Reuse results only while their economic inputs and completed-cycle count match."""
    cycle_fields = ('database', 'decision_date', 'execution_date', 'open_prices',
                    'collateral_usd', 'capital_flow_usd')
    inputs = dict(manifest, cycles=[{key: cycle[key] for key in cycle_fields if key in cycle}
                                   for cycle in manifest['cycles']])
    fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode() +
        Path(manifest['strategy_config']).read_bytes() + Path(manifest['portfolio_config']).read_bytes()).hexdigest()
    cache_identity = output / 'fast-input.sha256'
    write_json(output / 'manifest.json', manifest)
    env = dict(os.environ, ALGOTRADING_PAPER_BASELINE_MANIFEST=str(output / 'manifest.json'))
    fast_output = output / 'fast.json'
    if (not fast_output.exists() or not cache_identity.exists() or
            cache_identity.read_text().strip() != fingerprint or
            len(json.loads(fast_output.read_text())['rows']) != cycle_count):
        command = [str(binary)]
        if image:
            command = ['docker', 'run', '--rm', '--network', 'none', '--read-only', '--cap-drop', 'ALL',
                       '--security-opt', 'no-new-privileges', '--user', str(os.getuid()),
                       '--memory', '384m', '--cpus', '0.4', '--pids-limit', '64',
                       '-v', str(output) + ':' + str(output),
                       '-v', str(CONFIG) + ':' + str(CONFIG) + ':ro',
                       '-e', 'ALGOTRADING_PAPER_BASELINE_MANIFEST=' + str(output / 'manifest.json'),
                       '--entrypoint', 'algotrading_research', image]
        subprocess.run(command, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=120)
        if len(json.loads(fast_output.read_text())['rows']) != cycle_count:
            raise ValueError('Baseline output cycle count does not match the study')
        temporary = cache_identity.with_suffix('.tmp')
        temporary.write_text(fingerprint + '\n')
        temporary.replace(cache_identity)
    return json.loads(fast_output.read_text())


def collect(containers, output, shadow, binary, image=None):
    postgres_container, market_container = containers
    observed = query(postgres_container)
    check_profiles(observed)
    live, plan, intent = normalized(observed['snapshot']), observed['plan'], observed['intent']
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
        study = restart_study(output, study, identity, live, intent)
    restore_seed_intent(output, study, postgres_container, intent)
    cycles = study['cycles']
    seed = study.get('seed')
    last = cycles[-1] if cycles else seed
    if last is None or decision > last['decision_date']:
        if last and decision != last['execution_date']:
            raise ValueError('A forward daily observation is missing; do not invent intervening history')
        frozen = freeze_market(market_container, output, decision)
        # Reject a crossing-cycle capture rather than attach market evidence to the wrong account.
        if normalized(query(postgres_container)['snapshot']) != live:
            raise ValueError('Account changed during capture; retry')
        signals = next(row['signals'] for row in intent['strategies'] if row['strategy_id'] == 1)
        cycles.append({'decision_date': decision, 'execution_date': execution, 'database': str(frozen),
                       'open_prices': prices, 'live': live, 'signals': signals, 'plan': plan})
        write_json(journal, study)
    elif decision != last['decision_date'] or live != normalized(last['live']):
        raise ValueError('Previously captured account cycle changed; retained evidence preserved')
    previous_rows = study.get('previousRows', [])
    if not cycles:
        # Seeded study before its first cycle: keep showing the earlier rows.
        write_json(output / 'comparison.json', {'contractVersion': 'paper-comparison-v1', 'status': 'OBSERVING',
            'checkedAt': datetime.now(timezone.utc).isoformat(), 'profile': profile_label(), 'fingerprint': identity,
            'note': NOTE + ' Configuration changed; waiting for its first daily cycle.',
            'rows': previous_rows, 'positions': []})
        return
    manifest = {'strategy_config': str(PROFILE), 'portfolio_config': str(PORTFOLIO),
                'initial_cash': observed['initialCash'], 'commission_rate': observed['commissionRate'],
                'warmup_days': 100, 'cycles': cycles, 'output': str(output / 'fast.json')}
    if seed:
        manifest['start'] = baseline_start(seed)
    rows, positions = compare(cycles, run_baseline(output, manifest, binary, image, len(cycles)))
    marker = output / ('kraken-' + str(decision) + '.json')
    shadow_note = ' Kraken public plan observation: retained.'
    if not marker.exists():
        exported = output / 'producer-input.json'
        write_json(exported, [{'plan': plan}])
        try:
            subprocess.run(['python3', str(SHADOW_TOOL), '--plans', str(exported),
                            '--configuration-fingerprint', hashlib.sha256(observed['riskIdentity'].encode()).hexdigest(),
                            '--study-root', str(shadow.parent), '--output', str(shadow)],
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
            public_report = json.loads((shadow / 'report.json').read_text())
            if any(row['state'] == 'UNAVAILABLE' for row in public_report['plans']):
                raise ValueError('Public plan observation is not complete')
            write_json(marker, {'decisionDate': decision, 'report': str(shadow / 'report.json')})
        except (subprocess.SubprocessError, OSError, ValueError):
            shadow_note = ' Kraken public plan observation: unavailable; retrying, no private orders.'
    write_json(output / 'comparison.json', {'contractVersion': 'paper-comparison-v1', 'status': 'OBSERVING',
        'checkedAt': datetime.now(timezone.utc).isoformat(), 'profile': profile_label(),
        'fingerprint': identity, 'note': NOTE + shadow_note, 'rows': previous_rows + rows, 'positions': positions})


# ------------------------------------------------------------------ Kraken LIVE

def query_live(postgres_container):
    """The account, its real fills and venue costs, the day's collateral (no simulated exchange)."""
    sql = """SELECT json_build_object(
      'snapshot', (SELECT snapshot FROM trading_runtime_state LIMIT 1),
      'plan', (SELECT plan_payload::json FROM order_planner_live_notional_checkpoint ORDER BY timestamp DESC LIMIT 1),
      'intent', (SELECT intent_payload::json FROM strategy_market_update_checkpoint ORDER BY timestamp DESC LIMIT 1),
      'riskIdentity', (SELECT portfolio_config FROM portfolio_risk_service_metadata LIMIT 1),
      'strategyIdentity', (SELECT strategy_config FROM strategy_service_metadata LIMIT 1),
      'fills', (SELECT COALESCE(json_agg(row_to_json(f) ORDER BY f.fill_id), '[]'::json) FROM
        (SELECT fill_id, order_id, timestamp, coin, side, quantity, price, commission FROM trading_fills) f),
      'costs', (SELECT COALESCE(json_agg(row_to_json(c) ORDER BY c.timestamp, c.cost_id), '[]'::json) FROM
        (SELECT cost_id, kind, timestamp, amount FROM trading_venue_costs) c),
      'collateral', (SELECT COALESCE(json_object_agg(date, collateral_usd), '{}'::json) FROM trading_daily_collateral)
    );"""
    return json.loads(docker('exec', postgres_container, 'psql', '-XAt', '-U',
                             'algotrading', '-d', 'algotrading_paper', '-v', 'ON_ERROR_STOP=1', '-c', sql))


def binance_opens(execution_date, symbols):
    """Binance USDT-perpetual open of the execution day: the price the expected run fills at."""
    start = int(datetime.strptime(str(execution_date), '%Y%m%d').replace(tzinfo=timezone.utc).timestamp() * 1000)
    prices = {}
    for symbol in sorted(symbols):
        query_string = urllib.parse.urlencode({'symbol': symbol, 'interval': '1d', 'startTime': start, 'limit': 1})
        with urllib.request.urlopen(BINANCE_KLINES + '?' + query_string, timeout=15) as response:
            rows = json.loads(response.read(65536))
        if not rows or rows[0][0] != start:
            raise ValueError('Binance open of ' + str(execution_date) + ' unavailable for ' + symbol)
        prices[symbol] = float(rows[0][1])
    return prices


def priced_assets(live, plan, intent, previous_cycle, fast_output):
    """Every asset the account or the expected run may hold or trade at this open."""
    assets = {asset for asset, quantity in live['account_positions'].items() if abs(quantity) > 1e-12}
    assets |= {order['coin'] for order in plan.get('submit_orders', [])}
    for strategy in intent['strategies']:
        assets |= {asset for asset, signal in strategy['signals'].items() if signal != 0}
    if previous_cycle:
        assets |= set(previous_cycle.get('open_prices', {}))
    if fast_output.exists():
        rows = json.loads(fast_output.read_text())['rows']
        if rows:
            assets |= {asset for asset, quantity in rows[-1]['positions'].items() if abs(quantity) > 1e-12}
    return assets


def compare_live(cycles, baseline, seed, fills, costs):
    """PAPER-style rows plus where the difference comes from, accumulated since the seed."""
    rows, positions = compare(cycles, baseline)
    start = seed['execution_date']
    for row, cycle, expected in zip(rows, cycles, baseline['rows'], strict=True):
        day = cycle['execution_date']
        totals = {'FEE': 0.0, 'FUNDING': 0.0, 'INTEREST': 0.0, 'CAPITAL_FLOW': 0.0}
        for cost in costs:
            if start < cost['timestamp'] <= day:
                totals[cost['kind']] = totals.get(cost['kind'], 0.0) + cost['amount']
        fee_from_fills = sum(fill['commission'] for fill in fills if start < fill['timestamp'] <= day)
        # Fill price against the Binance open, for the orders of each cycle's plan:
        # positive = paid more on a buy or received less on a sell.
        slippage = 0.0
        for previous in cycles:
            if previous['execution_date'] > day:
                break
            planned = {order['order_id'] for order in previous['plan'].get('submit_orders', [])}
            for fill in fills:
                if fill['order_id'] in planned and fill['coin'] in previous['open_prices']:
                    direction = 1 if fill['side'] == 0 else -1
                    slippage += direction * (fill['price'] - previous['open_prices'][fill['coin']]) * fill['quantity']
        row.update({'liveFees': fee_from_fills - totals['FEE'], 'expectedFees': expected['fees'],
                    'liveFunding': totals['FUNDING'], 'liveInterest': totals['INTEREST'],
                    'capitalFlows': totals['CAPITAL_FLOW'], 'slippage': slippage,
                    'skipped': cycle.get('skipped', [])})
    return rows, positions


def collect_live(containers, output, binary, image=None):
    postgres_container, market_container = containers
    observed = query_live(postgres_container)
    check_profiles(observed)
    live, plan, intent = normalized(observed['snapshot']), observed['plan'], observed['intent']
    if not live or not plan or not intent:
        raise ValueError('Waiting for durable strategy/plan/account evidence')
    decision, execution = plan['decision_timestamp'], live['last_execution_timestamp']
    if live['last_bar_close_timestamp'] != decision or intent['timestamp'] != decision or execution <= decision:
        raise ValueError('Cycle is not fully joined/applied yet')
    if any(row['status'] in (0, 1, 2, 3) for row in live['orders']):
        raise ValueError('Waiting for terminal Kraken order lifecycle')
    # Expected fee rate at Kraken (chain.env; COMMISSION_RATE is the PAPER simulation's).
    commission_rate = float(os.environ.get('LIVE_COMMISSION_RATE', '0.0005'))
    journal = output / 'study.json'
    identity = hashlib.sha256(PROFILE.read_bytes() + PORTFOLIO.read_bytes() +
        json.dumps(['kraken-live', commission_rate, 100]).encode()).hexdigest()
    study = json.loads(journal.read_text()) if journal.exists() else {'fingerprint': identity, 'cycles': []}
    if study['fingerprint'] != identity:
        # Before its first row a study has nothing to archive: start again.
        study = (restart_study(output, study, identity, live, intent) if study['cycles']
                 else {'fingerprint': identity, 'cycles': [], 'previousRows': study.get('previousRows', [])})
    cycles, seed = study['cycles'], study.get('seed')
    if seed is None:
        # The first observed cycle is the starting point: the expected run begins from this
        # account, so the comparison starts with the next cycle.
        seed = study['seed'] = {'decision_date': decision, 'execution_date': execution, 'live': live, 'intent': intent}
    restore_seed_intent(output, study, postgres_container, intent)
    last = cycles[-1] if cycles else seed
    if decision > last['decision_date']:
        if decision != last['execution_date']:
            raise ValueError('A forward daily observation is missing; do not invent intervening history')
        frozen = freeze_market(market_container, output, decision)
        if normalized(query_live(postgres_container)['snapshot']) != live:
            raise ValueError('Account changed during capture; retry')
        prices = binance_opens(execution, priced_assets(live, plan, intent, cycles[-1] if cycles else None,
                                                        output / 'fast.json'))
        flows = sum(cost['amount'] for cost in observed['costs']
                    if cost['kind'] == 'CAPITAL_FLOW' and last['execution_date'] < cost['timestamp'] <= execution)
        signals = next(row['signals'] for row in intent['strategies'] if row['strategy_id'] == 1)
        skipped = sorted({order['coin'] for order in live['orders']
                          if order['status'] == ORDER_REJECTED and order['active_from'] == execution
                          and order.get('last_message', '').startswith(SKIP_CODES)})
        cycles.append({'decision_date': decision, 'execution_date': execution, 'database': str(frozen),
                       'open_prices': prices, 'live': live, 'signals': signals, 'plan': plan,
                       'collateral_usd': float(observed['collateral'].get(str(decision), 0.0)),
                       'capital_flow_usd': flows, 'skipped': skipped})
    elif decision == last['decision_date']:
        # Same cycle: venue fees arrive minutes after the fills and funding moves cash every
        # hour, so the latest row (or the starting point) follows the account until the next cycle.
        last['live'] = live
    else:
        raise ValueError('Previously captured account cycle changed; retained evidence preserved')
    write_json(journal, study)
    previous_rows = study.get('previousRows', [])
    if not cycles:
        write_json(output / 'comparison.json', {'contractVersion': 'paper-comparison-v1', 'status': 'OBSERVING',
            'checkedAt': datetime.now(timezone.utc).isoformat(), 'profile': profile_label(live=True),
            'fingerprint': identity, 'note': LIVE_NOTE + ' Started; waiting for the next daily cycle.',
            'rows': previous_rows, 'positions': []})
        return
    manifest = {'strategy_config': str(PROFILE), 'portfolio_config': str(PORTFOLIO),
                'initial_cash': seed['live']['account_cash'], 'commission_rate': commission_rate,
                'warmup_days': 100, 'cycles': cycles, 'output': str(output / 'fast.json'),
                'start': baseline_start(seed)}
    rows, positions = compare_live(cycles, run_baseline(output, manifest, binary, image, len(cycles)),
                                   seed, observed['fills'], observed['costs'])
    write_json(output / 'comparison.json', {'contractVersion': 'paper-comparison-v1', 'status': 'OBSERVING',
        'checkedAt': datetime.now(timezone.utc).isoformat(), 'profile': profile_label(live=True),
        'fingerprint': identity, 'note': LIVE_NOTE, 'rows': previous_rows + rows, 'positions': positions})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    if IN_REPOSITORY:
        state = ROOT / 'storage/chain'
        if MODE == 'live':
            state = state / 'live'
        defaults = {'postgres': 'postgres', 'market': 'market-data',
                    'output': state / 'comparison',
                    'shadow': ROOT / 'storage/kraken_shadow/chain-forward'}
    else:
        # MODE=live keeps its state in data/live/ (dash-api mounts <state>/comparison).
        state = ROOT / 'data/live' if MODE == 'live' else ROOT / 'data'
        defaults = {'postgres': 'postgres', 'market': 'market-data', 'output': state / 'comparison',
                    'shadow': ROOT / 'data/kraken_shadow/paper-forward'}
    parser.add_argument('--postgres-container', default=defaults['postgres'])
    parser.add_argument('--market-container', default=defaults['market'])
    parser.add_argument('--output', type=Path, default=defaults['output'])
    parser.add_argument('--shadow-output', type=Path, default=defaults['shadow'],
                        help='Kraken public plan observation study (journal reused across restarts)')
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/research/src/backtests/algotrading_research')
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--image', help='Run the packaged fast binary without host build dependencies')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    containers = (args.postgres_container, args.market_container)
    with (output / 'collector.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        running = True
        def stop(*_):
            nonlocal running
            running = False
        signal.signal(signal.SIGTERM, stop)
        while running:
            label = 'Kraken LIVE' if MODE == 'live' else 'PAPER'
            try:
                if MODE == 'live':
                    collect_live(containers, output, args.binary.resolve(), args.image)
                else:
                    collect(containers, output, args.shadow_output.resolve(), args.binary.resolve(), args.image)
                print(label + ' comparison refreshed', flush=True)
            except (ValueError, KeyError, sqlite3.Error, subprocess.SubprocessError, OSError):
                # No raw subprocess/authentication output in persistent diagnostics.
                print(label + ' comparison unavailable; retained evidence preserved', flush=True)
                if args.once:
                    raise SystemExit(1)
            if args.once:
                break
            for _ in range(60):
                if not running: break
                time.sleep(1)


if __name__ == '__main__':
    main()
