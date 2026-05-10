#!/usr/bin/env python3
from __future__ import annotations

import csv
import os
import sys
from decimal import Decimal, getcontext, ROUND_HALF_UP
from pathlib import Path
from collections import defaultdict

getcontext().prec = 28

BOOK_FILENAME = "book.csv"


def D(value) -> Decimal:
    if value is None:
        return Decimal("0")
    s = str(value).strip().replace(",", "")
    if s == "":
        return Decimal("0")
    return Decimal(s)


def fmt(x: Decimal, places: str = "0.00000001") -> str:
    return str(x.quantize(Decimal(places), rounding=ROUND_HALF_UP))


def fmt_eur(x: Decimal) -> str:
    return str(x.quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))


def normalize_asset(asset: str) -> str:
    a = asset.strip().upper()

    if a in {"EUR", "ZEUR", "EUR.F"}:
        return "EUR"

    if a in {"BTC", "XBT", "XXBT", "BTC.F", "XBT.F", "XXBT.F"}:
        return "BTC"

    return a


def find_downloads_dir() -> Path:
    if os.name == "nt":
        candidates = [
            Path(r"C:\Users\Juan\Downloads"),
            Path.home() / "Downloads",
            Path(r"C:\Users\Juan\OneDrive\Downloads"),
        ]
    else:
        candidates = [
            Path("/mnt/c/Users/Juan/Downloads/")
        ]

    print("\nPython executable:")
    print(sys.executable)

    print("\nCandidate Downloads folders:")
    print("-" * 72)

    for p in candidates:
        print(f"{p}  exists={p.exists()}  is_dir={p.is_dir()}")

    print("-" * 72)

    for p in candidates:
        if p.exists() and p.is_dir():
            return p

    raise FileNotFoundError("Could not find your Downloads folder.")


def find_book_csv(downloads_dir: Path) -> Path:
    direct = downloads_dir / BOOK_FILENAME

    if direct.exists():
        return direct

    print(f"\nCould not find {BOOK_FILENAME} directly in {downloads_dir}")
    print("Searching recursively...")

    matches = list(downloads_dir.rglob(BOOK_FILENAME))

    if matches:
        print(f"Found: {matches[0]}")
        return matches[0]

    print("\nFiles in selected Downloads folder:")
    print("-" * 72)

    files = sorted(p for p in downloads_dir.iterdir() if p.is_file())
    if not files:
        print("(No files found.)")
    else:
        for p in files:
            print(p.name)

    print("-" * 72)

    raise FileNotFoundError(f"Could not find {BOOK_FILENAME} in {downloads_dir}")


def sniff_dialect(path: Path):
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        sample = f.read(4096)

    try:
        return csv.Sniffer().sniff(sample, delimiters=",;")
    except csv.Error:
        return csv.excel


def read_ledger(path: Path):
    dialect = sniff_dialect(path)

    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f, dialect=dialect)
        rows = list(reader)
        fields = [x.strip().strip('"') for x in (reader.fieldnames or [])]

    expected = {
        "txid",
        "refid",
        "time",
        "type",
        "subtype",
        "aclass",
        "subclass",
        "asset",
        "wallet",
        "amount",
        "fee",
        "balance",
    }

    missing = expected - set(fields)

    if missing:
        raise ValueError(
            "book.csv does not look like the Kraken ledger CSV you showed.\n"
            f"Missing columns: {sorted(missing)}\n"
            f"Found columns: {fields}"
        )

    return rows, fields, dialect.delimiter


def main():
    downloads_dir = find_downloads_dir()
    book_path = find_book_csv(downloads_dir)

    rows, fields, delimiter = read_ledger(book_path)

    print()
    print("=" * 72)
    print("KRAKEN LEDGER DEPOSIT / WITHDRAWAL CHECK")
    print("=" * 72)
    print(f"Loaded file: {book_path}")
    print(f"Detected delimiter: {repr(delimiter)}")
    print(f"Rows loaded: {len(rows)}")
    print()

    by_asset = defaultdict(lambda: {
        "deposit_gross": Decimal("0"),
        "deposit_fees": Decimal("0"),
        "deposit_net_after_fees": Decimal("0"),

        "withdrawal_gross": Decimal("0"),
        "withdrawal_fees": Decimal("0"),
        "withdrawal_total_after_fees": Decimal("0"),

        "deposit_rows": 0,
        "withdrawal_rows": 0,
    })

    ignored_types = defaultdict(int)

    for row in rows:
        typ = row.get("type", "").strip().lower()
        asset = normalize_asset(row.get("asset", ""))
        amount = D(row.get("amount"))
        fee = abs(D(row.get("fee")))

        if typ == "deposit":
            # Usually deposits are positive.
            gross = amount

            if gross < 0:
                gross = abs(gross)

            # "After fees" estimate:
            # received into account = gross - fee
            net = gross - fee

            by_asset[asset]["deposit_gross"] += gross
            by_asset[asset]["deposit_fees"] += fee
            by_asset[asset]["deposit_net_after_fees"] += net
            by_asset[asset]["deposit_rows"] += 1

        elif typ == "withdrawal":
            # Usually withdrawals are negative.
            gross = abs(amount)

            # "After fees" estimate:
            # total leaving the account = withdrawn amount + fee
            total_out = gross + fee

            by_asset[asset]["withdrawal_gross"] += gross
            by_asset[asset]["withdrawal_fees"] += fee
            by_asset[asset]["withdrawal_total_after_fees"] += total_out
            by_asset[asset]["withdrawal_rows"] += 1

        else:
            ignored_types[typ or "(blank)"] += 1

    print("SUMMARY BY ASSET")
    print("-" * 72)

    for asset in sorted(by_asset.keys()):
        s = by_asset[asset]

        deposited_net = s["deposit_net_after_fees"]
        withdrawn_after_fees = s["withdrawal_total_after_fees"]

        # Positive means more withdrawn than deposited.
        withdrawn_minus_deposited = withdrawn_after_fees - deposited_net

        # Positive means more deposited than withdrawn.
        deposited_minus_withdrawn = deposited_net - withdrawn_after_fees

        places = "0.01" if asset == "EUR" else "0.00000001"

        print(f"\nAsset: {asset}")
        print(f"  Deposit rows:                     {s['deposit_rows']}")
        print(f"  Withdrawal rows:                  {s['withdrawal_rows']}")
        print(f"  Deposited gross:                  {fmt(s['deposit_gross'], places)} {asset}")
        print(f"  Deposit fees:                     {fmt(s['deposit_fees'], places)} {asset}")
        print(f"  Deposited after fees:             {fmt(deposited_net, places)} {asset}")
        print(f"  Withdrawn gross:                  {fmt(s['withdrawal_gross'], places)} {asset}")
        print(f"  Withdrawal fees:                  {fmt(s['withdrawal_fees'], places)} {asset}")
        print(f"  Withdrawn after fees:             {fmt(withdrawn_after_fees, places)} {asset}")
        print(f"  Withdrawn - deposited after fees: {fmt(withdrawn_minus_deposited, places)} {asset}")
        print(f"  Deposited - withdrawn after fees: {fmt(deposited_minus_withdrawn, places)} {asset}")

    print()
    print("IGNORED LEDGER TYPES")
    print("-" * 72)

    if ignored_types:
        for typ, count in sorted(ignored_types.items()):
            print(f"{typ}: {count}")
    else:
        print("None")

    print()
    print("IMPORTANT")
    print("-" * 72)
    print("This checks external cash/asset flows only: deposits and withdrawals.")
    print("It does not calculate trading profit/loss.")
    print("For tax gain/loss, compare this with the FIFO trade script result.")
    print()
    print("For a simple sanity check:")
    print("  If you deposited EUR, bought BTC, sold all BTC, then withdrew EUR,")
    print("  your approximate profit should be close to:")
    print("  EUR withdrawn after fees - EUR deposited after fees")
    print()
    print("=" * 72)


if __name__ == "__main__":
    main()