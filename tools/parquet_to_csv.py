#!/usr/bin/env python3
"""Convert crypto-lob-stream Parquet shards to CSV for the C++ readers.

This is the zero-Arrow build path: pyarrow reads Snappy Parquet, the C++ tools
consume plain CSV. Use --table depth for L2 diffs, snapshots for book anchors,
or trades for execution prints.

Example
-------
python3 tools/parquet_to_csv.py --table depth \
    --input depth/binance/BTCUSDT/2026-08.parquet \
    --exchange binance --asset BTCUSDT --out data/btcusdt_depth.csv

Then:
    ./build/hft_replay crypto-depth --file data/btcusdt_depth.csv \
        --asset BTCUSDT --exchange binance --price-scale 100
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import sys

try:
    import pyarrow.parquet as pq
except ImportError:  # pragma: no cover
    print("pyarrow is required: pip install pyarrow", file=sys.stderr)
    raise SystemExit(2)


def shards(inputs: list[str]) -> list[pathlib.Path]:
    paths: list[pathlib.Path] = []
    for value in inputs:
        path = pathlib.Path(value)
        if path.is_dir():
            paths.extend(sorted(path.rglob("*.parquet")))
        elif any(character in value for character in "*?["):
            paths.extend(sorted(pathlib.Path().glob(value)))
        else:
            paths.append(path)
    return paths


def convert(input_path: pathlib.Path, output, table: str, exchange: str,
            asset: str, limit: int) -> int:
    parquet = pq.ParquetFile(input_path)
    if table == "trades":
        columns = ["timestamp_ms", "exchange", "asset", "trade_id", "price",
                   "quantity", "buyer_maker"]
        writer = csv.writer(output)
        writer.writerow(["trade_id", "price", "qty", "quoteQty", "time",
                         "isBuyerMaker", "isBestMatch"])
    else:
        columns = ["timestamp_ms", "exchange", "asset", "side", "price",
                   "quantity", "last_update_id"]
        writer = csv.writer(output)
        writer.writerow(columns)

    emitted = 0
    for batch in parquet.iter_batches(batch_size=131_072, columns=columns):
        data = batch.to_pydict()
        for index in range(batch.num_rows):
            if exchange and data["exchange"][index] != exchange:
                continue
            if asset and data["asset"][index] != asset:
                continue
            if table == "trades":
                price = data["price"][index]
                quantity = data["quantity"][index]
                writer.writerow([
                    data["trade_id"][index], price, quantity, price * quantity,
                    data["timestamp_ms"][index], data["buyer_maker"][index], True,
                ])
            else:
                writer.writerow([
                    data["timestamp_ms"][index], data["exchange"][index],
                    data["asset"][index], data["side"][index],
                    data["price"][index], data["quantity"][index],
                    data["last_update_id"][index],
                ])
            emitted += 1
            if limit and emitted >= limit:
                return emitted
    return emitted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", choices=["trades", "depth", "snapshots"],
                        required=True)
    parser.add_argument("--input", nargs="+", required=True,
                        help="Parquet file, directory, or glob")
    parser.add_argument("--out", required=True)
    parser.add_argument("--exchange", default="binance")
    parser.add_argument("--asset", default="BTCUSDT")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    paths = shards(args.input)
    if not paths:
        parser.error("no Parquet inputs matched")
    output_path = pathlib.Path(args.out)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    total = 0
    with open(output_path, "w", newline="") as output:
        for path in paths:
            if not path.exists():
                print(f"missing {path}", file=sys.stderr)
                continue
            count = convert(path, output, args.table, args.exchange,
                            args.asset, args.limit)
            total += count
            print(f"{path}: {count} rows", file=sys.stderr)
            if args.limit and total >= args.limit:
                break
    print(f"wrote {total} rows to {output_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
