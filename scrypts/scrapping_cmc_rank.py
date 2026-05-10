import json
import time
from datetime import datetime, timedelta

import requests
from bs4 import BeautifulSoup
from tqdm import tqdm


# =========================
# CONFIG
# =========================
START_DATE = "20200101"
END_DATE = "20260101"
OUTPUT_FILE = "all_symbols.txt"
SLEEP_SECONDS = 1.0
TOP_N = 50


def daterange(start_dt, end_dt):
    current = start_dt
    while current <= end_dt:
        yield current
        current += timedelta(days=1)


def find_first_symbol_list(obj, path="root"):
    """
    Recursively find the first list of dicts that contains 'symbol' fields.
    Returns (path, list) or (None, None).
    """
    if isinstance(obj, list):
        if obj and all(isinstance(x, dict) for x in obj):
            if any("symbol" in x for x in obj):
                return path, obj
        for i, item in enumerate(obj[:20]):
            found_path, found_list = find_first_symbol_list(item, f"{path}[{i}]")
            if found_list is not None:
                return found_path, found_list

    elif isinstance(obj, dict):
        for k, v in obj.items():
            found_path, found_list = find_first_symbol_list(v, f"{path}.{k}")
            if found_list is not None:
                return found_path, found_list

    return None, None


def extract_symbols_from_html(html, top_n=50, debug=False):
    soup = BeautifulSoup(html, "html.parser")
    script = soup.find("script", id="__NEXT_DATA__")

    if not script or not script.string:
        if debug:
            print("[DEBUG] __NEXT_DATA__ missing or empty")
        return []

    try:
        next_data = json.loads(script.string)
    except json.JSONDecodeError as e:
        if debug:
            print(f"[DEBUG] Failed to parse __NEXT_DATA__: {e}")
        return []

    # This is the important fix:
    initial_state_raw = next_data.get("props", {}).get("initialState")

    if debug:
        print(f"[DEBUG] type(props.initialState): {type(initial_state_raw).__name__}")

    if not isinstance(initial_state_raw, str):
        if debug:
            print("[DEBUG] props.initialState is not a string")
        return []

    try:
        initial_state = json.loads(initial_state_raw)
    except json.JSONDecodeError as e:
        if debug:
            print(f"[DEBUG] Failed to parse props.initialState string as JSON: {e}")
            print("[DEBUG] First 500 chars of initialState:")
            print(initial_state_raw[:500])
        return []

    found_path, symbol_list = find_first_symbol_list(initial_state)

    if debug:
        print(f"[DEBUG] symbol list path: {found_path}")
        if symbol_list:
            print("[DEBUG] first 3 items:")
            for item in symbol_list[:3]:
                print(item)

    if not symbol_list:
        return []

    symbols = []
    seen = set()

    for item in symbol_list:
        if not isinstance(item, dict):
            continue

        symbol = item.get("symbol")
        if isinstance(symbol, str) and symbol and symbol not in seen:
            symbols.append(symbol)
            seen.add(symbol)

        if len(symbols) >= top_n:
            break

    return symbols


def fetch_page(session, url, max_retries=5):
    headers = {
        "User-Agent": (
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/124.0.0.0 Safari/537.36"
        )
    }

    delay = 1

    for attempt in range(max_retries):
        try:
            r = session.get(url, headers=headers, timeout=30)

            if r.status_code == 200:
                return r.text

            elif r.status_code == 429:
                print(f"[RETRY] 429 Too Many Requests. Sleeping {delay}s...")
                time.sleep(delay)
                delay *= 2  # exponential backoff

            elif r.status_code >= 500:
                print(f"[RETRY] Server error {r.status_code}. Sleeping {delay}s...")
                time.sleep(delay)
                delay *= 2

            else:
                r.raise_for_status()

        except requests.exceptions.RequestException as e:
            print(f"[RETRY] Request failed: {e}. Sleeping {delay}s...")
            time.sleep(delay)
            delay *= 2

    raise Exception(f"Failed after {max_retries} retries")


def main():
    start_dt = datetime.strptime(START_DATE, "%Y%m%d")
    end_dt = datetime.strptime(END_DATE, "%Y%m%d")

    all_dates = list(daterange(start_dt, end_dt))
    all_symbols = set()
    failed_fetch = []
    failed_parse = []

    session = requests.Session()

    for idx, dt in enumerate(tqdm(all_dates, desc="Scraping", unit="day")):
        date_str = dt.strftime("%Y%m%d")
        url = f"https://coinmarketcap.com/historical/{date_str}/"

        try:
            html = fetch_page(session, url)

            # Only debug the first page so output stays readable
            debug = (idx == 0)

            if debug:
                print(f"\n[DEBUG] {date_str} HTML length: {len(html)}")

            symbols = extract_symbols_from_html(html, top_n=TOP_N, debug=debug)

            if len(symbols) == 0:
                failed_parse.append(date_str)
                tqdm.write(f"[WARN] {date_str}: fetched but found 0 symbols")
            else:
                before = len(all_symbols)
                all_symbols.update(symbols)
                after = len(all_symbols)

                tqdm.write(
                    f"[{date_str}] {', '.join(symbols)}  (+{after - before} new)"
                )

        except Exception as e:
            failed_fetch.append(date_str)
            tqdm.write(f"[ERROR] {date_str} fetch failed: {e}")

        time.sleep(SLEEP_SECONDS)

    sorted_symbols = sorted(all_symbols)

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write(",".join(sorted_symbols))

    print("\nDone.")
    print(f"Unique symbols found: {len(sorted_symbols)}")
    print(f"Saved to: {OUTPUT_FILE}")

    if failed_fetch:
        print(f"\nFetch failures: {len(failed_fetch)}")
        print(failed_fetch)

    if failed_parse:
        print(f"\nParse failures (0 symbols): {len(failed_parse)}")
        print(failed_parse)


if __name__ == "__main__":
    main()