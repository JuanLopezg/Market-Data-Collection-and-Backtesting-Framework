"""standby/standby.py: the Kraken account is read as the gateway reads it (no network)."""
import importlib.util
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('standby', ROOT / 'standby/standby.py')
standby = importlib.util.module_from_spec(spec)
spec.loader.exec_module(standby)

SYMBOLS = {
    'BTCUSDT': {'source': 'BTCUSDT', 'venue_symbol': 'PF_XBTUSD', 'venue_units_per_source_unit': 1},
    '1000PEPEUSDT': {'source': '1000PEPEUSDT', 'venue_symbol': 'PF_PEPEUSD', 'venue_units_per_source_unit': 1000},
}


class FakeReader:
    def __init__(self, bundle):
        self.bundle = bundle

    def snapshot(self):
        return self.bundle


class KrakenAccountTest(unittest.TestCase):
    def read(self, bundle):
        fake = types.ModuleType('kraken_account')
        fake.reader_from_credentials = lambda path: FakeReader(bundle)
        sys.modules['kraken_account'] = fake
        try:
            return standby.kraken_account(types.SimpleNamespace(credentials='unused'), SYMBOLS)
        finally:
            del sys.modules['kraken_account']

    def test_cash_positions_and_collateral_follow_the_gateway(self):
        bundle = {'wallets': {'accounts': {'flex': {'currencies': {
                      'usd': {'quantity': '500.0', 'collateral': '500.0'},
                      'xbt': {'quantity': '0.01', 'collateral': '594.0'}}}}},
                  'positions': {'openPositions': [
                      {'symbol': 'PF_XBTUSD', 'side': 'short', 'size': '0.002', 'price': '60000'},
                      {'symbol': 'PF_PEPEUSD', 'side': 'long', 'size': '2000000', 'price': '0.00001'}]}}
        account = self.read(bundle)
        # cash = USD balance - entry value (short -0.002 x 60000 = -120, long 2e6 x 1e-5 = 20)
        self.assertAlmostEqual(account['cash'], 500.0 - (-120.0 + 20.0))
        self.assertAlmostEqual(account['collateral_usd'], 594.0)  # USD itself is not collateral
        self.assertAlmostEqual(account['positions']['BTCUSDT'], -0.002)
        self.assertAlmostEqual(account['positions']['1000PEPEUSDT'], 2000.0)  # 2e6 PEPE = 2000 x 1000PEPE

    def test_unmapped_venue_symbol_keeps_its_name(self):
        bundle = {'wallets': {'accounts': {'flex': {'currencies': {'usd': {'quantity': '10', 'collateral': '10'}}}}},
                  'positions': {'openPositions': [{'symbol': 'PF_NEWUSD', 'side': 'long', 'size': '3', 'price': '2'}]}}
        account = self.read(bundle)
        self.assertEqual(account['positions'], {'PF_NEWUSD': 3.0})
        self.assertAlmostEqual(account['cash'], 4.0)


if __name__ == '__main__':
    unittest.main()
