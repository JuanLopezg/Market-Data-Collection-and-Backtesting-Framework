#!/usr/bin/env python3
"""Local standby of the deployed chain: the orders the VPS would send, computed here.

If the VPS is down, `plan` tells what the chain would do at the next open: it downloads the
last --days daily klines of the Binance USDT perpetuals the market-data service ranks,
writes them in the market-data service's SQLite schema, runs the deployed strategy and
portfolio profile over them (build/standby/algotrading_standby_plan, the same profile code
as the comparison baseline) and derives the orders from the account's real holdings.
Phase 1: it never sends anything. Run under WSL from the repository root.

Commands:
  sync  Read the deployed profile from the VPS (read-only SSH): MODE, STRATEGY_FILE,
        PORTFOLIO_FILE and LIVE_COMMISSION_RATE from chain.env, those two config files, the
        Binance -> Kraken symbol map and the current release id, into standby/profile/.
        Run it after every release activation or config change on the VPS.
  plan  Compute the orders of the next open. --account kraken (default) reads the Kraken
        account with the read-only key (--credentials); --account-file takes a JSON
        {"cash", "collateral_usd", "positions": {source symbol: quantity}} instead.

Examples:
  python3 standby/standby.py sync
  python3 standby/standby.py plan
  python3 standby/standby.py plan --account-file account.json --date 2026-10-14
"""

import argparse
import datetime as dt
import hashlib
import json
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STANDBY = ROOT / 'standby'
PROFILE = STANDBY / 'profile'
RUNS = STANDBY / 'run'
PLANNER = ROOT / 'build/standby/algotrading_standby_plan'

sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'live_trading'))
import binance_perp_daily as binance  # noqa: E402  (public klines/exchangeInfo helpers)

# Same windows as the chain: market-data ranks the top 100 and the strategy sees the top 50
# with 100 days of history (config/market_data, strategy --market-top-n, comparison.py).
RANKING_SIZE = 100
TOP_N = 50
WARMUP_DAYS = 100
VPS_ROOT = '/algoTrading'
CHAIN_ENV_KEYS = ('MODE', 'STRATEGY_FILE', 'PORTFOLIO_FILE', 'LIVE_COMMISSION_RATE')


def day_key(day):
    return int(day.strftime('%Y%m%d'))


def ssh(args, command):
    result = subprocess.run(['ssh', '-i', str(Path(args.ssh_key).expanduser()), '-o', 'BatchMode=yes',
                             '-o', 'ConnectTimeout=20', args.host, command],
                            capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        raise SystemExit(f'ERROR: VPS command failed: {result.stderr.strip()[:300]}')
    return result.stdout


# ---------------------------------------------------------------- sync
def sync(args):
    """Copy only non-secret files; chain.env is filtered to four keys on the VPS side."""
    keys = '|'.join(CHAIN_ENV_KEYS)
    settings = {}
    for line in ssh(args, f"grep -E '^({keys})=' {VPS_ROOT}/chain/chain.env").splitlines():
        name, _, value = line.partition('=')
        settings[name] = value.strip().strip('"\'')
    for name in ('STRATEGY_FILE', 'PORTFOLIO_FILE'):
        if not settings.get(name) or '/' in settings[name]:
            raise SystemExit(f'ERROR: {name} missing or invalid in the VPS chain.env')
    release = ssh(args, f'cat {VPS_ROOT}/releases/current').strip()
    files = {
        'strategy.json': f"{VPS_ROOT}/config/strategies/{settings['STRATEGY_FILE']}",
        'portfolio.json': f"{VPS_ROOT}/config/portfolio/{settings['PORTFOLIO_FILE']}",
        'symbol_map.json': f'{VPS_ROOT}/chain/config/venues/kraken_futures/symbol_map.json',
    }
    PROFILE.mkdir(parents=True, exist_ok=True)
    digests = {}
    for local, remote in files.items():
        text = ssh(args, f'cat {remote}')
        json.loads(text)  # refuse anything that is not the expected JSON
        (PROFILE / local).write_text(text, encoding='utf-8')
        digests[local] = hashlib.sha256(text.encode()).hexdigest()
    profile = {'release': release, 'synced_at': dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds'),
               'mode': settings.get('MODE', 'paper'), 'strategy_file': settings['STRATEGY_FILE'],
               'portfolio_file': settings['PORTFOLIO_FILE'],
               'live_commission_rate': float(settings.get('LIVE_COMMISSION_RATE') or 0.0005),
               'sha256': digests}
    (PROFILE / 'profile.json').write_text(json.dumps(profile, indent=2) + '\n', encoding='utf-8')
    print(f"Synced release {release}: MODE={profile['mode']}, {profile['strategy_file']}, "
          f"{profile['portfolio_file']} -> {PROFILE}")


def load_profile():
    path = PROFILE / 'profile.json'
    if not path.exists():
        raise SystemExit('ERROR: no deployed profile yet; run: python3 standby/standby.py sync')
    profile = json.loads(path.read_text(encoding='utf-8'))
    mapping = json.loads((PROFILE / 'symbol_map.json').read_text(encoding='utf-8'))
    return profile, {row['source']: row for row in mapping['symbols']}


# ---------------------------------------------------------------- market data
def build_market_database(path, first_day, last_day):
    """Binance daily klines in the market-data service schema: ohlcv_data + daily ranking.

    The VPS ranks by the 24 h ticker right after 00:00 UTC, which is the completed day's
    quote volume; here each day is ranked by its kline quote volume, over the same
    eligible set (USDT PERPETUAL contracts with status TRADING).
    """
    info = binance.get_json(binance.EXCHANGE_INFO_URL)
    symbols = sorted(s['symbol'] for s in info['symbols']
                     if s.get('contractType') == 'PERPETUAL' and s.get('status') == 'TRADING'
                     and s.get('quoteAsset') == 'USDT')
    start_ms = int(dt.datetime(first_day.year, first_day.month, first_day.day, tzinfo=dt.timezone.utc).timestamp() * 1000)
    end_ms = int(dt.datetime(last_day.year, last_day.month, last_day.day, tzinfo=dt.timezone.utc).timestamp() * 1000)
    print(f'Downloading {len(symbols)} Binance USDT perpetuals, {first_day}..{last_day}')
    path.unlink(missing_ok=True)
    database = sqlite3.connect(path)
    database.executescript(
        'CREATE TABLE ohlcv_data (pair TEXT NOT NULL, date INTEGER NOT NULL, open REAL NOT NULL, '
        'high REAL NOT NULL, low REAL NOT NULL, close REAL NOT NULL, volume REAL NOT NULL, '
        'quote_volume REAL, PRIMARY KEY(pair, date));'
        'CREATE TABLE market_volume_rank_daily (date INTEGER NOT NULL, rank INTEGER NOT NULL, '
        'pair TEXT NOT NULL, quote_volume REAL NOT NULL, PRIMARY KEY(date, rank), UNIQUE(date, pair));')
    volumes = {}
    for index, symbol in enumerate(symbols, 1):
        for kline in binance.daily_klines(symbol, start_ms, end_ms):
            open_time = int(kline[0])
            if open_time < start_ms or open_time > end_ms:
                continue
            day = day_key(dt.datetime.fromtimestamp(open_time / 1000, dt.timezone.utc).date())
            # kline: [open time, open, high, low, close, base volume, close time, quote volume, ...]
            row = (symbol, day, *map(float, kline[1:6]), float(kline[7]))
            database.execute('INSERT INTO ohlcv_data VALUES (?, ?, ?, ?, ?, ?, ?, ?)', row)
            volumes.setdefault(day, []).append((symbol, row[7]))
        if index % 100 == 0 or index == len(symbols):
            print(f'  {index}/{len(symbols)} symbols')
    for day, rows in volumes.items():
        rows.sort(key=lambda item: (-item[1], item[0]))  # as the service: volume desc, then symbol
        database.executemany('INSERT INTO market_volume_rank_daily VALUES (?, ?, ?, ?)',
                             [(day, rank, symbol, volume) for rank, (symbol, volume)
                              in enumerate(rows[:RANKING_SIZE], 1)])
    database.commit()
    days = sorted(volumes)
    database.close()
    return days


# ---------------------------------------------------------------- account
def kraken_account(args, by_source):
    """The Kraken account as execution-state sees it (live_venue_adapter.cpp snapshot):
    cash = USD balance - entry value of open positions (spot convention), positions in
    source units, collateral = the other currencies' value after Kraken's haircut."""
    import kraken_account as kraken
    reader = kraken.reader_from_credentials(Path(args.credentials))
    bundle = reader.snapshot()
    currencies = bundle['wallets']['accounts']['flex']['currencies']
    usd = sum(float(row['quantity']) for code, row in currencies.items() if code.upper() == 'USD')
    collateral = sum(float(row['collateral']) for code, row in currencies.items() if code.upper() != 'USD')
    by_venue = {row['venue_symbol']: row for row in by_source.values()}
    positions, entry_value, unmapped = {}, 0.0, []
    for row in bundle['positions'].get('openPositions', []):
        size = float(row['size']) * (1 if row['side'] == 'long' else -1)
        entry_value += size * float(row['price'])
        mapping = by_venue.get(row['symbol'])
        if mapping is None:
            unmapped.append(row['symbol'])
            positions[row['symbol']] = positions.get(row['symbol'], 0.0) + size
        else:
            source = mapping['source']
            positions[source] = positions.get(source, 0.0) + size / float(mapping['venue_units_per_source_unit'])
    if unmapped:
        print(f'WARNING: Kraken positions outside the symbol map kept under their venue name: {unmapped}')
    return {'cash': usd - entry_value, 'collateral_usd': collateral, 'positions': positions,
            'source': 'kraken', 'usd_balance': usd}


# ---------------------------------------------------------------- plan
def plan(args):
    profile, by_source = load_profile()
    if not PLANNER.exists():
        raise SystemExit(f'ERROR: {PLANNER} missing; build it: meson compile -C build algotrading_standby_plan')
    today = dt.datetime.now(dt.timezone.utc).date()
    decision = dt.date.fromisoformat(args.date) if args.date else today - dt.timedelta(days=1)
    if decision >= today:
        raise SystemExit('ERROR: the decision day must be a completed UTC day (yesterday or earlier)')
    if args.days < WARMUP_DAYS + 1:
        raise SystemExit(f'ERROR: --days must be at least {WARMUP_DAYS + 1}')
    first = decision - dt.timedelta(days=args.days - 1)

    run = RUNS / decision.isoformat()
    run.mkdir(parents=True, exist_ok=True)
    database = run / 'market.sqlite'
    if args.reuse_data and database.exists():
        with sqlite3.connect(database) as existing:
            days = [row[0] for row in existing.execute('SELECT DISTINCT date FROM ohlcv_data ORDER BY date')]
        print(f'Reusing {database} ({len(days)} days)')
    else:
        days = build_market_database(database, first, decision)
    if not days or days[-1] != day_key(decision):
        raise SystemExit(f'ERROR: Binance has no completed data for {decision}')

    if args.account_file:
        account = json.loads(Path(args.account_file).read_text(encoding='utf-8'))
        account.setdefault('collateral_usd', 0.0)
        account['source'] = str(args.account_file)
    else:
        account = kraken_account(args, by_source)

    # Settle the signal state over the days whose full window is available.
    earliest = day_key(first + dt.timedelta(days=WARMUP_DAYS - 1))
    settle = [day for day in days if earliest <= day < day_key(decision)]
    commission = profile['live_commission_rate'] if profile['mode'] == 'live' else args.paper_commission
    manifest = {'database': str(database), 'strategy_config': str(PROFILE / 'strategy.json'),
                'portfolio_config': str(PROFILE / 'portfolio.json'), 'decision_date': day_key(decision),
                'settle_days': settle, 'warmup_days': WARMUP_DAYS, 'top_n': TOP_N,
                'initial_cash': 100000.0, 'commission_rate': commission,
                'account': {key: account[key] for key in ('cash', 'collateral_usd', 'positions')},
                'output': str(run / 'plan-raw.json')}
    (run / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    result = subprocess.run([str(PLANNER), str(run / 'manifest.json')], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f'ERROR: planner failed: {result.stderr.strip() or result.stdout.strip()[-500:]}')
    raw = json.loads((run / 'plan-raw.json').read_text(encoding='utf-8'))

    orders = []
    for order in raw['orders']:
        mapping = by_source.get(order['coin'])
        row = dict(order)
        if mapping is None:
            row.update(kraken_symbol=None, kraken_quantity=None, status='SKIPPED: not on Kraken')
        else:
            row.update(kraken_symbol=mapping['venue_symbol'],
                       kraken_quantity=order['quantity_at_close'] * float(mapping['venue_units_per_source_unit']),
                       status='PLANNED')
        orders.append(row)
    output = {'contractVersion': 'standby-plan-v2', 'release': profile['release'], 'mode': profile['mode'],
              'strategy_file': profile['strategy_file'], 'portfolio_file': profile['portfolio_file'],
              'decision_date': raw['decisionDate'], 'execution_date': raw['executionDate'],
              'account': {'source': account['source'], 'cash': account['cash'],
                          'collateral_usd': account['collateral_usd'], 'positions': account['positions']},
              'reference_capital': raw['referenceCapital'], 'signals': raw['signals'],
              'decisions': raw['decisions'], 'orders': orders,
              'sent': False}
    (run / 'plan.json').write_text(json.dumps(output, indent=2) + '\n', encoding='utf-8')
    print_plan(output)
    print(f'\nPlan written to {run / "plan.json"}. Nothing was sent.')


def print_plan(output):
    print(f"\nRelease {output['release']} ({output['mode']}), close {output['decision_date']} -> "
          f"open {output['execution_date']}")
    account = output['account']
    print(f"Account ({account['source']}): cash {account['cash']:,.2f} USD, collateral "
          f"{account['collateral_usd']:,.2f} USD, {sum(1 for q in account['positions'].values() if q)} positions")
    active = sorted(coin for coin, value in output['signals'].items() if value)
    print(f"Sizing capital {output['reference_capital']:,.2f} USD. Signals ({len(active)}): {', '.join(active) or 'none'}")
    for decision in sorted(output['decisions'], key=lambda row: row['coin']):
        print(f"  {decision['coin']:16} {decision['action']:6} weight {decision['target_weight']:.4f}  "
              f"target {decision['target_notional_usd']:12,.2f} USD")
    if not output['orders']:
        print('Orders: none')
        return
    print(f"\n{'Binance':16} {'Side':5} {'USD':>12} {'Close(T)':>14} {'Qty at close':>16}  {'Kraken':14} {'Contracts':>16}  Status")
    for order in sorted(output['orders'], key=lambda row: row['coin']):
        kraken_quantity = '' if order['kraken_quantity'] is None else f"{order['kraken_quantity']:.6g}"
        print(f"{order['coin']:16} {order['side']:5} {order['notional_usd']:12,.2f} {order['reference_close']:14.6g} "
              f"{order['quantity_at_close']:16.6g}  {order['kraken_symbol'] or '-':14} {kraken_quantity:>16}  {order['status']}")
    print('Quantities are estimated at close(T); the chain resolves them at the next open from the USD amounts.')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    sync_parser = commands.add_parser('sync', help='copy the deployed profile from the VPS (read-only)')
    plan_parser = commands.add_parser('plan', help='orders the chain would send at the next open (never sent)')
    for command in (sync_parser,):
        command.add_argument('--host', default='ubuntu@15.237.145.179')
        command.add_argument('--ssh-key', default='~/.ssh/algotrading-spain.pem')
    plan_parser.add_argument('--date', help='decision day YYYY-MM-DD (default: yesterday, the last completed day)')
    plan_parser.add_argument('--days', type=int, default=130,
                             help='daily bars to download: 100 of window plus days to settle the signals (default 130)')
    source = plan_parser.add_mutually_exclusive_group()
    source.add_argument('--account-file', help='account JSON instead of reading Kraken')
    source.add_argument('--credentials', default=str(ROOT / 'storage/kraken_shadow/.env'),
                        help='read-only Kraken key file (default storage/kraken_shadow/.env)')
    plan_parser.add_argument('--reuse-data', action='store_true',
                             help="reuse this decision day's downloaded market database")
    plan_parser.add_argument('--paper-commission', type=float, default=0.0005,
                             help='commission of the settling replay in PAPER mode (signals do not depend on it)')
    args = parser.parse_args()
    if args.command == 'sync':
        sync(args)
    else:
        plan(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
