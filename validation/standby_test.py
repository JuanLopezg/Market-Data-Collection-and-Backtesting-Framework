"""Standby account conversion and venue quantity estimates (no network)."""
import importlib.util
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

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


class KrakenLotStepTest(unittest.TestCase):
    def test_round_to_nearest_step(self):
        for quantity, expected in ((0.00028, 0.0003), (0.000083, 0.0001),
                                   (0.00004, 0.0), (0.00005, 0.0001),
                                   (0.00025, 0.0003), (0.0003, 0.0003)):
            with self.subTest(quantity=quantity):
                self.assertAlmostEqual(standby.round_to_nearest_step(quantity, 0.0001), expected)

    def test_half_step_tolerance_matches_gateway(self):
        step = 0.0001
        self.assertEqual(standby.round_to_nearest_step((0.5 - 0.5e-9) * step, step), step)
        self.assertEqual(standby.round_to_nearest_step((0.5 - 2e-9) * step, step), 0.0)

    def test_public_instruments_precision_including_negative_precision(self):
        with patch.object(standby, 'KrakenPublicReader') as reader:
            reader.return_value.read.return_value = {'result': 'success', 'instruments': [
                {'symbol': 'PF_XBTUSD', 'contractValueTradePrecision': 4},
                {'symbol': 'PF_PEPEUSD', 'contractValueTradePrecision': -3},
                {'symbol': 'PF_BADUSD', 'contractValueTradePrecision': True}]}
            self.assertEqual(standby.kraken_quantity_steps('unused'),
                             {'PF_XBTUSD': 0.0001, 'PF_PEPEUSD': 1000.0})
            reader.assert_called_once_with('live', 'unused')
            reader.return_value.read.assert_called_once_with('instruments')

    def test_annotations_preserve_estimates_and_apply_pepe_scale(self):
        orders = [{'kraken_symbol': 'PF_XBTUSD', 'kraken_quantity': 0.00004, 'status': 'PLANNED'},
                  {'kraken_symbol': 'PF_PEPEUSD', 'kraken_quantity': 1.6 * 1000, 'status': 'PLANNED'},
                  {'kraken_symbol': 'PF_MISSINGUSD', 'kraken_quantity': 1, 'status': 'PLANNED'},
                  {'kraken_symbol': None, 'kraken_quantity': None, 'status': 'SKIPPED: not on Kraken'}]
        with patch.object(standby, 'kraken_quantity_steps', return_value={
                'PF_XBTUSD': 0.0001, 'PF_PEPEUSD': 1000.0}) as fetch:
            standby.add_venue_quantities(orders, 'unused')
            fetch.assert_called_once_with('unused')
        self.assertEqual(orders[0]['venue_quantity'], 0.0)
        self.assertEqual(orders[0]['status'], 'BELOW_MINIMUM_QUANTITY')
        self.assertEqual(orders[0]['kraken_quantity'], 0.00004)
        self.assertEqual(orders[1]['venue_step'], 1000.0)
        self.assertEqual(orders[1]['venue_quantity'], 2000.0)
        self.assertEqual(orders[1]['kraken_quantity'], 1600.0)
        self.assertEqual(orders[1]['status'], 'PLANNED')
        self.assertIsNone(orders[2]['venue_step'])
        self.assertIsNone(orders[2]['venue_quantity'])
        self.assertEqual(orders[2]['status'], 'VENUE_STEP_UNAVAILABLE')
        self.assertEqual(orders[3]['status'], 'SKIPPED: not on Kraken')

    def test_instruments_failure_keeps_plan(self):
        for error in (ValueError('invalid metadata'), OSError('network unavailable')):
            with self.subTest(error=error), patch.object(standby, 'kraken_quantity_steps', side_effect=error):
                orders = [{'kraken_symbol': 'PF_XBTUSD', 'kraken_quantity': 0.00028, 'status': 'PLANNED'}]
                standby.add_venue_quantities(orders, 'unused')
                self.assertIsNone(orders[0]['venue_step'])
                self.assertIsNone(orders[0]['venue_quantity'])
                self.assertEqual(orders[0]['kraken_quantity'], 0.00028)
                self.assertEqual(orders[0]['status'], 'VENUE_STEP_UNAVAILABLE')


if __name__ == '__main__':
    unittest.main()
