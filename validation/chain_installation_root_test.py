"""Verify custom-root host paths without starting or stopping trading services."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallationRootTest(unittest.TestCase):
    def test_install_renders_host_paths_and_preserves_existing_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'algoTrading'
            root.mkdir()
            for name in ('chain', 'config', 'secrets', 'data'):
                (root / name).mkdir()
            (root / 'chain/chain.env').write_text('MODE=paper\nLOG_MODE=local\n')
            (root / 'secrets/chain.env').write_text('TEST_ONLY=preserve\n')
            (root / 'config/market_data.json').write_text('{}')
            (root / 'data/owned-state').write_text('preserve')
            subprocess.run(['bash', str(ROOT / 'deploy/chain/algo'), 'install', str(root)],
                           check=True, capture_output=True, text=True)
            chain_unit = (root / 'chain/systemd/algotrading-chain.service').read_text()
            self.assertIn(f'WorkingDirectory={root}/chain', chain_unit)
            self.assertIn(f'ExecStart={root}/chain/algo up', chain_unit)
            monitor = (root / 'chain/systemd/algotrading-monitor.service').read_text()
            self.assertIn(f'--output {root}/data/telemetry/host.json', monitor)
            self.assertIn(f'ReadWritePaths={root}/data/telemetry', monitor)
            comparison = (root / 'chain/systemd/algotrading-comparison.service').read_text()
            self.assertIn(f'EnvironmentFile={root}/chain/chain.env', comparison)
            kraken = (root / 'chain/systemd/algotrading-kraken.service').read_text()
            self.assertIn(f'ConditionPathExists={root}/secrets/kraken_readonly.env', kraken)
            receiver = (root / 'chain/logging/rsyslog.conf').read_text()
            self.assertIn(f'workDirectory="{root}/data/receiver"', receiver)
            self.assertIn(f'{root}/chain/logging/clean_daily_logs.py', receiver)
            self.assertIn(f'--root {root}/logs', receiver)
            apparmor = (root / 'chain/logging/rsyslog.apparmor').read_text()
            self.assertIn(f'{root}/logs/** rwk', apparmor)
            self.assertIn('owner /proc/[0-9]*/fd/ r,', apparmor)
            self.assertIn('/run/algotrading-logs/', apparmor)
            self.assertIn('/usr/bin/python3', apparmor)
            for folder in ('systemd', 'logging'):
                for path in (root / 'chain' / folder).iterdir():
                    if path.suffix in ('.service', '.timer', '.conf', '.apparmor'):
                        self.assertNotRegex(path.read_text(), r'(?<![A-Za-z0-9_])/(chain|data|config|secrets|logs)(?=/|\b)')
            compose = (root / 'chain/compose.yml').read_text()
            self.assertIn(':/var/lib/postgresql/data', compose)
            self.assertIn(':/data/market', compose)
            self.assertEqual((root / 'chain/chain.env').read_text(), 'MODE=paper\nLOG_MODE=local\n')
            self.assertEqual((root / 'secrets/chain.env').read_text(), 'TEST_ONLY=preserve\n')
            self.assertEqual((root / 'config/market_data.json').read_text(), '{}')
            self.assertEqual((root / 'data/owned-state').read_text(), 'preserve')
            # Reinstallation starts from repository templates, never prefixes twice.
            subprocess.run(['bash', str(ROOT / 'deploy/chain/algo'), 'install', str(root)],
                           check=True, capture_output=True)
            self.assertEqual((root / 'chain/logging/rsyslog.conf').read_text(), receiver)
            # A restrictive administrator umask must not block the syslog user.
            subprocess.run(['bash', '-c', 'umask 077; bash "$1" install "$2"',
                            'root-test', str(ROOT / 'deploy/chain/algo'), str(root)],
                           check=True, capture_output=True)
            self.assertEqual(root.stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / 'chain/logging/rsyslog.conf').stat().st_mode & 0o777, 0o644)
            self.assertEqual((root / 'secrets').stat().st_mode & 0o777, 0o700)


if __name__ == '__main__':
    unittest.main()
