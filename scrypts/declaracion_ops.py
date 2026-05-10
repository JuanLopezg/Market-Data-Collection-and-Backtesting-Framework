#!/usr/bin/env python3
from __future__ import annotations

import csv
from collections import defaultdict, deque
from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal, ROUND_HALF_UP, getcontext
from pathlib import Path
from typing import Dict, List, Tuple, Optional

getcontext().prec = 28


CSV_PATH = Path("/mnt/c/Users/Juan/Downloads/ops.csv.csv")
TAX_YEAR = 2025

# Output detailed realized gain/loss rows here:
REPORT_PATH = CSV_PATH.with_name(f"kraken_btc_eur_tax_report_{TAX_YEAR}.csv")

BTC_ASSETS = {"BTC", "XBT", "XXBT", "XXBT.F", "BTC.F"}
EUR_ASSETS = {"EUR", "ZEUR", "EUR.F"}
BTC_PAIR_MARKERS = ("BTC", "XBT")
EUR_PAIR_MARKERS = ("EUR", "ZEUR")


def D(value) -> Decimal:
    """Safe Decimal parser."""
    if value is None:
        return Decimal("0")
    s = str(value).strip().replace(",", "")
    if s == "":
        return Decimal("0")
    return Decimal(s)


def money(x: Decimal) -> str:
    return str(x.quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))


def qty(x: Decimal) -> str:
    return str(x.quantize(Decimal("0.00000001"), rounding=ROUND_HALF_UP))


def parse_time(value: str) -> datetime:
    s = value.strip().replace("Z", "+00:00")

    # Kraken exports are usually one of these.
    formats = [
        "%Y-%m-%d %H:%M:%S.%f",
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S.%f%z",
        "%Y-%m-%dT%H:%M:%S%z",
    ]

    for fmt in formats:
        try:
            return datetime.strptime(s, fmt)
        except ValueError:
            pass

    # Last attempt: Python ISO parser.
    return datetime.fromisoformat(s)


def normalize_asset(asset: str) -> str:
    a = asset.strip().upper()
    if a in BTC_ASSETS:
        return "BTC"
    if a in EUR_ASSETS:
        return "EUR"
    return a


def is_btc_eur_pair(pair: str) -> bool:
    p = pair.upper()
    return any(x in p for x in BTC_PAIR_MARKERS) and any(x in p for x in EUR_PAIR_MARKERS)


@dataclass
class Lot:
    date: datetime
    btc_remaining: Decimal
    cost_basis_remaining_eur: Decimal
    source: str


@dataclass
class DisposalRow:
    sale_date: str
    sale_source: str
    btc_sold: Decimal
    proceeds_eur: Decimal
    acquisition_date: str
    acquisition_source: str
    acquisition_basis_eur: Decimal
    gain_loss_eur: Decimal


class FIFOCalculator:
    def __init__(self):
        self.lots = deque()
        self.disposals: List[DisposalRow] = []
        self.warnings: List[str] = []

        self.total_btc_bought = Decimal("0")
        self.total_btc_sold = Decimal("0")

        self.total_buy_cost_eur = Decimal("0")
        self.total_sell_gross_eur = Decimal("0")
        self.total_sell_net_eur = Decimal("0")

        self.total_buy_fees_eur = Decimal("0")
        self.total_sell_fees_eur = Decimal("0")

        self.ignored_rows = 0

    def buy(
        self,
        date: datetime,
        btc_amount: Decimal,
        cost_eur: Decimal,
        fee_eur: Decimal,
        source: str,
    ):
        if btc_amount <= 0:
            self.warnings.append(f"Ignored buy with non-positive BTC amount: {source}")
            return

        basis = cost_eur + fee_eur

        self.lots.append(
            Lot(
                date=date,
                btc_remaining=btc_amount,
                cost_basis_remaining_eur=basis,
                source=source,
            )
        )

        self.total_btc_bought += btc_amount
        self.total_buy_cost_eur += basis
        self.total_buy_fees_eur += fee_eur

    def sell(
        self,
        date: datetime,
        btc_amount: Decimal,
        gross_proceeds_eur: Decimal,
        fee_eur: Decimal,
        source: str,
    ):
        if btc_amount <= 0:
            self.warnings.append(f"Ignored sell with non-positive BTC amount: {source}")
            return

        net_proceeds_eur = gross_proceeds_eur - fee_eur

        self.total_btc_sold += btc_amount
        self.total_sell_gross_eur += gross_proceeds_eur
        self.total_sell_net_eur += net_proceeds_eur
        self.total_sell_fees_eur += fee_eur

        remaining_to_match = btc_amount

        while remaining_to_match > 0:
            if not self.lots:
                self.warnings.append(
                    f"ERROR: Sold {qty(remaining_to_match)} BTC on {date.date()} "
                    f"but no BTC acquisition lot was available. "
                    f"You are missing earlier buys/deposits/acquisition history."
                )
                return

            lot = self.lots[0]
            matched_btc = min(remaining_to_match, lot.btc_remaining)

            sale_fraction = matched_btc / btc_amount
            proceeds_part = net_proceeds_eur * sale_fraction

            lot_fraction = matched_btc / lot.btc_remaining
            basis_part = lot.cost_basis_remaining_eur * lot_fraction

            gain_loss = proceeds_part - basis_part

            self.disposals.append(
                DisposalRow(
                    sale_date=date.isoformat(),
                    sale_source=source,
                    btc_sold=matched_btc,
                    proceeds_eur=proceeds_part,
                    acquisition_date=lot.date.isoformat(),
                    acquisition_source=lot.source,
                    acquisition_basis_eur=basis_part,
                    gain_loss_eur=gain_loss,
                )
            )

            lot.btc_remaining -= matched_btc
            lot.cost_basis_remaining_eur -= basis_part
            remaining_to_match -= matched_btc

            if lot.btc_remaining <= Decimal("0.00000000000001"):
                self.lots.popleft()

    @property
    def total_acquisition_basis_sold_eur(self) -> Decimal:
        return sum((r.acquisition_basis_eur for r in self.disposals), Decimal("0"))

    @property
    def net_gain_loss_eur(self) -> Decimal:
        return sum((r.gain_loss_eur for r in self.disposals), Decimal("0"))

    @property
    def remaining_btc(self) -> Decimal:
        return sum((lot.btc_remaining for lot in self.lots), Decimal("0"))

    @property
    def remaining_basis_eur(self) -> Decimal:
        return sum((lot.cost_basis_remaining_eur for lot in self.lots), Decimal("0"))


def read_csv(path: Path) -> Tuple[List[Dict[str, str]], List[str]]:
    if not path.exists():
        raise FileNotFoundError(f"CSV not found: {path}")

    # utf-8-sig handles possible BOM from Excel/Kraken exports.
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        fields = reader.fieldnames or []

    fields = [x.strip().strip('"') for x in fields]
    return rows, fields


def detect_csv_type(fields: List[str]) -> str:
    f = set(fields)

    trade_required = {
        "txid",
        "ordertxid",
        "pair",
        "time",
        "type",
        "price",
        "cost",
        "fee",
        "vol",
    }

    ledger_required = {
        "txid",
        "refid",
        "time",
        "type",
        "aclass",
        "asset",
        "amount",
        "fee",
        "balance",
    }

    if trade_required.issubset(f):
        return "trades"

    if ledger_required.issubset(f):
        return "ledger"

    raise ValueError(
        "Could not detect CSV type. "
        f"Fields found: {fields}"
    )


def calculate_from_trades(rows: List[Dict[str, str]]) -> FIFOCalculator:
    calc = FIFOCalculator()

    parsed_rows = []
    for row in rows:
        try:
            dt = parse_time(row["time"])
        except Exception:
            calc.warnings.append(f"Could not parse time for row txid={row.get('txid')}")
            continue

        parsed_rows.append((dt, row))

    parsed_rows.sort(key=lambda x: x[0])

    for dt, row in parsed_rows:
        if dt.year != TAX_YEAR:
            calc.ignored_rows += 1
            continue

        pair = row.get("pair", "")
        if not is_btc_eur_pair(pair):
            calc.ignored_rows += 1
            continue

        side = row.get("type", "").strip().lower()
        txid = row.get("txid", "")
        ordertxid = row.get("ordertxid", "")
        source = f"trade txid={txid} ordertxid={ordertxid}"

        btc_vol = abs(D(row.get("vol")))
        eur_cost = abs(D(row.get("cost")))
        eur_fee = abs(D(row.get("fee")))

        # Assumption for BTC/EUR Kraken Trades CSV:
        # cost and fee are treated as EUR amounts.
        if side == "buy":
            calc.buy(
                date=dt,
                btc_amount=btc_vol,
                cost_eur=eur_cost,
                fee_eur=eur_fee,
                source=source,
            )
        elif side == "sell":
            calc.sell(
                date=dt,
                btc_amount=btc_vol,
                gross_proceeds_eur=eur_cost,
                fee_eur=eur_fee,
                source=source,
            )
        else:
            calc.ignored_rows += 1

    return calc


def calculate_from_ledger(rows: List[Dict[str, str]]) -> FIFOCalculator:
    calc = FIFOCalculator()

    # Group trade ledger rows by refid.
    groups = defaultdict(list)

    for row in rows:
        typ = row.get("type", "").strip().lower()
        if typ != "trade":
            # EUR deposits/withdrawals are not gains/losses by themselves.
            calc.ignored_rows += 1
            continue

        refid = row.get("refid", "").strip()
        if not refid:
            calc.warnings.append(f"Trade ledger row without refid: txid={row.get('txid')}")
            continue

        groups[refid].append(row)

    parsed_groups = []

    for refid, group_rows in groups.items():
        try:
            dt = min(parse_time(r["time"]) for r in group_rows)
        except Exception:
            calc.warnings.append(f"Could not parse time for trade refid={refid}")
            continue

        parsed_groups.append((dt, refid, group_rows))

    parsed_groups.sort(key=lambda x: x[0])

    for dt, refid, group_rows in parsed_groups:
        if dt.year != TAX_YEAR:
            calc.ignored_rows += len(group_rows)
            continue

        btc_amount = Decimal("0")
        btc_fee = Decimal("0")
        eur_amount = Decimal("0")
        eur_fee = Decimal("0")

        unknown_assets = []

        for row in group_rows:
            asset = normalize_asset(row.get("asset", ""))
            amount = D(row.get("amount"))
            fee = abs(D(row.get("fee")))

            if asset == "BTC":
                btc_amount += amount
                btc_fee += fee
            elif asset == "EUR":
                eur_amount += amount
                eur_fee += fee
            else:
                unknown_assets.append(asset)

        if unknown_assets:
            calc.warnings.append(
                f"Trade refid={refid} has non BTC/EUR assets: {sorted(set(unknown_assets))}"
            )
            calc.ignored_rows += len(group_rows)
            continue

        source = f"ledger refid={refid}"

        # Buy BTC with EUR:
        # BTC amount positive, EUR amount negative.
        if btc_amount > 0 and eur_amount < 0:
            btc_received = btc_amount - btc_fee
            eur_spent = abs(eur_amount)

            if btc_fee > 0:
                calc.warnings.append(
                    f"Trade refid={refid} has BTC-denominated fee {qty(btc_fee)} BTC. "
                    f"The script reduced acquired BTC by that fee. Check manually."
                )

            calc.buy(
                date=dt,
                btc_amount=btc_received,
                cost_eur=eur_spent,
                fee_eur=eur_fee,
                source=source,
            )

        # Sell BTC for EUR:
        # BTC amount negative, EUR amount positive.
        elif btc_amount < 0 and eur_amount > 0:
            btc_sold = abs(btc_amount)
            eur_received_gross = eur_amount

            if btc_fee > 0:
                calc.warnings.append(
                    f"Trade refid={refid} has BTC-denominated fee {qty(btc_fee)} BTC. "
                    f"This may need manual EUR valuation for exact tax treatment."
                )

            calc.sell(
                date=dt,
                btc_amount=btc_sold,
                gross_proceeds_eur=eur_received_gross,
                fee_eur=eur_fee,
                source=source,
            )

        else:
            calc.warnings.append(
                f"Could not classify ledger trade refid={refid}: "
                f"BTC amount={btc_amount}, EUR amount={eur_amount}"
            )
            calc.ignored_rows += len(group_rows)

    return calc


def write_report(calc: FIFOCalculator, path: Path):
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "sale_date",
            "sale_source",
            "btc_sold",
            "proceeds_eur_net_of_sell_fee",
            "acquisition_date",
            "acquisition_source",
            "acquisition_basis_eur_fifo",
            "gain_loss_eur",
        ])

        for r in calc.disposals:
            writer.writerow([
                r.sale_date,
                r.sale_source,
                qty(r.btc_sold),
                money(r.proceeds_eur),
                r.acquisition_date,
                r.acquisition_source,
                money(r.acquisition_basis_eur),
                money(r.gain_loss_eur),
            ])


def main():
    rows, fields = read_csv(CSV_PATH)
    csv_type = detect_csv_type(fields)

    if csv_type == "trades":
        calc = calculate_from_trades(rows)
    elif csv_type == "ledger":
        calc = calculate_from_ledger(rows)
    else:
        raise RuntimeError(f"Unsupported CSV type: {csv_type}")

    write_report(calc, REPORT_PATH)

    print()
    print("=" * 72)
    print(f"KRAKEN BTC/EUR TAX SUMMARY — {TAX_YEAR}")
    print("=" * 72)
    print(f"CSV path:        {CSV_PATH}")
    print(f"CSV detected as: {csv_type}")
    print(f"Detailed report: {REPORT_PATH}")
    print()

    print("BTC MOVEMENT")
    print("-" * 72)
    print(f"BTC bought:                    {qty(calc.total_btc_bought)} BTC")
    print(f"BTC sold:                      {qty(calc.total_btc_sold)} BTC")
    print(f"BTC remaining after FIFO:       {qty(calc.remaining_btc)} BTC")
    print()

    print("EUR TOTALS")
    print("-" * 72)
    print(f"Total acquisition value sold:   {money(calc.total_acquisition_basis_sold_eur)} EUR")
    print(f"Total sale value, gross:        {money(calc.total_sell_gross_eur)} EUR")
    print(f"Total sale fees:                {money(calc.total_sell_fees_eur)} EUR")
    print(f"Total sale value, net of fees:  {money(calc.total_sell_net_eur)} EUR")
    print(f"Total buy fees included basis:  {money(calc.total_buy_fees_eur)} EUR")
    print()

    print("NUMBER TO DECLARE")
    print("-" * 72)
    print(f"NET CAPITAL GAIN / LOSS:        {money(calc.net_gain_loss_eur)} EUR")
    print()

    if calc.net_gain_loss_eur >= 0:
        print(f"Result: gain of {money(calc.net_gain_loss_eur)} EUR")
    else:
        print(f"Result: loss of {money(abs(calc.net_gain_loss_eur))} EUR")

    print()
    print("NOTES")
    print("-" * 72)
    print("This calculates the gain/loss figure, not the final tax owed.")
    print("Deposits and withdrawals are ignored unless they are part of BTC/EUR trades.")
    print("For Spain, this uses FIFO matching for BTC sold.")
    print()

    if calc.remaining_btc != 0:
        print("WARNING")
        print("-" * 72)
        print(
            f"The script still shows {qty(calc.remaining_btc)} BTC remaining "
            f"with basis {money(calc.remaining_basis_eur)} EUR."
        )
        print("If you really sold all BTC, check whether some trade rows are missing.")
        print()

    if calc.warnings:
        print("WARNINGS / THINGS TO CHECK")
        print("-" * 72)
        for w in calc.warnings:
            print(f"- {w}")
        print()

    print(f"Ignored rows: {calc.ignored_rows}")
    print("=" * 72)
    print()


if __name__ == "__main__":
    main()