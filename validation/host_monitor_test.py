"""Host clock and executable observations must stay distinct from trading readiness."""
import importlib.util
from pathlib import Path
import subprocess
import json
import os
import secrets
import time
import unittest
from unittest.mock import patch

source = Path(__file__).resolve().parents[1] / 'deploy/paper_trading/monitor.py'
spec = importlib.util.spec_from_file_location('host_monitor', source)
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


class HostMonitorTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('HOST_MONITOR_DOCKER_TEST') == '1', 'Set HOST_MONITOR_DOCKER_TEST=1 for isolated local Docker acceptance')
    def test_docker_executable_presence_and_stopped_process(self):
        context = json.loads(monitor.command(['docker', 'context', 'inspect']))[0]['Endpoints']['docker']['Host']
        endpoint = os.environ.get('DOCKER_HOST', context) if not os.environ.get('DOCKER_CONTEXT') else context
        self.assertTrue(endpoint.startswith(('unix://', 'npipe://')), 'Only local Docker is allowed')
        project = 'algotrading-paper-' + secrets.token_hex(6)
        name = project + '-process-check'
        try:
            subprocess.run(['docker', 'run', '-d', '--name', name, '--network', 'none',
                            '--label', 'com.docker.compose.project=' + project,
                            '--label', 'com.docker.compose.service=strategy', 'golang:1.23', 'sh', '-c',
                            'cp /usr/bin/sleep /tmp/algotrading_strategy_service; /tmp/algotrading_strategy_service 120 & echo "$!" > /tmp/trading.pid; wait || true; sleep 120'],
                           check=True, stdout=subprocess.DEVNULL)
            time.sleep(.5)
            def state():
                return monitor.containers(project, ['strategy'])[0]['process']['processState']
            self.assertEqual(state(), 'RUNNING')
            for action, expected in [('STOP', 'STOPPED'), ('CONT', 'RUNNING'), ('KILL', 'MISSING')]:
                subprocess.run(['docker', 'exec', name, 'sh', '-c', 'kill -' + action + ' "$(cat /tmp/trading.pid)"'], check=True)
                time.sleep(.1)
                self.assertEqual(state(), expected)
            subprocess.run(['docker', 'stop', '-t', '1', name], check=True, stdout=subprocess.DEVNULL)
            self.assertEqual(state(), 'STOPPED')
            print('Actual local host clock observation: ' + json.dumps(monitor.clock_status()))
        finally:
            subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def test_clock_verified_unsynchronized_and_unavailable(self):
        for value, synced in [('yes', True), ('no', False)]:
            with patch.object(monitor.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, value + '\n')):
                result = monitor.clock_status()
                self.assertTrue(result['clockObserved'])
                self.assertEqual(result['clockSynced'], synced)
                self.assertIn('offset not measured', result['clockOffsetLabel'])
        for value in ['unknown', '']:
            with patch.object(monitor.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, value)):
                self.assertFalse(monitor.clock_status()['clockObserved'])
        for error in [FileNotFoundError(), subprocess.TimeoutExpired('timedatectl', 3)]:
            with patch.object(monitor.subprocess, 'run', side_effect=error):
                self.assertFalse(monitor.clock_status()['clockObserved'])

    def test_container_running_does_not_prove_trading_process(self):
        with patch.object(monitor, 'command', return_value='PID COMMAND STAT\n1 docker-init S\n2 sleep S\n'):
            result = monitor.trading_process('strategy', 'fixture', True)
            self.assertFalse(result['processRunning'])
            self.assertEqual(result['processState'], 'MISSING')
        for state, running in [('Sl', True), ('R', True), ('T', False), ('Z', False)]:
            output = 'PID COMMAND STAT\n1 docker-init S\n2 ' + monitor.TRADING_PROCESSES['strategy'][:15] + ' ' + state + '\n'
            with patch.object(monitor, 'command', return_value=output) as command:
                self.assertEqual(monitor.trading_process('strategy', 'fixture', True)['processRunning'], running)
                self.assertEqual(command.call_args.args[0][-1], 'pid,comm,stat')
        with patch.object(monitor, 'command', side_effect=RuntimeError('Unavailable')):
            result = monitor.trading_process('strategy', 'fixture', True)
            self.assertNotIn('processRunning', result)
            self.assertEqual(result['processState'], 'UNKNOWN')
        with patch.object(monitor, 'command') as command:
            self.assertFalse(monitor.trading_process('strategy', 'fixture', False)['processRunning'])
            command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
