#!/usr/bin/env python3
"""One-command local shadow rehearsal using actual PAPER producer plans.

Default: isolated packaged service study, then read-only fixture observations.
--unit: bounded observer regressions without Docker. No actual venue is contacted.
"""
import argparse
import copy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import tempfile


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('shadow_observer', ROOT / 'live_trading/shadow_observer.py')
observer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observer)


class VenueFixture:
    """Spy all HTTP calls; fixture modes never implement a trading route."""

    def __init__(self):
        self.calls = []
        self.mode = 'healthy'
        self.snapshot = {'venue': 'LOCAL_FIXTURE', 'complete': True, 'observed_at': time.time(),
            'markets': {coin: {'perpetual': True, 'trading': True, 'quantity_step': '0.001',
                              'minimum_quantity': '0.001', 'mark_price': '200', 'quote_currency': 'USDT'}
                        for coin in ('BTCUSDT', 'ETHUSDT')}}
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                fixture.calls.append(('GET', self.path))
                if self.path != '/snapshot':
                    self.send_error(404)
                    return
                if fixture.mode == 'disconnect':
                    self.connection.close()
                    return
                if fixture.mode == 'redirect':
                    self.send_response(302)
                    self.send_header('Location', '/private/orders')
                    self.end_headers()
                    return
                if fixture.mode == 'error':
                    self.send_error(503)
                    return
                data = json.dumps(fixture.snapshot).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                fixture.calls.append((self.command, self.path))
                self.send_error(405)

            do_DELETE = do_POST
            do_PUT = do_POST

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.reader = observer.FixtureVenueReader(f'http://127.0.0.1:{self.server.server_port}')

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)


def example_plan():
    day = int((datetime.now(timezone.utc).date() - timedelta(days=1)).strftime('%Y%m%d'))
    return {'metadata': {'schema_version': 1, 'message_id': 'observer-unit-plan'},
        'decision_timestamp': day, 'state_revision': 1,
        'decisions': {'decision_timestamp': day}, 'reference_closes': {'date': day, 'closes': {'BTCUSDT': 199}},
        'cancel_order_ids': [], 'submit_orders': [{'economic_order_id': 'observer-unit-order',
        'coin': 'BTCUSDT', 'side': 0, 'decision_timestamp': day, 'state_revision': 1,
        'reference_close': 199, 'notional_usd': 10000, 'delta_notional_usd': 10000}]}


def exercise_observer(directory, plan, fingerprint):
    """Use real producer output in integration; synthetic mutations test errors only."""
    fixture = VenueFixture()
    journal = observer.ShadowJournal(directory, fingerprint)
    checks = []
    try:
        result = journal.observe(plan, fixture.reader, time.time())
        assert result['state'] == 'OBSERVED' and all(row['state'] == 'OBSERVED' for row in result['orders'])
        assert all(row['raw_quantity'] == str(observer.number(row['notional_usd']) / observer.number(row['reference_close']))
                   for row in result['orders'])
        checks.append('Actual producer quantities retain close(T); fixture marks are observations, not fills')
        calls = len(fixture.calls)
        assert journal.observe(plan, fixture.reader, time.time()) == result and len(fixture.calls) == calls
        journal.close()
        journal = observer.ShadowJournal(directory, fingerprint)
        assert journal.observe(plan, fixture.reader, time.time()) == result and len(fixture.calls) == calls
        checks.append('Duplicate delivery and observer restart retain one immutable observation without rereading prices')

        def variant(name):
            value = copy.deepcopy(plan)
            value['metadata']['message_id'] += ':' + name
            return value

        # A real completed catalogue says unavailable; a failed/incomplete fetch
        # cannot silently turn the same symbol into an unavailable-market skip.
        unsupported = variant('unsupported')
        removed_coin = plan['submit_orders'][-1]['coin']
        removed = fixture.snapshot['markets'].pop(removed_coin)
        outcome = journal.observe(unsupported, fixture.reader, time.time())
        assert any(row['coin'] == removed_coin and row['reason'] == 'PERPETUAL_UNAVAILABLE' for row in outcome['orders'])
        fixture.snapshot['markets'][removed_coin] = removed
        checks.append('Unavailable perpetual is explicitly skipped without an alternate trade')

        for mode in ('error', 'disconnect', 'redirect'):
            value = variant(mode)
            fixture.mode = mode
            assert journal.observe(value, fixture.reader, time.time())['state'] == 'UNAVAILABLE'
            fixture.mode = 'healthy'
            assert journal.observe(value, fixture.reader, time.time())['state'] == 'OBSERVED'
        checks.append('HTTP failure, disconnect and forbidden redirect remain retryable; recovery observes the original plan')

        for name, changes in (('stale', {'observed_at': time.time() - 60}),
                              ('future-clock', {'observed_at': time.time() + 60}),
                              ('incomplete', {'complete': False}), ('missing', {'markets': None})):
            value = variant(name)
            snapshot = copy.deepcopy(fixture.snapshot)
            fixture.snapshot.update(changes)
            assert journal.observe(value, fixture.reader, time.time())['state'] == 'UNAVAILABLE'
            fixture.snapshot = snapshot
            fixture.snapshot['observed_at'] = time.time()
            assert journal.observe(value, fixture.reader, time.time())['state'] == 'OBSERVED'
        checks.append('Stale, future, incomplete and missing catalogue data are unavailable, not empty success')

        small = variant('minimum-size')
        fixture.snapshot['markets']['BTCUSDT']['minimum_quantity'] = '1000000'
        assert any(row['reason'] == 'BELOW_MINIMUM_QUANTITY' for row in journal.observe(small, fixture.reader, time.time())['orders'])
        fixture.snapshot['markets']['BTCUSDT']['minimum_quantity'] = '0.001'
        quiet = variant('no-orders')
        quiet['submit_orders'] = []
        assert journal.observe(quiet, fixture.reader, time.time())['state'] == 'NO_ORDERS'
        cancel = variant('cancel-intent')
        cancel['submit_orders'] = []
        cancel['cancel_order_ids'] = [123]
        assert journal.observe(cancel, fixture.reader, time.time())['cancel_order_ids_not_sent'] == [123]
        checks.append('Minimum-size, no-order and cancel-intent cycles are explicit; no cancels are sent')

        conflict = copy.deepcopy(plan)
        conflict['cancel_order_ids'] = [321]
        try:
            journal.observe(conflict, fixture.reader, time.time())
        except ValueError:
            pass
        else:
            raise AssertionError('Changed plan identity was accepted')
        try:
            observer.ShadowJournal(directory, 'f' * 64 if fingerprint != 'f' * 64 else 'a' * 64)
        except ValueError:
            pass
        else:
            raise AssertionError('Changed source configuration was accepted')
        checks.append('Conflicting retained plan and source configuration are rejected')
        assert fixture.calls and all(call == ('GET', '/snapshot') for call in fixture.calls)
        checks.append('HTTP spy observed only GET /snapshot; zero submit/cancel/private writes')
        (Path(directory) / 'venue-calls.json').write_text(json.dumps(fixture.calls, indent=2))
        return journal.export(checks, plan['metadata']['message_id'])
    finally:
        journal.close()
        fixture.close()


def run_shadow_study(directory, sql, risk, before, after):
    rows = json.loads(sql("SELECT json_agg(json_build_object('request',request_payload::json,'plan',plan_payload::json)) "
                         "FROM order_planner_live_notional_checkpoint"))
    assert len(rows) == 1 and len(rows[0]['plan']['submit_orders']) == 2
    plan = rows[0]['plan']
    assert {order['coin'] for order in plan['submit_orders']} == {'BTCUSDT', 'ETHUSDT'}
    assert all(order['side'] == 0 and order['reference_close'] == 199 and order['notional_usd'] == 10000
               for order in plan['submit_orders'])
    assert before['fills'] == after['fills'] and before['positions'] == after['positions']
    output = directory / 'shadow'
    output.mkdir()
    (output / 'producer-input.json').write_text(json.dumps(rows, indent=2))
    report = exercise_observer(output, plan, risk['configurationFingerprint'])
    # The observer only reads the exported plan; confirm original durable trading
    # plan bytes remain unchanged after every negative/unsupported mutation.
    current = json.loads(sql("SELECT plan_payload FROM order_planner_live_notional_checkpoint"))
    assert current == plan
    report['checks'].append('Actual PAPER warmup/plan, two simulated fills and worker restart verified; source plan unchanged')
    # Re-export so both offline report formats contain the integration acceptance.
    journal = observer.ShadowJournal(output, risk['configurationFingerprint'])
    try:
        report = journal.export(report['checks'], plan['metadata']['message_id'])
    finally:
        journal.close()
    (output / 'accepted.json').write_text(json.dumps(report, indent=2) + '\n')
    print('SHADOW-REHEARSAL: PASS: local producer plans, read-only fixture, skips, faults and durable observations', flush=True)
    print('Shadow report:', output / 'report.html', flush=True)


class ObserverRegressionTest(unittest.TestCase):
    def test_bounded_fixture_rehearsal(self):
        with tempfile.TemporaryDirectory() as directory:
            report = exercise_observer(directory, example_plan(), 'a' * 64)
            self.assertFalse(report['submitted'])
            self.assertGreater(report['unavailable_attempts'], 0)

    def test_public_addresses_credentials_and_paths_are_rejected(self):
        for url in ('http://15.237.145.179:8092', 'http://localhost:8000',
                    'http://user:password@127.0.0.1:8000', 'http://127.0.0.1:8000/private'):
            with self.assertRaises(ValueError):
                observer.FixtureVenueReader(url)

    def test_future_plan_and_side_mismatch_rejected_before_http(self):
        plan = example_plan()
        plan['decision_timestamp'] = int(datetime.now(timezone.utc).strftime('%Y%m%d'))
        with self.assertRaises(ValueError):
            observer.validate_plan(plan, time.time())
        plan = example_plan()
        plan['submit_orders'][0]['side'] = 1
        with self.assertRaises(ValueError):
            observer.validate_plan(plan, time.time())

    def test_invalid_snapshot_and_failed_identity_cannot_become_success(self):
        fixture = VenueFixture()
        try:
            with tempfile.TemporaryDirectory() as directory:
                journal = observer.ShadowJournal(directory, 'a' * 64)
                try:
                    plan = example_plan()
                    fixture.snapshot = []
                    self.assertEqual(journal.observe(plan, fixture.reader, time.time())['state'], 'UNAVAILABLE')
                    changed = copy.deepcopy(plan)
                    changed['cancel_order_ids'] = [123]
                    with self.assertRaises(ValueError):
                        journal.observe(changed, fixture.reader, time.time())
                    self.assertEqual(journal.db.execute('SELECT count(*) FROM plans').fetchone()[0], 0)
                finally:
                    journal.close()
        finally:
            fixture.close()

    def test_older_cycle_cannot_replace_current_observation(self):
        fixture = VenueFixture()
        try:
            with tempfile.TemporaryDirectory() as directory:
                journal = observer.ShadowJournal(directory, 'a' * 64)
                try:
                    plan = example_plan()
                    self.assertEqual(journal.observe(plan, fixture.reader, time.time())['state'], 'OBSERVED')
                    old = copy.deepcopy(plan)
                    day = int((datetime.now(timezone.utc).date() - timedelta(days=2)).strftime('%Y%m%d'))
                    old['metadata']['message_id'] += ':older'
                    old['decision_timestamp'] = day
                    old['decisions']['decision_timestamp'] = day
                    old['reference_closes']['date'] = day
                    old['submit_orders'][0]['decision_timestamp'] = day
                    calls = len(fixture.calls)
                    with self.assertRaises(ValueError):
                        journal.observe(old, fixture.reader, time.time())
                    self.assertEqual(len(fixture.calls), calls)
                finally:
                    journal.close()
        finally:
            fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unit', action='store_true')
    parser.add_argument('--runtime-image', default=os.environ.get('PAPER_RUNTIME_IMAGE', 'algotrading-runtime:risk-evaluations'))
    parser.add_argument('--api-image', default=os.environ.get('PAPER_API_IMAGE', 'algotrading-telegram-api'))
    parser.add_argument('--web-image', default=os.environ.get('PAPER_WEB_IMAGE', 'algotrading-browser-web'))
    args = parser.parse_args()
    if args.unit:
        result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(ObserverRegressionTest))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    import paper_trading_test
    os.environ.update(PAPER_SHADOW_TEST='1', PAPER_RECOVERY_TEST='0', PAPER_BROWSER_HOLD_SECONDS='0',
                      PAPER_RUNTIME_IMAGE=args.runtime_image, PAPER_API_IMAGE=args.api_image, PAPER_WEB_IMAGE=args.web_image)
    if not os.environ.get('PAPER_FIXTURE_BINARY'):
        for source in sorted((ROOT / 'storage/paper_validation').glob('*/fixture.go'), reverse=True):
            if source.read_text() == paper_trading_test.FIXTURE and source.with_name('server').is_file():
                os.environ['PAPER_FIXTURE_BINARY'] = str(source.with_name('server'))
                break
    paper_trading_test.main()


if __name__ == '__main__':
    main()
