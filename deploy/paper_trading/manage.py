#!/usr/bin/env python3
"""Prepare and operate the separate current-data paper stack on a local Linux host."""
import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / 'storage/paper_trading'
COMPOSE = Path(__file__).with_name('docker-compose.yml')


def local_docker():
    context = os.environ.get('DOCKER_CONTEXT')
    endpoint = None if context else os.environ.get('DOCKER_HOST')
    if not endpoint:
        result = subprocess.check_output(['docker', 'context', 'inspect', *([context] if context else [])], text=True)
        endpoint = json.loads(result)[0]['Endpoints']['docker']['Host']
    if not endpoint.startswith(('unix://', 'npipe://')):
        raise ValueError('Only local Docker endpoints are supported')


def prepare(port):
    RUN.mkdir(parents=True, exist_ok=True)
    (RUN / 'telemetry').mkdir(exist_ok=True)
    environment = RUN / '.env'
    if environment.exists():
        print('Existing paper configuration preserved:', environment)
        return
    market = json.loads((ROOT / 'deploy/live/market_data_config.json').read_text())
    market.update(stream='ALGOTRADING_PAPER_RUNTIME', publish_paper_execution_prices=True)
    (RUN / 'market_data_config.json').write_text(json.dumps(market, indent=2) + '\n')
    environment.write_text(f'PAPER_PROJECT=algotrading-paper\nPAPER_RUN_DIR={RUN}\n'
                          f'POSTGRES_PASSWORD={secrets.token_hex(24)}\n'
                          f'DASHBOARD_VIEWER_PASSWORD={secrets.token_hex(12)}\n'
                          f'PAPER_DASHBOARD_PORT={port}\nINITIAL_CASH=100000\nCOMMISSION_RATE=0.001\n')
    environment.chmod(0o600)
    print('Paper configuration prepared. Viewer password is in:', environment)


def compose(*args):
    if not (RUN / '.env').exists():
        raise ValueError('Run prepare first')
    return subprocess.run(['docker', 'compose', '--project-name', 'algotrading-paper',
                           '--env-file', str(RUN / '.env'), '-f', str(COMPOSE), *args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'build', 'start', 'stop', 'status', 'monitor', 'run'])
    parser.add_argument('--port', type=int, default=8092)
    args = parser.parse_args()
    if args.action == 'prepare':
        if not 1024 <= args.port <= 65535:
            parser.error('Choose a port between 1024 and 65535')
        prepare(args.port)
        return
    local_docker()
    if args.action == 'build':
        targets = ['algotrading_' + name for name in ('market_data_service', 'strategy_service',
                   'portfolio_risk_service', 'order_planner_service', 'execution_state_service',
                   'exchange_gateway', 'simulated_exchange_service', 'historical_market_data_service')]
        targets.append('algotrading_research')
        subprocess.run(['meson', 'compile', '-C', str(ROOT / 'build'), '-j', '2', *targets], check=True)
        subprocess.run(['bash', str(ROOT / 'deploy/live/build_runtime_bundle.sh'), '--paper'], check=True)
        compose('build')
    elif args.action in ('start', 'run'):
        compose('up', '-d')
        if args.action == 'start':
            print('Paper stack started. Run monitor in another terminal for host readings and daily log capture.')
    elif args.action == 'stop':
        # Preserve all durable state; this command never removes volumes.
        compose('stop')
    elif args.action == 'status':
        compose('ps', '-a')
    if args.action in ('monitor', 'run'):
        try:
            subprocess.run(['python3', str(COMPOSE.with_name('monitor.py')), '--project', 'algotrading-paper',
                            '--output', str(RUN / 'telemetry/host.json'), '--disk-path', str(RUN),
                            '--log-root', str(RUN / 'algotrading/services')], check=True)
        except KeyboardInterrupt:
            pass
        finally:
            if args.action == 'run':
                compose('stop')


if __name__ == '__main__':
    main()
