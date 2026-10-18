#!/usr/bin/env python3
"""Verify the dashboard's actual fill SQL in an isolated, network-free PostgreSQL."""

import json
import math
from pathlib import Path
import re
import subprocess
import time
import uuid


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "dashboard/dashboard-api/internal/integration/postgres/reader.go").read_text()

    def query_constant(name):
        match = re.search(r"const " + name + r" = `([\s\S]*?)`", source)
        assert match, f"Missing SQL constant: {name}"
        return match.group(1)

    container = "execution-fees-test-" + uuid.uuid4().hex[:10]

    def sql(statement):
        result = subprocess.run(
            ["docker", "exec", "-i", container, "psql", "-U", "postgres", "-XAtq",
             "-v", "ON_ERROR_STOP=1"], input=statement, text=True,
            capture_output=True, check=True,
        )
        return result.stdout.strip()

    def read(prefix, name, *args):
        query = query_constant(name) % args
        return json.loads(sql(query_constant(prefix) + query))

    subprocess.run(
        ["docker", "run", "-d", "--pull", "never", "--network", "none", "--name", container,
         "--tmpfs", "/var/lib/postgresql/data", "-e", "POSTGRES_HOST_AUTH_METHOD=trust",
         "postgres:16-alpine"], check=True, capture_output=True,
    )
    try:
        deadline = time.monotonic() + 50
        while True:
            ready = subprocess.run(
                ["docker", "exec", container, "pg_isready", "-U", "postgres"],
                capture_output=True,
            )
            server = subprocess.run(
                ["docker", "exec", container, "head", "-n", "1",
                 "/var/lib/postgresql/data/postmaster.pid"], capture_output=True, text=True,
            )
            # Wait for the final PID 1 server, not the temporary initialization server.
            if ready.returncode == 0 and server.stdout.strip() == "1":
                try:
                    sql("SELECT 1;")
                    break
                except subprocess.CalledProcessError:
                    pass
            assert time.monotonic() < deadline, "PostgreSQL did not become ready"
            time.sleep(0.3)

        sql("""
CREATE TABLE trading_fills (
  fill_id bigint PRIMARY KEY, order_id bigint, strategy_id bigint, timestamp bigint,
  coin text, side int, quantity double precision, price double precision, commission double precision
);
INSERT INTO trading_fills VALUES
  (1,7,1,20261017,'BTC',0,1,100,0),
  (2,8,1,20261017,'ETH',1,2,50,0.1),
  (3,9,1,20261018,'BTC',0,1,100,0),
  (4,10,1,20261018,'ETH',1,1,50,0);
""")
        assert not json.loads(sql(query_constant("venueFillFeesPresentSQL")))["present"]
        legacy = read("legacyFillFeesSQL", "ledgerFillsSQL", 25)
        assert legacy["totalRows"] == 4 and legacy["totalCommission"] == 0.1
        assert all(row["fee_recorded"] for row in legacy["rows"])

        sql("""
CREATE TABLE trading_venue_costs (
  cost_id text PRIMARY KEY, kind text, fill_id bigint, amount double precision
);
""")
        assert json.loads(sql(query_constant("venueFillFeesPresentSQL")))["present"]
        before = read("venueFillFeesSQL", "fillsForOrdersSQL", "order_id IN (7)", 25)
        assert before["rows"][0]["commission"] == 0 and before["pendingFeeRows"] == 3

        sql("""
INSERT INTO trading_venue_costs VALUES
  ('fee-a','FEE',1,-0.003), ('fee-b','FEE',1,-0.002), ('fee-c','FEE',2,-0.02),
  ('zero-fee','FEE',4,0), ('funding','FUNDING',1,-0.9), ('interest','INTEREST',1,-0.4),
  ('orphan','FEE',999,-0.5), ('unlinked','FEE',0,-0.7);
INSERT INTO trading_venue_costs VALUES ('fee-a','FEE',1,-0.003) ON CONFLICT DO NOTHING;
""")
        after = read("venueFillFeesSQL", "fillsForOrdersSQL", "order_id IN (7)", 25)
        assert after["totalRows"] == 4 and after["pendingFeeRows"] == 1
        assert len(after["rows"]) == 1 and after["rows"][0]["fee_recorded"]
        assert math.isclose(after["rows"][0]["commission"], 0.005)
        assert math.isclose(after["totalCommission"], 0.125)
        assert after == read("venueFillFeesSQL", "fillsForOrdersSQL", "order_id IN (7)", 25)

        ledger = read("venueFillFeesSQL", "ledgerFillsSQL", 1)
        assert ledger["totalRows"] == ledger["distinctFillIds"] == 4
        assert ledger["invalidRows"] == 0 and len(ledger["rows"]) == 1
        assert ledger["rows"][0]["fill_id"] == 4 and ledger["rows"][0]["fee_recorded"]
        assert math.isclose(ledger["totalCommission"], 0.125)
        assert ledger["grossBuyNotional"] == 200 and ledger["grossSellNotional"] == 150
        full = read("venueFillFeesSQL", "ledgerFillsSQL", 200000)
        assert len(full["rows"]) == 4 and math.isclose(full["rows"][1]["commission"], 0.12)
        print("PASS: legacy schema; delayed/multiple/zero fees; pending fees; no duplicate fills; "
              "funding/interest/orphan exclusion; global totals; bounded rows; repeatable reads")
    finally:
        subprocess.run(["docker", "rm", "-f", container], check=True, capture_output=True)


if __name__ == "__main__":
    main()
