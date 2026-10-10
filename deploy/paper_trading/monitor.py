#!/usr/bin/env python3
"""Publish bounded, non-secret Linux host and project-container telemetry for the dashboard."""
import argparse
import importlib.util
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

SERVICES = ('nats', 'postgres', 'market-data', 'strategy', 'portfolio-risk',
            'execution-state', 'order-planner', 'exchange-gateway', 'simulated-exchange',
            'dashboard-api', 'dashboard-web', 'dashboard-watchdog', 'dashboard-alert-notifier')
SECRET = re.compile(r'(?i)(password|passwd|secret|token|api[_-]?key|authorization)[" \x27]*[:=]|[a-z]+://[^ /]+:[^ /]+@|bot\d+:[a-z0-9_-]+|-----BEGIN')


TRADING_PROCESSES = {
    'market-data': 'algotrading_market_data_service',
    'strategy': 'algotrading_strategy_service',
    'portfolio-risk': 'algotrading_portfolio_risk_service',
    'execution-state': 'algotrading_execution_state_service',
    'order-planner': 'algotrading_order_planner_service',
    'exchange-gateway': 'algotrading_exchange_gateway',
    'simulated-exchange': 'algotrading_simulated_exchange_service',
}


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
        states = [line.split()[2] for line in output.splitlines()[1:]
                  if len(line.split()) == 3 and line.split()[1] == TRADING_PROCESSES[service][:15]]
        if not states:
            return dict(row, processState='MISSING', processRunning=False, detail='Trading executable absent; container alone is not sufficient')
        if any(state[0] not in 'ZXTt' for state in states):
            return dict(row, processState='RUNNING', processRunning=True, detail='Executable observed by Docker top; responsiveness not measured')
        return dict(row, processState='STOPPED', processRunning=False, detail='Executable stopped or defunct')
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        return row


def capture_logs(project, root):
    """Local fallback; VPS uses the persistent rsyslog receiver instead."""
    source = Path(__file__).resolve().parents[1] / 'live/logging/clean_daily_logs.py'
    spec = importlib.util.spec_from_file_location('paper_daily_logs', source)
    writer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(writer)
    writer.validate_root(root)
    root.mkdir(parents=True, exist_ok=True)
    cursor_file = root.parent / 'log_cursors.json'
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
        import hashlib
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
                from datetime import timedelta
                if timestamp[:10] < (datetime.now(timezone.utc).date() - timedelta(days=1)).isoformat():
                    continue
            content = '[REDACTED: credential-bearing diagnostic]' if SECRET.search(content) else content
            writer.append_record(root, f'{timestamp} service=algotrading.{project}.{name} {content}\n'.encode())
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
            check = state.get('Health', {}).get('Status')
            health = 'HEALTHY' if running and check == 'healthy' else 'WARN' if running else 'CRITICAL'
            if check == 'unhealthy':
                health = 'CRITICAL'
            rows[name] = {'name': name, 'state': 'RUNNING' if running else 'RESTARTING' if state.get('Restarting') else 'STOPPED',
                          'health': health, 'restartCount': item['RestartCount'],
                          'uptimeLabel': 'Started ' + state['StartedAt'][:19] + ' UTC' if running else 'Stopped',
                          'cpuPct': -1, 'ramMb': -1,
                          'lastHeartbeat': 'Container observed; no application heartbeat' if running else 'Container not running'}
            if name in TRADING_PROCESSES:
                rows[name]['process'] = trading_process(name, item['Id'], running)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disk-path', type=Path, default=Path('/'))
    parser.add_argument('--services', default=','.join(SERVICES))
    parser.add_argument('--interval', type=float, default=10)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--log-root', type=Path, help='Capture local diagnostics with the two-day/50-MiB policy')
    args = parser.parse_args()
    context = os.environ.get('DOCKER_CONTEXT')
    endpoint = None if context else os.environ.get('DOCKER_HOST')
    if not endpoint:
        endpoint = json.loads(command(['docker', 'context', 'inspect', *([context] if context else [])]))[0]['Endpoints']['docker']['Host']
    if not endpoint.startswith(('unix://', 'npipe://')):
        parser.error('Only local Docker endpoints are supported')
    if not re.fullmatch(r'algotrading-paper(?:-[a-f0-9]{12})?', args.project) or args.interval < 5:
        parser.error('Use a paper project identity and an interval of at least five seconds')
    expected = args.services.split(',')
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
    previous = cpu_ticks()
    next_logs = 0
    time.sleep(.25)
    while not stopping:
        try:
            result, previous = sample(args.project, args.disk_path, expected, previous)
            temporary = args.output.with_suffix('.tmp')
            temporary.write_text(json.dumps(result, allow_nan=False) + '\n')
            temporary.chmod(0o644)
            temporary.replace(args.output)
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
