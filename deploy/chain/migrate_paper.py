#!/usr/bin/env python3
"""Move the older algotrading-paper stack into a chain installation, keeping its history.

Copies only: the old Docker volumes and storage folders stay untouched as a fallback.

  (default)  read-only check: find every source, show sizes and the settings to be used
  --apply    copy into <root>/data and <root>/config, update chain.env and secrets,
             give the copied PostgreSQL the chain password, verify every file by checksum

Before --apply, stop the old stack (on the VPS: systemctl stop algotrading-paper
algotrading-paper-comparison algotrading-paper-monitor) and keep the chain down.
Secret values are copied file to file and never printed.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
IN_REPOSITORY = HERE.parent.name == 'deploy'
HELPER_IMAGE = 'alpine:3.20'

# (old Compose service, mount point inside it, folder under <root>/data)
VOLUMES = [
    ('postgres', '/var/lib/postgresql/data', 'postgres'),
    ('nats', '/data', 'nats'),
    ('market-data', '/data/market', 'market'),
    ('dashboard-watchdog', '/data/watchdog', 'dashboard/watchdog'),
    ('dashboard-api', '/data/manual-audit', 'dashboard/audit'),
    ('dashboard-api', '/data/alert-ack', 'dashboard/alerts'),
    ('dashboard-alert-notifier', '/data/notifier', 'dashboard/notifier'),
]
# Values the chain hard-codes; the old stack must match or the history would not resume.
CHAIN_STREAM = 'ALGOTRADING_PAPER_RUNTIME'
CHAIN_FIXED_FLAGS = {'--market-top-n': '50', '--market-history-buffer-days': '5', '--strategy': '1:Pure_RSI'}
IMAGE_PORTFOLIO_PATH = '/opt/algotrading/config/portfolio/'


def run(*args, capture=True, stdin=None):
    result = subprocess.run(args, check=True, text=True, input=stdin,
                            stdout=subprocess.PIPE if capture else None, stderr=subprocess.PIPE)
    return result.stdout if capture else ''


def flag(command, name):
    """Value after a command-line flag, or None."""
    return command[command.index(name) + 1] if name in command[:-1] else None


def environment(container):
    return dict(entry.split('=', 1) for entry in container['Config']['Env'] or [])


def read_env_file(path):
    values = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if '=' in line and not line.lstrip().startswith('#'):
                key, value = line.split('=', 1)
                values[key.strip()] = value
    return values


def update_env_file(path, values):
    """Set KEY=value lines in place; other lines and comments stay as they are."""
    lines = path.read_text().splitlines() if path.exists() else []
    pending = dict(values)
    for index, line in enumerate(lines):
        key = line.split('=', 1)[0].strip()
        if '=' in line and not line.lstrip().startswith('#') and key in pending:
            lines[index] = f'{key}={pending.pop(key)}'
    lines += [f'{key}={value}' for key, value in pending.items()]
    path.write_text('\n'.join(lines) + '\n')


class Source:
    """Everything the old stack holds, found from its containers (running or stopped)."""

    def __init__(self, project, storage, shadow_study):
        names = run('docker', 'ps', '-a', '--filter', f'label=com.docker.compose.project={project}',
                    '--format', '{{.Names}}').split()
        if not names:
            raise SystemExit(f'ERROR: no containers of Compose project {project!r}')
        self.containers = {}
        for container in json.loads(run('docker', 'inspect', *names)):
            service = container['Config']['Labels'].get('com.docker.compose.service')
            self.containers[service] = container
        missing = {service for service, _, _ in VOLUMES} - set(self.containers)
        if missing:
            raise SystemExit(f'ERROR: project {project!r} has no container for {sorted(missing)}')
        self.running = sorted(c['Name'].lstrip('/') for c in self.containers.values() if c['State']['Running'])
        self.comparison = storage / 'paper_trading/comparison'
        self.market_config = storage / 'paper_trading/market_data_config.json'
        self.shadow = storage / 'kraken_shadow' / shadow_study

    def command(self, service):
        return self.containers[service]['Config']['Cmd'] or []

    def volume(self, service, destination):
        """Docker volume name or host path mounted at destination."""
        for mount in self.containers[service]['Mounts']:
            if mount['Destination'] == destination:
                return mount['Name'] if mount['Type'] == 'volume' else mount['Source']
        raise SystemExit(f'ERROR: {service} has nothing mounted at {destination}')

    def strategy_file(self):
        """(contents, suggested name). The activity overlay bind-mounts its own strategy file."""
        inside = flag(self.command('strategy'), '--strategy-config')
        for mount in self.containers['strategy']['Mounts']:
            if mount['Type'] == 'bind' and mount['Destination'] == inside:
                # Read through Docker: Docker Desktop reports an internal path, not the host one.
                text = run('docker', 'run', '--rm', '--network', 'none', '-v', f'{mount["Source"]}:/file:ro',
                           HELPER_IMAGE, 'cat', '/file')
                return text, Path(mount['Source']).name
        image = self.containers['strategy']['Config']['Image']
        return image_file(image, inside), Path(inside).name


def image_file(image, path):
    return run('docker', 'run', '--rm', '--network', 'none', '--entrypoint', 'cat', image, path)


def settings(source, config):
    """Non-secret chain.env values and the problems that block the migration."""
    problems = []
    simulated, strategy = source.command('simulated-exchange'), source.command('strategy')
    risk, execution = source.command('portfolio-risk'), source.command('execution-state')
    dashboard = environment(source.containers['dashboard-api'])
    notifier = environment(source.containers['dashboard-alert-notifier'])
    for service in ('strategy', 'portfolio-risk', 'execution-state', 'simulated-exchange', 'order-planner'):
        if flag(source.command(service), '--stream') != CHAIN_STREAM:
            problems.append(f'{service} uses another stream than {CHAIN_STREAM}')
    for name, expected in CHAIN_FIXED_FLAGS.items():
        for command in (strategy, risk, execution):
            if name in command and flag(command, name) != expected:
                problems.append(f'{name} is {flag(command, name)}, the chain uses {expected}')
    if flag(execution, '--initial-cash') != flag(simulated, '--initial-cash'):
        problems.append('execution-state and simulated-exchange disagree on --initial-cash')

    # Strategy and portfolio files must be byte-identical: services check their stored identity.
    image = source.containers['strategy']['Config']['Image']
    strategy_text, strategy_name = source.strategy_file()
    # Prefer the installed file with the same contents (keeps the name the comparison expects).
    same = [path.name for path in sorted((config / 'strategies').glob('*.json')) if path.read_text() == strategy_text]
    strategy_name = same[0] if same else strategy_name
    portfolio_name = Path(flag(risk, '--portfolio-config')).name
    portfolio_text = image_file(image, IMAGE_PORTFOLIO_PATH + portfolio_name)
    files = {f'strategies/{strategy_name}': strategy_text, f'portfolio/{portfolio_name}': portfolio_text}
    for relative, text in files.items():
        target = config / relative
        if target.exists() and target.read_text() != text:
            problems.append(f'{target} differs from the file the old stack uses')

    values = {
        'STRATEGY_FILE': strategy_name,
        'PORTFOLIO_FILE': portfolio_name,
        'INITIAL_CASH': flag(simulated, '--initial-cash'),
        'COMMISSION_RATE': flag(simulated, '--commission-rate'),
        'MARKET_WARMUP_DAYS': flag(strategy, '--market-warmup-days'),
        'RSI_ENTRY': dashboard.get('DASHBOARD_RSI_ENTRY', '80'),
        'RSI_EXIT': dashboard.get('DASHBOARD_RSI_EXIT', '70'),
        'QUOTE_VOLUME_FROM': dashboard.get('DASHBOARD_QUOTE_VOLUME_FROM', ''),
        'PAPER_COMPARISON_FILE': '/data/comparison/comparison.json' if dashboard.get('DASHBOARD_PAPER_COMPARISON_FILE') else '',
        'NOTIFIER_SINK': notifier.get('DASHBOARD_NOTIFIER_SINK', 'TEST_FILE'),
    }
    secrets = {key: value for key, value in (
        ('DASHBOARD_VIEWER_PASSWORD', dashboard.get('DASHBOARD_VIEWER_PASSWORD', '')),
        ('DASHBOARD_TELEGRAM_BOT_TOKEN', notifier.get('DASHBOARD_TELEGRAM_BOT_TOKEN', '')),
        ('DASHBOARD_TELEGRAM_CHAT_ID', notifier.get('DASHBOARD_TELEGRAM_CHAT_ID', ''))) if value}
    return values, files, secrets, problems


def size(mount):
    return run('docker', 'run', '--rm', '--network', 'none', '-v', f'{mount}:/from:ro', HELPER_IMAGE,
               'du', '-sh', '/from').split()[0]


def copy_volume(mount, target):
    """Copy with owners and modes, then compare per-file SHA-256 inside the same container."""
    target.mkdir(parents=True, exist_ok=True)
    script = ('cp -a /from/. /to/ && chown "$(stat -c %u:%g /from)" /to && chmod "$(stat -c %a /from)" /to && '
              '(cd /from && find . -type f -exec sha256sum {} + | sort -k2) > /tmp/from && '
              '(cd /to && find . -type f -exec sha256sum {} + | sort -k2) > /tmp/to && '
              'cmp -s /tmp/from /tmp/to && wc -l < /tmp/from')
    files = run('docker', 'run', '--rm', '--network', 'none', '-v', f'{mount}:/from:ro', '-v', f'{target}:/to',
                HELPER_IMAGE, 'sh', '-c', script).strip()
    return int(files)


def copy_folder(source, target, rewrites):
    """Host folder copy; absolute paths inside its JSON files move to the new location."""
    shutil.copytree(source, target, symlinks=True)
    for path in target.rglob('*.json'):
        text = path.read_text()
        for old, new in rewrites:
            text = text.replace(str(old), str(new))
        path.write_text(text)
    # Each comparison cycle names its frozen market copy; point it at the new folder
    # whatever the old absolute path was.
    study = target / 'study.json'
    if study.exists():
        journal = json.loads(study.read_text())
        for cycle in journal.get('cycles', []):
            cycle['database'] = str(target / Path(cycle['database']).name)
        study.write_text(json.dumps(journal, indent=2, allow_nan=False) + '\n')
    return sum(1 for path in target.rglob('*') if path.is_file())


def set_postgres_password(data, password):
    """Start the copied cluster alone (no network) and set the chain password over the local socket."""
    name = 'migrate-postgres'
    run('docker', 'run', '-d', '--name', name, '--network', 'none', '-v', f'{data}:/var/lib/postgresql/data',
        'postgres:16-alpine')
    try:
        for _ in range(120):
            if subprocess.run(['docker', 'exec', name, 'pg_isready', '-U', 'algotrading', '-d', 'algotrading_paper'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(0.5)
        else:
            raise SystemExit('ERROR: copied PostgreSQL did not start; see docker logs migrate-postgres')
        psql = ['docker', 'exec', '-i', name, 'psql', '-XAt', '-U', 'algotrading', '-d', 'algotrading_paper',
                '-v', 'ON_ERROR_STOP=1']
        # Password via stdin: never on a command line.
        run(*psql, stdin="ALTER ROLE algotrading PASSWORD '" + password.replace("'", "''") + "';\n")
        return run(*psql, '-c', 'SELECT (SELECT count(*) FROM trading_fills), '
                                 '(SELECT count(*) FROM simulated_exchange_positions);').strip()
    finally:
        subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=None if IN_REPOSITORY else HERE.parent,
                        required=IN_REPOSITORY, help='chain installation root ("/" on the VPS)')
    parser.add_argument('--project', default='algotrading-paper', help='old Compose project')
    parser.add_argument('--storage', type=Path, default=Path('/opt/algotrading/storage'),
                        help='old storage folder (paper_trading/ and kraken_shadow/)')
    parser.add_argument('--shadow-study', default='algotrading-paper-activity-forward')
    parser.add_argument('--dashboard-password', choices=('paper', 'chain'), default='paper',
                        help='keep the old dashboard password (paper) or the installation one (chain)')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()

    root = args.root.resolve()
    data, config, chain = root / 'data', root / 'config', root / 'chain'
    secrets_file = root / 'secrets/chain.env'
    if not (chain / 'chain.env').exists() or not secrets_file.exists():
        raise SystemExit(f'ERROR: {root} is not a chain installation (run algo install first)')
    source = Source(args.project, args.storage.resolve(), args.shadow_study)
    values, files, secrets, problems = settings(source, config)
    if args.dashboard_password == 'chain':
        secrets.pop('DASHBOARD_VIEWER_PASSWORD', None)

    folders = [(source.comparison, data / 'comparison'), (source.shadow, data / 'kraken_shadow/paper-forward')]
    print(f'Old project {args.project} -> {root}')
    for service, destination, target in VOLUMES:
        mount = source.volume(service, destination)
        print(f'  {mount:<48} {size(mount):>6}  -> data/{target}')
    for folder, target in folders:
        state = 'missing, skipped' if not folder.exists() else f'{sum(1 for p in folder.rglob("*") if p.is_file())} files'
        print(f'  {str(folder):<48} {state:>6}  -> {target.relative_to(root)}')
    print(f'  {source.market_config} -> config/market_data.json')
    for relative in files:
        print(f'  strategy/portfolio file -> config/{relative}')
    print('chain.env:', ', '.join(f'{key}={value}' for key, value in values.items()))
    print('secrets copied (values hidden):', ', '.join(secrets) or 'none', '+ POSTGRES_PASSWORD set in the copied database')

    chain_running = run('docker', 'ps', '-q', '--filter', 'label=com.docker.compose.project=algotrading').split()
    if source.running:
        problems.append('old stack still running: ' + ', '.join(source.running))
    if chain_running:
        problems.append('chain containers are running; run algo down first')
    for _, _, target in VOLUMES:
        if (data / target).exists() and any((data / target).iterdir()):
            problems.append(f'data/{target} is not empty')
    for _, target in folders:
        if target.exists():
            problems.append(f'{target} already exists')
    if problems:
        print('\nBlocking problems:' if args.apply else '\nTo fix before --apply:')
        for problem in problems:
            print('  -', problem)
        if args.apply:
            raise SystemExit(1)
    if not args.apply:
        print('\nCheck only; nothing changed. Run again with --apply.')
        return

    report = {'migratedAt': datetime.now(timezone.utc).isoformat(), 'project': args.project, 'volumes': [], 'folders': []}
    for service, destination, target in VOLUMES:
        mount = source.volume(service, destination)
        count = copy_volume(mount, data / target)
        report['volumes'].append({'source': mount, 'target': f'data/{target}', 'files': count, 'checksums': 'equal'})
        print(f'Copied {mount} -> data/{target} ({count} files, checksums equal)')
    rewrites = [(source.comparison, data / 'comparison'), (source.shadow, data / 'kraken_shadow/paper-forward')]
    for folder, target in folders:
        if folder.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            count = copy_folder(folder, target, rewrites)
            report['folders'].append({'source': str(folder), 'target': str(target), 'files': count})
            print(f'Copied {folder} -> {target} ({count} files)')

    market = config / 'market_data.json'
    if market.exists():
        shutil.copy2(market, market.with_suffix('.json.before-migration'))
    shutil.copy2(source.market_config, market)
    for relative, text in files.items():
        (config / relative).parent.mkdir(parents=True, exist_ok=True)
        (config / relative).write_text(text)

    update_env_file(chain / 'chain.env', values)
    update_env_file(secrets_file, secrets)
    os.chmod(secrets_file, 0o600)
    password = read_env_file(secrets_file).get('POSTGRES_PASSWORD')
    if not password:
        raise SystemExit(f'ERROR: POSTGRES_PASSWORD missing in {secrets_file}')
    fills, positions = set_postgres_password(data / 'postgres', password).split('|')
    report.update(settings=values, secretKeys=sorted(secrets), fills=int(fills), positions=int(positions))
    print(f'PostgreSQL ready with the chain password ({fills} fills, {positions} positions)')

    report_file = root / 'reports' / f'migration-{datetime.now(timezone.utc):%Y%m%d_%H%M%S}.json'
    report_file.parent.mkdir(parents=True, exist_ok=True)
    report_file.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Report: {report_file}\nNext: {chain}/algo up, then compare the dashboard with the old one.')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        # Never echo the command line: some carry credentials.
        print(f'ERROR: a docker step failed (exit {error.returncode}): {(error.stderr or "").strip()[:400]}',
              file=sys.stderr)
        sys.exit(1)
