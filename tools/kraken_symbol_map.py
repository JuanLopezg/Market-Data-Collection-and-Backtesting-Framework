#!/usr/bin/env python3
"""Build the Binance -> Kraken Futures symbol map from public instrument and price data.

The strategy names assets by their Binance USDT perpetual (BTCUSDT, 1000PEPEUSDT). Kraken
Multi-M lists PF_<asset>USD perpetuals in underlying units (PF_XBTUSD, PF_PEPEUSD). The map
written here is the single table the exchange gateway, kraken_shadow.py and the dashboard
use; nothing at runtime guesses names or scales.

How an entry is accepted:
  1. Name candidates: the Binance base itself (scale 1) and, only for an explicit
     1000 / 1000000 / 1M prefix followed by a letter, the base without it (scale 1000 or
     1,000,000). Other digits are never stripped (1INCH stays 1INCH). Kraken's XBT is BTC.
  2. The Kraken contract must be a tradeable, non-expired PF_ perpetual quoted in USD with
     contractSize 1.
  3. The scale is confirmed by prices taken at the same moment:
     Binance mark / Kraken mark must be within 5% of the scale. A same-named but different
     token, or a wrong scale, fails here and is reported instead of mapped.

Review the printed rejections and the git diff of the output before committing it.
Public endpoints only; no credentials.

Usage:
    python3 tools/kraken_symbol_map.py --output config/venues/kraken_futures/symbol_map.json
"""

import argparse
import datetime as dt
import json
import sys
import urllib.request
from pathlib import Path

BINANCE_EXCHANGE_INFO_URL = "https://fapi.binance.com/fapi/v1/exchangeInfo"
BINANCE_MARK_PRICES_URL = "https://fapi.binance.com/fapi/v1/premiumIndex"
KRAKEN_INSTRUMENTS_URL = "https://futures.kraken.com/derivatives/api/v3/instruments"
KRAKEN_TICKERS_URL = "https://futures.kraken.com/derivatives/api/v3/tickers"

PRICE_TOLERANCE = 0.05
SCALE_PREFIXES = [("1000000", 1_000_000), ("1000", 1000), ("1M", 1_000_000)]


def get_json(url: str):
    request = urllib.request.Request(url, headers={"User-Agent": "algoTrading-symbol-map"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def binance_perpetuals() -> dict[str, float]:
    """Trading Binance USDT perpetuals on coins -> current mark price."""
    info = get_json(BINANCE_EXCHANGE_INFO_URL)
    marks = {row["symbol"]: float(row["markPrice"]) for row in get_json(BINANCE_MARK_PRICES_URL)}
    result = {}
    for s in info["symbols"]:
        if (s.get("contractType") == "PERPETUAL" and s.get("quoteAsset") == "USDT" and
                s.get("underlyingType", "COIN") == "COIN" and s.get("status") == "TRADING" and
                marks.get(s["symbol"], 0.0) > 0.0):
            result[s["symbol"]] = marks[s["symbol"]]
    return result


def kraken_perpetuals() -> dict[str, dict]:
    """Tradeable Kraken Multi-M perpetuals by asset name (XBT -> BTC)."""
    instruments = get_json(KRAKEN_INSTRUMENTS_URL)
    tickers = {row["symbol"]: row for row in get_json(KRAKEN_TICKERS_URL)["tickers"]}
    result = {}
    for instrument in instruments["instruments"]:
        symbol = instrument.get("symbol", "")
        if (not symbol.startswith("PF_") or instrument.get("tradfi") is not False or
                instrument.get("type") != "flexible_futures" or instrument.get("quote") != "USD"):
            continue
        ticker = tickers.get(symbol)
        if (not instrument.get("tradeable") or instrument.get("isExpired") or ticker is None or
                ticker.get("suspended") or float(ticker.get("markPrice") or 0.0) <= 0.0):
            continue
        asset = instrument["base"].upper()
        asset = "BTC" if asset == "XBT" else asset
        result[asset] = {"symbol": symbol, "contract_size": float(instrument["contractSize"]),
                         "mark": float(ticker["markPrice"])}
    return result


def candidates(base: str) -> list[tuple[str, int]]:
    names = [(base, 1)]
    for prefix, scale in SCALE_PREFIXES:
        if base.startswith(prefix) and len(base) > len(prefix) and base[len(prefix)].isalpha():
            names.append((base[len(prefix):], scale))
            break
    return names


def build(binance: dict[str, float], kraken: dict[str, dict]) -> tuple[list[dict], list[str]]:
    entries, rejected = [], []
    for source, binance_mark in sorted(binance.items()):
        base = source[:-len("USDT")]
        found = False
        problems = []
        for asset, scale in candidates(base):
            market = kraken.get(asset)
            if market is None:
                continue
            if market["contract_size"] != 1.0:
                problems.append(f"{market['symbol']} contractSize {market['contract_size']:g}")
                continue
            ratio = binance_mark / market["mark"]
            if abs(ratio / scale - 1.0) > PRICE_TOLERANCE:
                problems.append(f"{market['symbol']} price ratio {ratio:.6g}, expected {scale}")
                continue
            entries.append({"source": source, "asset": asset, "venue_symbol": market["symbol"],
                            "venue_units_per_source_unit": scale})
            found = True
            break
        if not found and problems:
            rejected.append(f"{source}: " + "; ".join(problems))
    return entries, rejected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True, type=Path, help="symbol map JSON to write")
    args = parser.parse_args()

    binance = binance_perpetuals()
    kraken = kraken_perpetuals()
    entries, rejected = build(binance, kraken)

    venue_symbols = [entry["venue_symbol"] for entry in entries]
    duplicates = sorted({symbol for symbol in venue_symbols if venue_symbols.count(symbol) > 1})
    if duplicates:
        print("Two Binance symbols map to the same Kraken contract: " + ", ".join(duplicates), file=sys.stderr)
        return 1

    document = {
        "schema_version": 1,
        "venue": "kraken-futures",
        "source": "binance-usdt-perpetual",
        "generated_by": "tools/kraken_symbol_map.py",
        "generated_at": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "note": ("Explicit Binance -> Kraken Futures mapping. A source symbol missing here is not "
                 "tradeable on Kraken. venue_units_per_source_unit converts quantities "
                 "(venue = source x scale) and prices (venue = source / scale)."),
        "symbols": entries,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

    scaled = [f"{e['source']} -> {e['venue_symbol']} x{e['venue_units_per_source_unit']}"
              for e in entries if e["venue_units_per_source_unit"] != 1]
    print(f"{len(binance)} Binance perpetuals, {len(kraken)} Kraken perpetuals, {len(entries)} mapped")
    print("Scaled: " + (", ".join(scaled) or "none"))
    for line in rejected:
        print("Not mapped, review: " + line)
    print(f"Written {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
