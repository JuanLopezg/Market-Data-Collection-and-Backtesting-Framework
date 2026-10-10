#!/usr/bin/env python3
"""Read-only observation of CURRENT notional plans against a local venue fixture.

This preparation adapter accepts loopback HTTP only. It has no credentials,
submit/cancel interface, fill simulator or trading-state writer. A real venue
reader and aligned backtest comparison require separate acceptance.
"""
from datetime import datetime, timezone
from decimal import Decimal, ROUND_DOWN
import hashlib
import html
import ipaddress
import json
from pathlib import Path
import re
import sqlite3
import urllib.error
import urllib.parse
import urllib.request


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def number(value, positive=False):
    if isinstance(value, bool):
        raise ValueError('Boolean is not an economic number')
    result = Decimal(str(value))
    if not result.is_finite() or (positive and result <= 0):
        raise ValueError('Invalid economic number')
    return result


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        raise ValueError('Venue redirects are forbidden')


class FixtureVenueReader:
    """One bounded GET route; proxy settings and redirects cannot widen access."""

    def __init__(self, base_url):
        url = urllib.parse.urlsplit(base_url)
        if (url.scheme != 'http' or url.username or url.password or url.query or url.fragment
                or url.path not in ('', '/') or not url.hostname
                or not ipaddress.ip_address(url.hostname).is_loopback):
            raise ValueError('Use a numeric loopback HTTP fixture address only')
        self.url = base_url.rstrip('/') + '/snapshot'
        self.client = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def snapshot(self):
        try:
            with self.client.open(self.url, timeout=2) as response:
                body = response.read(1024 * 1024 + 1)
                if len(body) > 1024 * 1024:
                    raise ValueError('Venue fixture response exceeds its bound')
                return json.loads(body)
        except (OSError, ValueError) as error:
            # Do not copy remote response/error text into logs or reports.
            raise ValueError('Venue fixture unavailable or invalid') from error


def validate_plan(plan, now):
    metadata = plan['metadata']
    day = plan['decision_timestamp']
    if metadata['schema_version'] != 1 or not metadata['message_id']:
        raise ValueError('Invalid CURRENT plan metadata')
    if datetime.strptime(str(day), '%Y%m%d').date() >= datetime.fromtimestamp(now, timezone.utc).date():
        raise ValueError('Plan must use a completed UTC day')
    if plan['reference_closes']['date'] != day or plan['decisions']['decision_timestamp'] != day:
        raise ValueError('Plan/reference/decision dates disagree')
    seen = set()
    for order in plan['submit_orders']:
        identity = order['economic_order_id']
        if not identity or identity in seen:
            raise ValueError('Missing or duplicate economic order identity')
        seen.add(identity)
        if order['decision_timestamp'] != day or order['state_revision'] != plan['state_revision']:
            raise ValueError('Order does not belong to this plan')
        close = number(order['reference_close'], positive=True)
        size = number(order['notional_usd'], positive=True)
        delta = number(order['delta_notional_usd'])
        if order['side'] not in (0, 1) or (delta > 0) != (order['side'] == 0):
            raise ValueError('Order side and signed delta disagree')
        if abs(abs(delta) - size) > Decimal('1e-9') * max(Decimal(1), size):
            raise ValueError('Order delta and absolute notional disagree')
        if number(plan['reference_closes']['closes'][order['coin']]) != close:
            raise ValueError('Order changed its completed reference close')


def assess(plan, snapshot, now):
    # Missing metadata is not evidence that a market is unsupported. Only a
    # complete, fresh snapshot can produce a terminal skip/observation.
    if not isinstance(snapshot, dict) or snapshot.get('complete') is not True or snapshot.get('venue') != 'LOCAL_FIXTURE':
        raise ValueError('Incomplete or wrong-source venue snapshot')
    age = number(now) - number(snapshot['observed_at'])
    if age < -5 or age > 30:
        raise ValueError('Venue snapshot is stale or ahead of the clock')
    markets = snapshot['markets']
    if not isinstance(markets, dict):
        raise ValueError('Invalid venue market catalogue')
    observations = []
    for order in plan['submit_orders']:
        row = {'economic_order_id': order['economic_order_id'], 'coin': order['coin'],
               'side': 'BUY' if order['side'] == 0 else 'SELL', 'submitted': False,
               'notional_usd': order['notional_usd'], 'reference_close': order['reference_close'],
               'state': 'SKIPPED', 'reason': 'PERPETUAL_UNAVAILABLE'}
        market = markets.get(order['coin'])
        if market is not None:
            if market['perpetual'] is not True or market['trading'] is not True:
                row['reason'] = 'PERPETUAL_NOT_TRADING'
            else:
                quantity = number(order['notional_usd'], True) / number(order['reference_close'], True)
                step = number(market['quantity_step'], True)
                minimum = number(market['minimum_quantity'], True)
                mark = number(market['mark_price'], True)
                rounded = (quantity / step).to_integral_value(rounding=ROUND_DOWN) * step
                row.update(raw_quantity=str(quantity), fixture_rounded_quantity=str(rounded),
                           observed_mark=str(mark), quote_currency=market['quote_currency'])
                # Rounding is an observation only. Never alter the planner payload
                # or interpret a public price as an execution/fill confirmation.
                row['state'] = 'OBSERVED' if rounded >= minimum else 'SKIPPED'
                row['reason'] = 'READ_ONLY_PROPOSAL' if rounded >= minimum else 'BELOW_MINIMUM_QUANTITY'
        observations.append(row)
    return {'state': 'OBSERVED' if observations or plan['cancel_order_ids'] else 'NO_ORDERS',
            'orders': observations, 'cancel_order_ids_not_sent': plan['cancel_order_ids'],
            'venue_observed_at': snapshot['observed_at'], 'submitted': False,
            'decision_timestamp': plan['decision_timestamp'], 'scope': 'LOCAL_FIXTURE_ONLY',
            'fills': None, 'pnl': None, 'fees': None, 'funding': None,
            'collateral': None, 'quote_to_usd_conversion': None}


class ShadowJournal:
    """Own only observation state; retries cannot mutate trading or duplicate a plan."""

    def __init__(self, directory, configuration_fingerprint, assessor=assess,
                 scope='LOCAL_FIXTURE_ONLY', source_identity=None):
        if not re.fullmatch('[0-9a-f]{64}', configuration_fingerprint):
            raise ValueError('Use the persisted source configuration fingerprint')
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.directory / 'observations.sqlite')
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS configuration(fingerprint TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS plans(id TEXT PRIMARY KEY, hash TEXT NOT NULL,
                day INTEGER NOT NULL, result TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS attempts(id TEXT NOT NULL, hash TEXT NOT NULL,
                observed_at REAL NOT NULL, state TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS observation_source(identity TEXT NOT NULL);
        ''')
        current = self.db.execute('SELECT fingerprint FROM configuration').fetchone()
        if current and current[0] != configuration_fingerprint:
            self.db.close()
            raise ValueError('Shadow state belongs to another source configuration')
        identity = canonical({'scope': scope, 'source': source_identity})
        retained_source = self.db.execute('SELECT identity FROM observation_source').fetchone()
        if retained_source and retained_source[0] != identity:
            self.db.close()
            raise ValueError('Shadow state belongs to another observation source or policy')
        if not retained_source:
            # Existing journals predate venue support and contain fixture results.
            has_observations = self.db.execute('SELECT 1 FROM plans UNION SELECT 1 FROM attempts LIMIT 1').fetchone()
            if has_observations and (scope != 'LOCAL_FIXTURE_ONLY' or source_identity is not None):
                self.db.close()
                raise ValueError('Use separate state for a real venue observer')
            with self.db:
                self.db.execute('INSERT INTO observation_source VALUES (?)', (identity,))
        if not current:
            with self.db:
                self.db.execute('INSERT INTO configuration VALUES (?)', (configuration_fingerprint,))
        self.fingerprint = configuration_fingerprint
        self.assessor = assessor
        self.scope = scope

    def close(self):
        self.db.close()

    def observe(self, plan, reader, now):
        validate_plan(plan, now)
        identity, payload_hash = plan['metadata']['message_id'], digest(plan)
        # Include failed attempts in conflict detection: unavailable transport
        # does not allow a producer to rewrite the same logical plan on retry.
        hashes = self.db.execute('SELECT hash FROM plans WHERE id=? UNION SELECT hash FROM attempts WHERE id=?',
                                 (identity, identity)).fetchall()
        if any(row[0] != payload_hash for row in hashes):
            raise ValueError('Conflicting payload for a retained plan identity')
        existing = self.db.execute('SELECT result FROM plans WHERE id=?', (identity,)).fetchone()
        if existing:
            return json.loads(existing[0])
        latest = self.db.execute('SELECT max(day) FROM plans').fetchone()[0]
        if latest and plan['decision_timestamp'] < latest:
            raise ValueError('Out-of-order plan cannot become a new current observation')
        try:
            result = self.assessor(plan, reader.snapshot(), now)
            if result['scope'] != self.scope or result['submitted'] is not False:
                raise ValueError('Assessor returned the wrong observation scope or submission state')
        except (ValueError, KeyError, TypeError, ArithmeticError):
            with self.db:
                self.db.execute('INSERT INTO attempts VALUES (?,?,?,?)', (identity, payload_hash, now, 'UNAVAILABLE'))
            return {'state': 'UNAVAILABLE', 'orders': [], 'submitted': False, 'scope': self.scope}
        result.update(message_id=identity, plan_hash=payload_hash, configuration_fingerprint=self.fingerprint)
        with self.db:
            self.db.execute('INSERT INTO plans VALUES (?,?,?,?)',
                            (identity, payload_hash, plan['decision_timestamp'], canonical(result)))
        return result

    def export(self, checks, source_message_id):
        if self.scope != 'LOCAL_FIXTURE_ONLY':
            raise ValueError('Fixture reports cannot describe an actual venue observer')
        plans = [json.loads(row[0]) for row in self.db.execute('SELECT result FROM plans ORDER BY day,id')]
        attempts = self.db.execute('SELECT count(*) FROM attempts').fetchone()[0]
        report = {'scope': 'LOCAL_FIXTURE_ONLY', 'configuration_fingerprint': self.fingerprint,
                  'producer_plan_message_id': source_message_id,
                  'submitted': False, 'plans': plans, 'unavailable_attempts': attempts, 'checks': checks,
                  'venue_acceptance': False, 'aligned_backtest_comparison': 'PENDING',
                  'costs_collateral_and_real_fills': 'UNAVAILABLE'}
        rows = ''.join('<tr><td>' + html.escape('Producer plan' if plan['message_id'] == source_message_id
            else 'Synthetic: ' + plan['message_id'].removeprefix(source_message_id + ':')) + '</td>'
            + ''.join('<td>' + html.escape(str(row.get(key, 'Unavailable'))) + '</td>'
            for key in ('coin', 'side', 'state', 'reason', 'notional_usd', 'raw_quantity', 'observed_mark')) + '</tr>'
            for plan in plans for row in plan['orders'])
        checks_html = ''.join('<li>' + html.escape(check) + '</li>' for check in checks)
        page = ('<!doctype html><html lang="en"><meta charset="utf-8"><title>Local shadow rehearsal</title>'
                '<style>body{font:17px system-ui;margin:32px;max-width:1200px}table{border-collapse:collapse}'
                'td,th{border:1px solid #aaa;padding:10px;text-align:left}pre{white-space:pre-wrap}</style>'
                '<h1>Local shadow rehearsal</h1><p>Controlled fixture. No real orders submitted. '
                'No actual venue acceptance or estimated profit. Fees, funding, collateral and FX are unavailable. '
                'Synthetic rows exercise error cases; they are not additional strategy decisions.</p>'
                '<h2>Passed checks</h2><ul>' + checks_html + '</ul><h2>Retained observations</h2><table>'
                '<tr><th>Scenario</th><th>Asset</th><th>Side</th><th>State</th><th>Reason</th><th>USD intent</th>'
                '<th>Raw quantity at close(T)</th><th>Fixture mark</th></tr>' + rows + '</table>'
                '<h2>Evidence</h2><pre>' + html.escape(json.dumps(report, indent=2)) + '</pre></html>')
        for name, contents in (('report.json', json.dumps(report, indent=2) + '\n'), ('report.html', page)):
            temporary = self.directory / (name + '.tmp')
            temporary.write_text(contents)
            temporary.replace(self.directory / name)
        return report
