#!/usr/bin/env python3
"""Download Binance public archives from data.binance.vision.

No API key, no authentication, no rate-limited REST endpoints. The script
downloads the .zip plus its .CHECKSUM, verifies SHA-256, and extracts the CSV.

Examples
--------
# USD-M futures bookDepth (all snapshots for January 2023, one zip per day)
python3 tools/download_binance.py --market futures --kind bookDepth \
    --symbol BTCUSDT --start 2023-01-01 --end 2023-01-31 --out data/binance

# Spot monthly aggTrades
python3 tools/download_binance.py --market spot --kind aggTrades \
    --symbol BTCUSDT --start 2024-01 --end 2024-03 --monthly --out data/binance
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import pathlib
import sys
import urllib.request
import zipfile

BASE = "https://data.binance.vision/data"


def month_range(start: str, end: str) -> list[str]:
    start_date = dt.date.fromisoformat(start + "-01") if len(start) == 7 else dt.date.fromisoformat(start)
    end_date = dt.date.fromisoformat(end + "-01") if len(end) == 7 else dt.date.fromisoformat(end)
    months = []
    year, month = start_date.year, start_date.month
    while (year, month) <= (end_date.year, end_date.month):
        months.append(f"{year:04d}-{month:02d}")
        month += 1
        if month == 13:
            year += 1
            month = 1
    return months


def day_range(start: str, end: str) -> list[str]:
    first = dt.date.fromisoformat(start)
    last = dt.date.fromisoformat(end)
    return [(first + dt.timedelta(days=offset)).isoformat()
            for offset in range((last - first).days + 1)]


def archive_url(market: str, kind: str, symbol: str, period: str, monthly: bool) -> str:
    if market == "futures":
        prefix = "futures/um"
    else:
        prefix = "spot"
    folder = "monthly" if monthly else "daily"
    return f"{BASE}/{prefix}/{folder}/{kind}/{symbol}/{symbol}-{kind}-{period}.zip"


def download(url: str, destination: pathlib.Path) -> pathlib.Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        return destination
    request = urllib.request.Request(url, headers={"User-Agent": "hft-public-data/1.0"})
    with urllib.request.urlopen(request, timeout=120) as response, \
            open(destination, "wb") as output:
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            output.write(chunk)
    return destination


def verify(archive: pathlib.Path, url: str) -> None:
    checksum_url = url + ".CHECKSUM"
    try:
        with urllib.request.urlopen(urllib.request.Request(
                checksum_url, headers={"User-Agent": "hft-public-data/1.0"}),
                timeout=30) as response:
            expected = response.read().decode().split()[0].strip()
    except Exception:
        print(f"warning: no checksum for {archive.name}", file=sys.stderr)
        return
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest.lower() != expected.lower():
        raise RuntimeError(f"checksum mismatch for {archive}: {digest} != {expected}")


def fetch_one(url: str, out_dir: pathlib.Path, keep_zip: bool) -> pathlib.Path:
    name = url.rsplit("/", 1)[-1]
    archive = out_dir / "zips" / name
    download(url, archive)
    verify(archive, url)
    with zipfile.ZipFile(archive) as zipped:
        zipped.extractall(out_dir)
    extracted = out_dir / name[:-4]
    if not keep_zip:
        archive.unlink(missing_ok=True)
    return extracted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--market", choices=["spot", "futures"], default="futures")
    parser.add_argument("--kind", choices=["trades", "aggTrades", "bookDepth"],
                        default="trades")
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--start", required=True, help="YYYY-MM-DD or YYYY-MM")
    parser.add_argument("--end", required=True, help="YYYY-MM-DD or YYYY-MM")
    parser.add_argument("--out", default="data/binance")
    parser.add_argument("--monthly", action="store_true")
    parser.add_argument("--keep-zip", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()

    if args.monthly:
        periods = month_range(args.start, args.end)
    else:
        periods = day_range(args.start, args.end)
    if args.kind == "bookDepth" and args.market != "futures":
        parser.error("bookDepth is only published for USD-M futures")
    if args.kind == "bookDepth" and args.monthly:
        parser.error("bookDepth archives are daily only; drop --monthly")

    urls = [archive_url(args.market, args.kind, args.symbol, period, args.monthly)
            for period in periods]
    out_dir = pathlib.Path(args.out)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(fetch_one, url, out_dir, args.keep_zip): url
                   for url in urls}
        for future in concurrent.futures.as_completed(futures):
            url = futures[future]
            try:
                print("ok", future.result())
            except Exception as error:  # noqa: BLE001
                failures += 1
                print("failed", url, error, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
