#!/usr/bin/env python3
"""Publish bounded, non-secret Linux host and project-container telemetry for the dashboard."""
import argparse
import importlib.util
import hashlib
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

# Short service names of chain/compose.yml (default selection). MODE=live replaces the PAPER
# gateway and simulated exchange with gateway-kraken (see default_services).
CHAIN_SERVICES = ('nats', 'postgres', 'market-data', 'strategy', 'risk', 'execution', 'planner',
                  'gateway', 'sim-exchange', 'dash-api', 'dash-web', 'watchdog', 'notifier')
LIVE_CHAIN_SERVICES = ('nats', 'postgres', 'market-data', 'strategy', 'risk', 'execution', 'planner',
                       'gateway-kraken', 'dash-api', 'dash-web', 'watchdog', 'notifier')
# Service names retained by the isolated PAPER validation fixture.
LEGACY_SERVICES = ('nats', 'postgres', 'market-data', 'strategy', 'portfolio-risk',
                   'execution-state', 'order-planner', 'exchange-gateway', 'simulated-exchange',
                   'dashboard-api', 'dashboard-web', 'dashboard-watchdog', 'dashboard-alert-notifier')
SERVICES = tuple(dict.fromkeys(CHAIN_SERVICES + LIVE_CHAIN_SERVICES + LEGACY_SERVICES))
SECRET = re.compile(r'(?i)(password|passwd|secret|token|api[_-]?key|authorization)[" \x27]*[:=]|[a-z]+://[^ /]+:[^ /]+@|bot\d+:[a-z0-9_-]+|-----BEGIN')


TRADING_PROCESSES = {
    'market-data': 'algotrading_market_data_service',
    'strategy': 'algotrading_strategy_service',
    'risk': 'algotrading_portfolio_risk_service',
    'execution': 'algotrading_execution_state_service',
    'planner': 'algotrading_order_planner_service',
    'gateway': 'algotrading_exchange_gateway',
    'gateway-kraken': 'algotrading_exchange_gateway',
    'sim-exchange': 'algotrading_simulated_exchange_service',
    'portfolio-risk': 'algotrading_portfolio_risk_service',
    'execution-state': 'algotrading_execution_state_service',
    'order-planner': 'algotrading_order_planner_service',
    'exchange-gateway': 'algotrading_exchange_gateway',
    'simulated-exchange': 'algotrading_simulated_exchange_service',
}


def default_services(chain_env=Path(__file__).resolve().parent / 'chain.env'):
    """Chain services of the installed mode: MODE in the chain.env next to this script."""
    mode = 'paper'
    try:
        for line in chain_env.read_text().splitlines():
            if line.startswith('MODE='):
                mode = line.split('=', 1)[1].strip()
    except OSError:
        pass  # A checkout or fixture without chain.env watches the PAPER chain.
    return LIVE_CHAIN_SERVICES if mode == 'live' else CHAIN_SERVICES


def clock_status():
    """Read the collector host's NTP status; never change the clock or invent an offset."""
    try:
        result = subprocess.run(['timedatectl', 'show', '--property=NTPSynchronized', '--value'],
                                capture_output=True, text=True, timeout=3)
        value = result.stdout.strip()
        if result.returncode == 0 and value in ('yes', 'no'):
            return {'clockObserved': True, 'clockSynced': value == 'yes',
                    'clockOffsetLabel': 'Host systemd NTP: ' + ('synchronized' if value == 'yes' else 'not synchronized') + '; offset not measured'}
    except (OSError, subprocess.TimeoutExpired):
        pass
    return {'clockObserved': False, 'clockSynced': False,
            'clockOffsetLabel': 'Host NTP status unavailable; offset not measured'}


def trading_process(service, container_id, running):
    """External process observation, distinct from an application heartbeat or readiness."""
    row = {'service': service, 'processState': 'UNKNOWN', 'detail': 'Process probe unavailable'}
    if not running:
        return dict(row, processState='STOPPED', processRunning=False, detail='Container is not running')
    try:
        # comm excludes arguments, including database credentials. Linux comm is limited to 15 characters.
        output = command(['docker', 'top', container_id, '-eo', 'pid,comm,stat'])
        states = []
        for line in output.splitlines()[1:]:
            fields = line.split()
            if len(fields) == 3 and fields[1] == TRADING_PROCESSES[service][:15]:
                states.append(fields[2])
        if not states:
            return dict(row, processState='MISSING', processRunning=False, detail='Trading executable absent; container alone is not sufficient')
        if any(state[0] not in 'ZXTt' for state in states):
            return dict(row, processState='RUNNING', processRunning=True, detail='Executable observed by Docker top; responsiveness not measured')
        return dict(row, processState='STOPPED', processRunning=False, detail='Executable stopped or defunct')
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        return row


def capture_logs(project, root):
    """Local fallback; VPS uses the persistent rsyslog receiver instead."""
    source = Path(__file__).resolve().parent / 'logging/clean_daily_logs.py'
    spec = importlib.util.spec_from_file_location('paper_daily_logs', source)
    writer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(writer)
    writer.validate_root(root)
    root.mkdir(parents=True, exist_ok=True)
    # Inside the log root, next to the service folders; retention ignores plain files.
    cursor_file = root / '.capture_cursors.json'
    cursors = json.loads(cursor_file.read_text()) if cursor_file.exists() else {}
    identifiers = command(['docker', 'ps', '-aq', '--filter', f'label=com.docker.compose.project={project}']).split()
    inspected = json.loads(command(['docker', 'inspect', *identifiers])) if identifiers else []
    for container in inspected:
        name = container['Config']['Labels'].get('com.docker.compose.service', '')
        if name not in SERVICES:
            continue
        identifier = container['Id']
        cursor = cursors.get(identifier, {'timestamp': '1970-01-01T00:00:00Z', 'hashes': []})
        process = subprocess.Popen(['docker', 'logs', '--timestamps', '--since', cursor['timestamp'], identifier],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        latest = cursor['timestamp']
        hashes = list(cursor['hashes'])
        for raw in process.stdout:
            text = raw.decode('utf-8', errors='replace').rstrip('\r\n')
            timestamp, _, content = text.partition(' ')
            if not re.match(r'^\d{4}-\d{2}-\d{2}T', timestamp):
                continue
            signature = hashlib.sha256(raw).hexdigest()
            if timestamp == cursor['timestamp'] and signature in cursor['hashes']:
                continue
            if timestamp[:10] < (datetime.now(timezone.utc).date()).isoformat():
                # The receiver retains yesterday as well; older backlog is unnecessary.
                if timestamp[:10] < (datetime.now(timezone.utc).date() - timedelta(days=1)).isoformat():
                    continue
            content = '[REDACTED: credential-bearing diagnostic]' if SECRET.search(content) else content
            writer.append_record(root, f'{timestamp} service={name} {content}\n'.encode())
            if timestamp != latest:
                latest, hashes = timestamp, []
            hashes.append(signature)
        if process.wait(timeout=15) == 0:
            cursors[identifier] = {'timestamp': latest, 'hashes': hashes[-2000:]}
        else:
            raise RuntimeError('Local diagnostic capture failed')
    # Discard cursors for removed containers, while retaining their daily logs.
    cursors = {item['Id']: cursors[item['Id']] for item in inspected if item['Id'] in cursors}
    temporary = cursor_file.with_suffix('.tmp')
    temporary.write_text(json.dumps(cursors) + '\n')
    temporary.replace(cursor_file)
    writer.clean(root, datetime.now(timezone.utc).date())
    writer.trim(root)


def command(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RuntimeError('Docker telemetry query failed')
    return result.stdout


def cpu_ticks():
    values = [int(value) for value in Path('/proc/stat').read_text().splitlines()[0].split()[1:]]
    # guest time is already included in user/nice; exclude it from the total.
    return sum(values[:8]), values[3] + values[4]


def memory_mib(text):
    match = re.fullmatch(r'([\d.]+)\s*(B|kB|MB|GB|KiB|MiB|GiB)', text.strip())
    if not match:
        raise ValueError('Unsupported Docker memory unit')
    scale = {'B': 1, 'kB': 1000, 'MB': 1000**2, 'GB': 1000**3,
             'KiB': 1024, 'MiB': 1024**2, 'GiB': 1024**3}
    return float(match[1]) * scale[match[2]] / 1024**2


def container_observation(name, container):
    """Describe Docker health separately from the trading executable probe."""
    state = container['State']
    running = bool(state['Running'])
    check = state.get('Health', {}).get('Status')
    if not running or check == 'unhealthy':
        health = 'CRITICAL'
    elif check == 'healthy':
        health = 'HEALTHY'
    else:
        health = 'WARN'
    if running:
        lifecycle = 'RUNNING'
    elif state.get('Restarting'):
        lifecycle = 'RESTARTING'
    else:
        lifecycle = 'STOPPED'
    row = {'name': name, 'state': lifecycle, 'health': health,
           'restartCount': container['RestartCount'],
           'uptimeLabel': 'Started ' + state['StartedAt'][:19] + ' UTC' if running else 'Stopped',
           'cpuPct': -1, 'ramMb': -1,
           'lastHeartbeat': 'Container observed; no application heartbeat' if running else 'Container not running'}
    if name in TRADING_PROCESSES:
        row['process'] = trading_process(name, container['Id'], running)
    return row


def containers(project, expected):
    identifiers = command(['docker', 'ps', '-aq', '--filter', f'label=com.docker.compose.project={project}']).split()
    rows = {}
    if identifiers:
        inspected = json.loads(command(['docker', 'inspect', *identifiers]))
        running_ids = []
        names = {}
        for item in inspected:
            name = item['Config']['Labels'].get('com.docker.compose.service', '')
            if name not in expected or item['Config']['Labels'].get('com.docker.compose.oneoff') == 'True':
                continue
            state = item['State']
            running = bool(state['Running'])
            if running:
                running_ids.append(item['Id'])
                names[item['Name'].lstrip('/')] = name
            rows[name] = container_observation(name, item)
        # A terminated container can return no stats; absence remains unknown.
        output = command(['docker', 'stats', '--no-stream', '--format', '{{json .}}', *running_ids]) if running_ids else ''
        for line in output.splitlines():
            line = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', line)
            if not line.strip():
                continue
            sample = json.loads(line)
            name = names.get(sample.get('Name'))
            if name in rows and rows[name]['state'] == 'RUNNING':
                rows[name]['cpuPct'] = float(sample['CPUPerc'].rstrip('%'))
                rows[name]['ramMb'] = round(memory_mib(sample['MemUsage'].split('/')[0]), 2)
    return [rows.get(name, {'name': name, 'state': 'STOPPED', 'health': 'CRITICAL', 'restartCount': 0,
                           'uptimeLabel': 'Not found', 'cpuPct': -1, 'ramMb': -1,
                           'lastHeartbeat': 'Expected service missing'}) for name in expected]


def sample(project, disk_path, expected, previous):
    # Slow later probes must not make early measurements appear newly collected.
    observed_at = datetime.now(timezone.utc).isoformat()
    ticks = cpu_ticks()
    total = ticks[0] - previous[0]
    cpu = 100 * (1 - (ticks[1] - previous[1]) / total) if total > 0 else 0
    memory = {line.split(':')[0]: int(line.split()[1]) * 1024
              for line in Path('/proc/meminfo').read_text().splitlines()}
    ram_used = memory['MemTotal'] - memory['MemAvailable']
    disk = os.statvfs(disk_path)
    disk_total = disk.f_blocks * disk.f_frsize
    disk_used = (disk.f_blocks - disk.f_bfree) * disk.f_frsize
    uptime = float(Path('/proc/uptime').read_text().split()[0])
    host = {'cpuPct': round(cpu, 2), 'ramPct': round(100 * ram_used / memory['MemTotal'], 2),
            'diskPct': round(100 * disk_used / disk_total, 2), 'load1m': os.getloadavg()[0],
            'ramLabel': f'{ram_used / 1024**2:.0f} / {memory["MemTotal"] / 1024**2:.0f} MiB',
            'diskLabel': f'{disk_used / 1024**3:.1f} / {disk_total / 1024**3:.1f} GiB',
            'diskAvailableGiB': round(disk.f_bavail * disk.f_frsize / 1024**3, 2),
            # Numeric values for the resource history charts.
            'ramUsedMiB': round(ram_used / 1024**2, 1), 'ramTotalMiB': round(memory['MemTotal'] / 1024**2, 1),
            'diskUsedGiB': round(disk_used / 1024**3, 3), 'diskTotalGiB': round(disk_total / 1024**3, 3),
            'cpuCount': os.cpu_count() or 1,
            'uptimeLabel': f'{uptime / 86400:.1f} days', 'clockOffsetLabel': 'Not measured',
            'clockObserved': False, 'clockSynced': False, 'networkRxLabel': 'Not measured',
            'networkTxLabel': 'Not measured', 'state': 'HEALTHY'}
    host.update(clock_status())
    if max(host['ramPct'], host['diskPct']) >= 95:
        host['state'] = 'CRITICAL'
    elif max(cpu, host['ramPct'], host['diskPct']) >= 85 or host['diskAvailableGiB'] < 15:
        host['state'] = 'WARN'
    errors = []
    try:
        rows = containers(project, expected)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        rows = []
        errors.append('Container telemetry unavailable')
    scope = 'WSL Linux host; Docker Desktop containers are a separate scope' if 'microsoft' in os.uname().release.lower() else 'Linux host'
    processes = [row.pop('process', {'service': row['name'], 'processState': 'STOPPED',
                                    'processRunning': False, 'detail': 'Expected service missing'})
                 for row in rows if row['name'] in TRADING_PROCESSES]
    return {'schemaVersion': 1, 'project': project, 'observedAt': observed_at,
            'scope': scope, 'vps': host, 'containers': rows, 'processes': processes, 'errors': errors}, ticks


def history_record(result):
    """One compact time-series row: host totals and per-container CPU/RAM.

    Docker reports container CPU as a percentage of one core; it is divided by the host
    core count so that containers and the host share one scale (percent of host capacity).
    Unavailable values are null, never zero.
    """
    host = result['vps']
    cores = max(1, host['cpuCount'])
    containers = {}
    for row in result['containers']:
        if row['state'] != 'RUNNING':
            continue
        cpu = round(row['cpuPct'] / cores, 3) if row['cpuPct'] >= 0 else None
        ram = row['ramMb'] if row['ramMb'] >= 0 else None
        containers[row['name']] = [cpu, ram]
    return {'t': result['observedAt'], 'cpuPct': host['cpuPct'], 'ramUsedMiB': host['ramUsedMiB'],
            'ramTotalMiB': host['ramTotalMiB'], 'diskUsedGiB': host['diskUsedGiB'],
            'diskTotalGiB': host['diskTotalGiB'], 'containers': containers}


def append_history(path, record):
    with path.open('a', encoding='utf-8') as handle:
        handle.write(json.dumps(record, allow_nan=False, separators=(',', ':')) + '\n')


def trim_history(path, days):
    """Keep only the last `days` of history (rewritten atomically)."""
    if not path.exists():
        return
    cutoff = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()
    kept = [line for line in path.read_text(encoding='utf-8').splitlines()
            if line.startswith('{"t":"') and line[6:6 + len(cutoff)] >= cutoff]
    temporary = path.with_suffix('.tmp')
    temporary.write_text(''.join(line + '\n' for line in kept), encoding='utf-8')
    temporary.chmod(0o644)
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disk-path', type=Path, default=Path('/'))
    parser.add_argument('--services', help='Comma-separated services (default: the chain services of MODE in chain.env)')
    parser.add_argument('--interval', type=float, default=10)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--log-root', type=Path, help='Capture local diagnostics with the two-day/50-MiB policy')
    parser.add_argument('--history-interval', type=float, default=30,
                        help='Seconds between rows of history.jsonl (next to --output)')
    parser.add_argument('--history-days', type=float, default=7, help='History retention in days')
    args = parser.parse_args()
    context = os.environ.get('DOCKER_CONTEXT')
    endpoint = None if context else os.environ.get('DOCKER_HOST')
    if not endpoint:
        endpoint = json.loads(command(['docker', 'context', 'inspect', *([context] if context else [])]))[0]['Endpoints']['docker']['Host']
    if not endpoint.startswith(('unix://', 'npipe://')):
        parser.error('Only local Docker endpoints are supported')
    # 'dashboard' is the local replay dashboard project (research/replay.py dashboard).
    if not re.fullmatch(r'algotrading|algotrading-paper(?:-[a-f0-9]{12})?|dashboard', args.project) or args.interval < 5:
        parser.error('Use the chain, a paper or the local dashboard project and an interval of at least five seconds')
    expected = args.services.split(',') if args.services else list(default_services())
    if not expected or any(name not in SERVICES for name in expected):
        parser.error('Unknown service in telemetry selection')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    os.umask(0o022)
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    history = args.output.with_name('history.jsonl')
    previous = cpu_ticks()
    next_logs = 0
    next_history = 0
    next_history_trim = 0
    time.sleep(.25)
    while not stopping:
        try:
            result, previous = sample(args.project, args.disk_path, expected, previous)
            temporary = args.output.with_suffix('.tmp')
            temporary.write_text(json.dumps(result, allow_nan=False) + '\n')
            temporary.chmod(0o644)
            temporary.replace(args.output)
            if time.monotonic() >= next_history:
                append_history(history, history_record(result))
                next_history = time.monotonic() + args.history_interval
            if time.monotonic() >= next_history_trim:
                trim_history(history, args.history_days)
                next_history_trim = time.monotonic() + 3600
            if args.log_root and time.monotonic() >= next_logs:
                capture_logs(args.project, args.log_root)
                next_logs = time.monotonic() + 60
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
            print('HOST-MONITOR: snapshot failed; dashboard will mark old telemetry stale', flush=True)
        if args.once:
            return
        deadline = time.monotonic() + args.interval
        while not stopping and time.monotonic() < deadline:
            time.sleep(.25)


if __name__ == '__main__':
    main()
