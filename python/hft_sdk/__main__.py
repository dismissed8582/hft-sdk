"""Command-line entry point: ``hft-sdk data/sample_ticks.csv``."""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from typing import Any

import hft_sdk


def _run_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="hft-sdk",
        description="Run the LOB imbalance engine and PnL backtest on market data.",
    )
    parser.add_argument(
        "source",
        help="tick CSV, Binance archive CSV, crypto-lob Parquet, or http(s)/hf:// URL",
    )
    parser.add_argument(
        "--scheme",
        default="UF",
        choices=["UF", "LF", "MF", "MTF"],
        help="order-book filter scheme (default: UF)",
    )
    parser.add_argument(
        "--horizon",
        default=1,
        type=int,
        choices=list(hft_sdk.HORIZONS),
        help="forward-return horizon in seconds (default: 1)",
    )
    parser.add_argument(
        "--signal",
        default="volume",
        choices=["volume", "count"],
        help="OBI weighting (default: volume)",
    )
    parser.add_argument("--fee-bps", type=float, default=2.0,
                        help="taker fee in bps per side (default: 2.0)")
    parser.add_argument("--latency-us", type=int, default=100,
                        help="execution latency in microseconds (default: 100)")
    parser.add_argument("--maker", action="store_true",
                        help="charge the maker fee instead of the taker fee")
    parser.add_argument("--threshold", type=float, default=0.0,
                        help="minimum |signal| to trade (default: 0.0)")
    parser.add_argument("--price-scale", type=float, default=100.0,
                        help="price ticks per unit for raw feeds (default: 100)")
    parser.add_argument("--qty-scale", type=float, default=1e8,
                        help="quantity ticks per unit for raw feeds (default: 1e8)")
    parser.add_argument("--asset", help="filter a multi-asset file")
    parser.add_argument("--exchange", help="filter a multi-exchange file")
    parser.add_argument("--table", choices=["depth", "snapshots", "trades"],
                        help="override Parquet table detection")
    parser.add_argument("--force", action="store_true",
                        help="re-download the source URL if it is cached")
    parser.add_argument("--json", action="store_true",
                        help="print machine-readable JSON")
    return parser


def _fetch_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="hft-sdk fetch",
        description="Download public market data into the local cache.",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("url", nargs="?",
                       help="direct http(s) or hf:// URL to download")
    group.add_argument("--binance", metavar="SYMBOL",
                       help="download Binance public archives")
    group.add_argument("--crypto-lob", metavar="SYMBOL",
                       help="download a crypto-lob-stream Parquet shard")
    parser.add_argument("--start", help="Binance start date (YYYY-MM-DD or YYYY-MM)")
    parser.add_argument("--end", help="Binance end date (defaults to --start)")
    parser.add_argument("--period", help="crypto-lob month (YYYY-MM)")
    parser.add_argument("--market", default="spot", choices=["spot", "futures"],
                        help="Binance market (default: spot)")
    parser.add_argument("--kind", default="trades",
                        choices=["trades", "aggTrades", "bookDepth"],
                        help="Binance archive kind (default: trades)")
    parser.add_argument("--monthly", action="store_true",
                        help="use monthly instead of daily Binance archives")
    parser.add_argument("--table", default="depth",
                        choices=["depth", "snapshots", "trades"],
                        help="crypto-lob table (default: depth)")
    parser.add_argument("--exchange", default="binance",
                        help="crypto-lob exchange (default: binance)")
    parser.add_argument("-o", "--out", help="output directory")
    parser.add_argument("--force", action="store_true",
                        help="download even if the file already exists")
    parser.add_argument("--jobs", type=int, default=4,
                        help="parallel downloads for Binance (default: 4)")
    return parser


def _selected_metrics(result: hft_sdk.RunResult):
    code = hft_sdk.scheme_code(result.scheme)
    horizon_s = hft_sdk.HORIZONS[result.horizon]
    for row in result.metrics:
        if row.scheme == code and row.horizon_s == horizon_s:
            return row
    return None


def _run_main(argv: Sequence[str]) -> int:
    args = _run_parser().parse_args(argv)
    volume = args.signal == "volume"

    result = hft_sdk.run_source(
        args.source,
        scheme=args.scheme,
        horizon=args.horizon,
        volume=volume,
        table=args.table,
        price_scale=args.price_scale,
        quantity_scale=args.qty_scale,
        asset=args.asset,
        exchange=args.exchange,
    )

    config = hft_sdk.BacktestConfig()
    config.maker = args.maker
    config.maker_fee_bps = args.fee_bps
    config.taker_fee_bps = args.fee_bps
    config.latency_ns = args.latency_us * 1_000
    config.threshold = args.threshold
    pnl = result.backtest(config)
    metrics = _selected_metrics(result)

    summary = {
        "source": args.source,
        "snapshots": len(result.snapshots),
        "scheme": hft_sdk.scheme_code(result.scheme),
        "horizon_s": hft_sdk.HORIZONS[result.horizon],
        "signal": args.signal,
        "metrics": None if metrics is None else {
            "samples": metrics.samples,
            "pearson": metrics.pearson.pearson,
            "r2": metrics.pearson.r2,
            "regime_pearson": metrics.regime.pearson,
            "regime_r2": metrics.regime.r2,
        },
        "pnl": {
            "trade_count": pnl.trade_count,
            "total_return": pnl.total_return,
            "win_rate": pnl.win_rate,
            "sharpe_trade": pnl.sharpe_trade,
            "max_drawdown": pnl.max_drawdown,
            "total_fees": pnl.total_fees,
        },
    }

    if args.json:
        json.dump(summary, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    print(f"source:   {summary['source']}")
    print(f"snapshots:{summary['snapshots']}")
    print(
        f"settings: scheme={summary['scheme']} "
        f"horizon={summary['horizon_s']}s signal={summary['signal']}"
    )
    if metrics is not None:
        print(
            f"signal:   samples={metrics.samples} "
            f"pearson={metrics.pearson.pearson:.6f} "
            f"r2={metrics.pearson.r2:.6f}"
        )
    print(
        f"pnl:      trades={pnl.trade_count} "
        f"return={pnl.total_return:.4%} "
        f"win_rate={pnl.win_rate:.2%} "
        f"sharpe={pnl.sharpe_trade:.2f} "
        f"max_dd={pnl.max_drawdown:.2%} "
        f"fees={pnl.total_fees:.4%}"
    )
    return 0


def _fetch_main(argv: Sequence[str]) -> int:
    args = _fetch_parser().parse_args(argv)
    out_dir: Any = args.out

    if args.url is not None:
        path = hft_sdk.fetch(args.url, out_dir=out_dir, force=args.force)
        print(path)
        return 0

    if args.binance is not None:
        if not args.start:
            _fetch_parser().error("--start is required with --binance")
        paths = hft_sdk.fetch_binance(
            args.binance,
            args.start,
            args.end,
            market=args.market,
            kind=args.kind,
            monthly=args.monthly,
            out_dir=out_dir,
            force=args.force,
            jobs=args.jobs,
        )
        for path in paths:
            print(path)
        return 0

    if not args.period:
        _fetch_parser().error("--period is required with --crypto-lob")
    path = hft_sdk.fetch_crypto_lob(
        args.crypto_lob,
        args.period,
        table=args.table,
        exchange=args.exchange,
        out_dir=out_dir,
        force=args.force,
    )
    print(path)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] in {"fetch", "download"}:
        return _fetch_main(arguments[1:])
    return _run_main(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
