"""Download and normalize public market data.

The helpers in this module deliberately avoid API keys: they use Binance's
public archive at ``data.binance.vision`` and the Hugging Face
``Goooddy/crypto-lob-stream`` dataset. Everything is cached on disk, and the
same functions that download a file can feed it straight into the engine.

Examples
--------
>>> path = fetch_crypto_lob("BTCUSDT", "2026-08", table="depth")
>>> result = hft_sdk.run_source(path, price_scale=100.0)

>>> paths = fetch_binance("BTCUSDT", "2024-01", "2024-03", market="spot",
...                       kind="aggTrades", monthly=True)
>>> result = hft_sdk.run_sources(paths)
"""

from __future__ import annotations

import concurrent.futures
import csv
import datetime as dt
import hashlib
import heapq
import itertools
import os
import pathlib
import urllib.parse
import urllib.request
import zipfile
from collections.abc import Iterable, Iterator, Mapping, Sequence
from typing import Any

import numpy as np

HF_BASE = "https://huggingface.co/datasets/Goooddy/crypto-lob-stream/resolve/main"
BINANCE_BASE = "https://data.binance.vision/data"
USER_AGENT = "hft-sdk/1.0"

ARRAY_KEYS = (
    "order_id",
    "timestamp_ns",
    "price_ticks",
    "quantity",
    "bid_price_ticks",
    "ask_price_ticks",
    "type_codes",
    "side_codes",
)

EVENT_NEW = 0
EVENT_MODIFY = 1
EVENT_CANCEL = 2
EVENT_TRADE = 3

SIDE_NONE = 0
SIDE_BID = 1
SIDE_ASK = 2
SIDE_BUY = 3
SIDE_SELL = 4


def default_data_dir() -> pathlib.Path:
    """Return the cache directory, honouring ``HFT_SDK_DATA_DIR``."""
    configured = os.environ.get("HFT_SDK_DATA_DIR")
    if configured:
        root = pathlib.Path(configured).expanduser()
    else:
        cache = os.environ.get("XDG_CACHE_HOME")
        base = pathlib.Path(cache).expanduser() if cache else pathlib.Path.home() / ".cache"
        root = base / "hft-sdk"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _is_url(value: str) -> bool:
    lowered = value.lower()
    return lowered.startswith(("http://", "https://", "hf://"))


def _expand_url(url: str) -> str:
    if url.lower().startswith("hf://"):
        return f"{HF_BASE}/{url[5:].lstrip('/')}"
    return url


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _download(
    url: str,
    destination: pathlib.Path,
    *,
    force: bool,
    timeout: float,
    expected_sha256: str | None,
    progress: bool,
) -> pathlib.Path:
    url = _expand_url(url)
    destination = destination.expanduser()
    if destination.exists() and not force:
        if expected_sha256 is None or _sha256(destination) == expected_sha256.lower():
            return destination
        destination.unlink()

    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response, \
                partial.open("wb") as output:
            total = int(response.headers.get("Content-Length") or 0)
            written = 0
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                output.write(chunk)
                written += len(chunk)
                if progress and total:
                    percent = 100.0 * written / total
                    print(
                        f"\r{percent:5.1f}%  {destination.name}  "
                        f"{written >> 20}/{total >> 20} MiB",
                        end="",
                        flush=True,
                    )
        if progress and total:
            print()
        if expected_sha256 is not None:
            actual = _sha256(partial)
            if actual != expected_sha256.lower():
                raise RuntimeError(
                    f"checksum mismatch for {destination.name}: {actual} "
                    f"!= {expected_sha256.lower()}"
                )
        partial.replace(destination)
    except Exception:
        partial.unlink(missing_ok=True)
        raise
    return destination


def download(
    url: str,
    destination: str | pathlib.Path | None = None,
    *,
    out_dir: str | pathlib.Path | None = None,
    force: bool = False,
    timeout: float = 120.0,
    sha256: str | None = None,
    progress: bool = True,
) -> pathlib.Path:
    """Download ``url`` (HTTP or ``hf://``) into the cache or ``destination``."""
    if destination is not None:
        candidate = pathlib.Path(destination).expanduser()
        if candidate.is_dir():
            out_dir = candidate
            destination = None
    if destination is None and out_dir is not None:
        name = urllib.parse.unquote(pathlib.PurePosixPath(
            _expand_url(url).split("?", 1)[0]).name)
        if not name:
            raise ValueError("cannot infer a filename from the URL")
        destination = pathlib.Path(out_dir).expanduser() / name
    if destination is None:
        expanded = _expand_url(url)
        name = urllib.parse.unquote(pathlib.PurePosixPath(
            expanded.split("?", 1)[0]).name)
        if not name:
            raise ValueError("cannot infer a filename from the URL")
        host = urllib.parse.urlparse(expanded).netloc or "local"
        destination = default_data_dir() / "downloads" / host / name
    return _download(
        url,
        pathlib.Path(destination),
        force=force,
        timeout=timeout,
        expected_sha256=sha256,
        progress=progress,
    )


def fetch(url: str, *, out_dir: str | pathlib.Path | None = None,
          force: bool = False, progress: bool = True) -> pathlib.Path:
    """Alias for :func:`download` with the common cache arguments."""
    return download(url, out_dir=out_dir, force=force, progress=progress)


def resolve_source(
    source: str | pathlib.Path,
    *,
    force: bool = False,
    progress: bool = True,
) -> pathlib.Path:
    """Return a local path, downloading URLs into the cache when needed."""
    if isinstance(source, pathlib.Path):
        return source.expanduser()
    text = str(source)
    if _is_url(text):
        return download(text, force=force, progress=progress)
    return pathlib.Path(text).expanduser()


def _month_range(start: str, end: str) -> list[str]:
    first = dt.date.fromisoformat(start + "-01" if len(start) == 7 else start)
    last = dt.date.fromisoformat(end + "-01" if len(end) == 7 else end)
    months: list[str] = []
    year, month = first.year, first.month
    while (year, month) <= (last.year, last.month):
        months.append(f"{year:04d}-{month:02d}")
        month += 1
        if month == 13:
            year += 1
            month = 1
    return months


def _day_range(start: str, end: str) -> list[str]:
    first = dt.date.fromisoformat(start)
    last = dt.date.fromisoformat(end)
    return [
        (first + dt.timedelta(days=offset)).isoformat()
        for offset in range((last - first).days + 1)
    ]


def _binance_url(market: str, kind: str, symbol: str, period: str,
                 monthly: bool) -> str:
    prefix = "futures/um" if market == "futures" else "spot"
    folder = "monthly" if monthly else "daily"
    return f"{BINANCE_BASE}/{prefix}/{folder}/{kind}/{symbol}/{symbol}-{kind}-{period}.zip"


def _checksum(url: str) -> str | None:
    request = urllib.request.Request(url + ".CHECKSUM",
                                     headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read().decode().split()[0].strip()
    except Exception:
        return None


def _fetch_binance_one(
    url: str,
    out_dir: pathlib.Path,
    *,
    force: bool,
    keep_zip: bool,
    progress: bool,
) -> pathlib.Path:
    name = url.rsplit("/", 1)[-1]
    csv_path = out_dir / (name[:-4] + ".csv")
    if csv_path.exists() and not force:
        return csv_path

    archive = _download(
        url,
        out_dir / "zips" / name,
        force=force,
        timeout=180.0,
        expected_sha256=_checksum(url),
        progress=progress,
    )
    with zipfile.ZipFile(archive) as zipped:
        zipped.extractall(out_dir)
    if not keep_zip:
        archive.unlink(missing_ok=True)
    if not csv_path.exists():
        extracted = list(out_dir.glob(pathlib.Path(name).stem + "*"))
        if not extracted:
            raise RuntimeError(f"archive did not contain {csv_path.name}")
        csv_path = extracted[0]
    return csv_path


def fetch_binance(
    symbol: str,
    start: str,
    end: str | None = None,
    *,
    market: str = "spot",
    kind: str = "trades",
    monthly: bool = False,
    out_dir: str | pathlib.Path | None = None,
    force: bool = False,
    keep_zip: bool = False,
    jobs: int = 4,
    progress: bool = True,
) -> list[pathlib.Path]:
    """Download Binance public archives and return the extracted CSV paths.

    ``market`` is ``spot`` or ``futures``; ``kind`` is ``trades``,
    ``aggTrades`` or ``bookDepth``. Dates are ``YYYY-MM-DD`` (daily) or
    ``YYYY-MM`` (monthly). ``bookDepth`` is futures+daily only.
    """
    if market not in {"spot", "futures"}:
        raise ValueError("market must be spot or futures")
    if kind not in {"trades", "aggTrades", "bookDepth"}:
        raise ValueError("kind must be trades, aggTrades or bookDepth")
    if kind == "bookDepth" and market != "futures":
        raise ValueError("bookDepth is only published for USD-M futures")
    if kind == "bookDepth" and monthly:
        raise ValueError("bookDepth archives are daily only")

    end = end or start
    periods = _month_range(start, end) if monthly else _day_range(start, end)
    target = (pathlib.Path(out_dir).expanduser()
              if out_dir is not None else default_data_dir() / "binance")
    target.mkdir(parents=True, exist_ok=True)

    urls = [_binance_url(market, kind, symbol, period, monthly)
            for period in periods]
    results: list[pathlib.Path] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = [
            pool.submit(
                _fetch_binance_one, url, target, force=force,
                keep_zip=keep_zip, progress=progress,
            )
            for url in urls
        ]
        for future in futures:
            results.append(future.result())
    return results


def fetch_crypto_lob(
    symbol: str,
    period: str,
    *,
    table: str = "depth",
    exchange: str = "binance",
    out_dir: str | pathlib.Path | None = None,
    force: bool = False,
    progress: bool = True,
) -> pathlib.Path:
    """Download one crypto-lob-stream Parquet shard.

    ``table`` is ``depth``, ``snapshots`` or ``trades`` and ``period`` is
    ``YYYY-MM``. Files are cached under ``<data-dir>/crypto-lob`` by default.
    """
    if table not in {"depth", "snapshots", "trades"}:
        raise ValueError("table must be depth, snapshots or trades")
    url = f"{HF_BASE}/{table}/{exchange}/{symbol}/{period}.parquet"
    target = (pathlib.Path(out_dir).expanduser()
              if out_dir is not None else default_data_dir() / "crypto-lob")
    destination = target / table / exchange / symbol / f"{period}.parquet"
    return _download(
        url,
        destination,
        force=force,
        timeout=300.0,
        expected_sha256=None,
        progress=progress,
    )


def _header(path: pathlib.Path) -> list[str]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        return next(reader, [])


def _lower_columns(header: Iterable[str]) -> dict[str, int]:
    return {name.strip().lower(): index for index, name in enumerate(header)}


def _find(columns: Mapping[str, int], *names: str) -> int | None:
    for name in names:
        index = columns.get(name.lower())
        if index is not None:
            return index
    return None


def _looks_headerless_binance(row: Sequence[str]) -> bool:
    if len(row) not in {6, 7, 8}:
        return False
    try:
        float(row[0])
        float(row[1])
        float(row[2])
    except ValueError:
        return False
    for index in (5, 4):
        try:
            float(row[index])
            return True
        except (ValueError, IndexError):
            continue
    return False


def detect_source(path: str | pathlib.Path,
                  table: str | None = None) -> str:
    """Classify a file as ``tick``, ``binance``, ``bookdepth`` or a Parquet table."""
    path = pathlib.Path(path)
    suffix = path.suffix.lower()
    if suffix == ".parquet":
        try:
            import pyarrow.parquet as pq
        except ImportError as error:  # pragma: no cover - dependency is declared
            raise ImportError(
                "parquet support requires pyarrow: pip install pyarrow"
            ) from error
        names = set(pq.ParquetFile(path).schema_arrow.names)
        if "trade_id" in names:
            return "trades"
        if "last_update_id" in names:
            return "depth"
        if {"side", "price", "quantity"} <= names:
            if table in {"depth", "snapshots"}:
                return table
            raise ValueError(
                "cannot tell depth from snapshots; pass table='depth' or "
                "table='snapshots'"
            )
        raise ValueError(f"unsupported parquet columns: {sorted(names)}")

    header = _header(path)
    if _looks_headerless_binance(header):
        return "binance"
    columns = _lower_columns(header)
    if "order_id" in columns and (
        "timestamp_ns" in columns or "timestamp" in columns
    ):
        return "tick"
    if "percentage" in columns and "depth" in columns and "notional" in columns:
        return "bookdepth"
    if (
        _find(columns, "isbuyermaker", "is_buyer_maker", "buyer_maker", "m")
        is not None
    ):
        return "binance"
    if (
        "exchange" in columns
        and "asset" in columns
        and "side" in columns
        and "price" in columns
        and "quantity" in columns
    ):
        return "depth"
    raise ValueError(f"unsupported data source: {path}")


def _timestamp_ns(value: int | float, unit: str) -> int:
    value = int(value)
    if unit == "ns":
        return value
    if unit == "us":
        return value * 1_000
    if unit == "ms":
        return value * 1_000_000
    if unit == "s":
        return value * 1_000_000_000
    magnitude = abs(value)
    if magnitude >= 100_000_000_000_000_000:
        return value
    if magnitude >= 100_000_000_000_000:
        return value * 1_000
    if magnitude >= 100_000_000_000:
        return value * 1_000_000
    return value * 1_000_000_000


def _parse_utc_ns(text: str) -> int:
    text = text.strip()
    if not text:
        raise ValueError("empty timestamp")
    if text.lstrip("-").isdigit():
        return _timestamp_ns(int(text), "auto")
    try:
        parsed = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError(f"unsupported timestamp: {text}") from error
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return int(parsed.timestamp() * 1_000_000_000)


def _empty_batch() -> dict[str, list[int]]:
    return {key: [] for key in ARRAY_KEYS}


def _finish_batch(batch: dict[str, list[int]]) -> dict[str, np.ndarray]:
    return {key: np.asarray(values, dtype=np.int64) for key, values in batch.items()}


class _LevelBook:
    def __init__(self) -> None:
        self.ids: dict[tuple[int, int], int] = {}
        self.quantities: dict[tuple[int, int], int] = {}
        self.bids: list[int] = []
        self.asks: list[int] = []
        self.next_id = 1

    def _clean(self) -> None:
        while self.bids and (SIDE_BID, -self.bids[0]) not in self.ids:
            heapq.heappop(self.bids)
        while self.asks and (SIDE_ASK, self.asks[0]) not in self.ids:
            heapq.heappop(self.asks)

    def update(self, side: int, price_ticks: int, quantity: int) -> tuple[int, int, int, int] | None:
        key = (side, price_ticks)
        if quantity > 0:
            if key in self.ids:
                event_type = EVENT_MODIFY
                order_id = self.ids[key]
            else:
                event_type = EVENT_NEW
                order_id = self.next_id
                self.next_id += 1
                self.ids[key] = order_id
                heapq.heappush(
                    self.bids if side == SIDE_BID else self.asks,
                    -price_ticks if side == SIDE_BID else price_ticks,
                )
            self.quantities[key] = quantity
        else:
            if key not in self.ids:
                return None
            event_type = EVENT_CANCEL
            order_id = self.ids.pop(key)
            self.quantities.pop(key, None)
        self._clean()
        best_bid = -self.bids[0] if self.bids else 0
        best_ask = self.asks[0] if self.asks else 0
        return event_type, order_id, best_bid, best_ask


class _Snapshot:
    def __init__(self, timestamp_ns: int) -> None:
        self.timestamp_ns = timestamp_ns
        self.depth: dict[int, float] = {}
        self.notional: dict[int, float] = {}


def _iter_binance_csv(
    path: pathlib.Path,
    *,
    price_scale: float,
    quantity_scale: float,
    batch_size: int,
) -> Iterator[dict[str, np.ndarray]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        first = next(reader, None)
        if first is None:
            return
        if _looks_headerless_binance(first):
            if len(first) == 8:
                time_index, price_index, quantity_index = 5, 1, 2
                maker_index, id_index = 6, 0
            else:
                time_index, price_index, quantity_index = 4, 1, 2
                maker_index, id_index = 5, 0
            time_unit = "auto"
            rows = itertools.chain([first], reader)
        else:
            columns = _lower_columns(first)
            time_index = _find(columns, "time", "transact_time", "timestamp",
                               "timestamp_ms")
            price_index = _find(columns, "price")
            quantity_index = _find(columns, "qty", "quantity")
            maker_index = _find(columns, "isbuyermaker", "is_buyer_maker",
                                "buyer_maker", "m")
            id_index = _find(columns, "trade_id", "id", "agg_trade_id", "a")
            if None in (time_index, price_index, quantity_index, maker_index):
                raise ValueError(f"unsupported Binance trade CSV: {path}")
            time_unit = "ms" if columns.get("timestamp_ms") is not None else "auto"
            rows = reader

        counter = 0
        batch = _empty_batch()
        for row in rows:
            if not row:
                continue
            counter += 1
            timestamp = _timestamp_ns(row[time_index], time_unit)
            price_ticks = int(round(float(row[price_index]) * price_scale))
            quantity = int(round(float(row[quantity_index]) * quantity_scale))
            maker = row[maker_index].strip().lower() in {"1", "true", "t"}
            side = SIDE_SELL if maker else SIDE_BUY
            order_id = int(row[id_index]) if id_index is not None else counter

            batch["order_id"].append(order_id)
            batch["timestamp_ns"].append(timestamp)
            batch["price_ticks"].append(price_ticks)
            batch["quantity"].append(quantity)
            batch["bid_price_ticks"].append(price_ticks)
            batch["ask_price_ticks"].append(price_ticks)
            batch["type_codes"].append(EVENT_TRADE)
            batch["side_codes"].append(side)
            if len(batch["order_id"]) >= batch_size:
                yield _finish_batch(batch)
                batch = _empty_batch()
        if batch["order_id"]:
            yield _finish_batch(batch)


def _iter_parquet_trades(
    path: pathlib.Path,
    *,
    price_scale: float,
    quantity_scale: float,
    asset: str | None,
    exchange: str | None,
    batch_size: int,
) -> Iterator[dict[str, np.ndarray]]:
    import pyarrow.parquet as pq

    parquet = pq.ParquetFile(path)
    names = set(parquet.schema_arrow.names)
    columns = ["timestamp_ms", "price", "quantity", "buyer_maker"]
    if "trade_id" in names:
        columns.append("trade_id")
    if "asset" in names:
        columns.append("asset")
    if "exchange" in names:
        columns.append("exchange")

    counter = 0
    for record in parquet.iter_batches(batch_size=batch_size, columns=columns):
        data = record.to_pydict()
        count = record.num_rows
        batch = _empty_batch()
        for index in range(count):
            if asset is not None and "asset" in data and data["asset"][index] != asset:
                continue
            if exchange is not None and "exchange" in data and \
                    data["exchange"][index] != exchange:
                continue
            counter += 1
            price_ticks = int(round(data["price"][index] * price_scale))
            quantity = int(round(data["quantity"][index] * quantity_scale))
            maker = bool(data["buyer_maker"][index])
            order_id = (int(data["trade_id"][index])
                        if "trade_id" in data else counter)
            batch["order_id"].append(order_id)
            batch["timestamp_ns"].append(
                int(data["timestamp_ms"][index]) * 1_000_000)
            batch["price_ticks"].append(price_ticks)
            batch["quantity"].append(quantity)
            batch["bid_price_ticks"].append(price_ticks)
            batch["ask_price_ticks"].append(price_ticks)
            batch["type_codes"].append(EVENT_TRADE)
            batch["side_codes"].append(SIDE_SELL if maker else SIDE_BUY)
        if batch["order_id"]:
            yield _finish_batch(batch)


def _iter_depth_rows(
    rows: Iterator[tuple[int, str, str, str, float, float]],
    *,
    price_scale: float,
    quantity_scale: float,
    asset: str | None,
    exchange: str | None,
    batch_size: int,
) -> Iterator[dict[str, np.ndarray]]:
    books: dict[str, _LevelBook] = {}
    batch = _empty_batch()
    for timestamp_ns, row_exchange, row_asset, side_text, price, quantity in rows:
        if exchange is not None and row_exchange != exchange:
            continue
        if asset is not None and row_asset != asset:
            continue
        side = SIDE_BID if side_text.strip().lower() == "bid" else SIDE_ASK
        price_ticks = int(round(price * price_scale))
        quantity_ticks = int(round(quantity * quantity_scale))
        book = books.get(row_asset)
        if book is None:
            book = _LevelBook()
            books[row_asset] = book
        update = book.update(side, price_ticks, quantity_ticks)
        if update is None:
            continue
        event_type, order_id, best_bid, best_ask = update
        batch["order_id"].append(order_id)
        batch["timestamp_ns"].append(timestamp_ns)
        batch["price_ticks"].append(price_ticks)
        batch["quantity"].append(quantity_ticks)
        batch["bid_price_ticks"].append(best_bid)
        batch["ask_price_ticks"].append(best_ask)
        batch["type_codes"].append(event_type)
        batch["side_codes"].append(side)
        if len(batch["order_id"]) >= batch_size:
            yield _finish_batch(batch)
            batch = _empty_batch()
    if batch["order_id"]:
        yield _finish_batch(batch)


def _iter_crypto_csv_depth(
    path: pathlib.Path,
    *,
    price_scale: float,
    quantity_scale: float,
    asset: str | None,
    exchange: str | None,
    batch_size: int,
) -> Iterator[dict[str, np.ndarray]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None:
            return
        columns = _lower_columns(header)
        timestamp_index = _find(columns, "timestamp_ms", "timestamp", "time")
        exchange_index = _find(columns, "exchange")
        asset_index = _find(columns, "asset", "symbol")
        side_index = _find(columns, "side")
        price_index = _find(columns, "price")
        quantity_index = _find(columns, "quantity", "qty")
        if None in (timestamp_index, side_index, price_index, quantity_index):
            raise ValueError(f"unsupported crypto-lob depth CSV: {path}")
        time_unit = "ms" if columns.get("timestamp_ms") is not None else "auto"

        def rows() -> Iterator[tuple[int, str, str, str, float, float]]:
            for row in reader:
                if not row:
                    continue
                yield (
                    _timestamp_ns(row[timestamp_index], time_unit),
                    row[exchange_index] if exchange_index is not None else "",
                    row[asset_index] if asset_index is not None else "",
                    row[side_index],
                    float(row[price_index]),
                    float(row[quantity_index]),
                )

        yield from _iter_depth_rows(
            rows(), price_scale=price_scale, quantity_scale=quantity_scale,
            asset=asset, exchange=exchange, batch_size=batch_size,
        )


def _iter_parquet_depth(
    path: pathlib.Path,
    *,
    price_scale: float,
    quantity_scale: float,
    asset: str | None,
    exchange: str | None,
    batch_size: int,
) -> Iterator[dict[str, np.ndarray]]:
    import pyarrow.parquet as pq

    parquet = pq.ParquetFile(path)
    names = set(parquet.schema_arrow.names)
    columns = ["timestamp_ms", "price", "quantity", "side"]
    if "asset" in names:
        columns.append("asset")
    if "exchange" in names:
        columns.append("exchange")

    def rows() -> Iterator[tuple[int, str, str, str, float, float]]:
        for record in parquet.iter_batches(batch_size=batch_size, columns=columns):
            data = record.to_pydict()
            for index in range(record.num_rows):
                yield (
                    int(data["timestamp_ms"][index]) * 1_000_000,
                    data["exchange"][index] if "exchange" in data else "",
                    data["asset"][index] if "asset" in data else "",
                    data["side"][index],
                    float(data["price"][index]),
                    float(data["quantity"][index]),
                )

    yield from _iter_depth_rows(
        rows(), price_scale=price_scale, quantity_scale=quantity_scale,
        asset=asset, exchange=exchange, batch_size=batch_size,
    )


def _iter_binance_bookdepth(
    path: pathlib.Path,
    *,
    price_scale: float,
    quantity_scale: float,
    batch_size: int,
) -> Iterator[dict[str, np.ndarray]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None:
            return
        if _find(_lower_columns(header), "percentage") is None:
            raise ValueError(f"unsupported bookDepth CSV: {path}")

    batch = _empty_batch()
    synthetic_id = 0

    def emit(snapshot: _Snapshot) -> None:
        nonlocal batch, synthetic_id
        if -1 not in snapshot.depth or 1 not in snapshot.depth:
            return
        bid_price = snapshot.notional[-1] / snapshot.depth[-1]
        ask_price = snapshot.notional[1] / snapshot.depth[1]
        mid = 0.5 * (bid_price + ask_price)
        if mid <= 0.0:
            return
        bid_ticks = int(round(mid * 0.99 * price_scale))
        ask_ticks = int(round(mid * 1.01 * price_scale))
        bid_quantity = int(round(snapshot.notional[-1] * quantity_scale))
        ask_quantity = int(round(snapshot.notional[1] * quantity_scale))
        synthetic_id += 1
        batch["order_id"].append(synthetic_id)
        batch["timestamp_ns"].append(snapshot.timestamp_ns)
        batch["price_ticks"].append(bid_ticks)
        batch["quantity"].append(bid_quantity)
        batch["bid_price_ticks"].append(bid_ticks)
        batch["ask_price_ticks"].append(ask_ticks)
        batch["type_codes"].append(EVENT_NEW)
        batch["side_codes"].append(SIDE_BID)
        synthetic_id += 1
        batch["order_id"].append(synthetic_id)
        batch["timestamp_ns"].append(snapshot.timestamp_ns)
        batch["price_ticks"].append(ask_ticks)
        batch["quantity"].append(ask_quantity)
        batch["bid_price_ticks"].append(bid_ticks)
        batch["ask_price_ticks"].append(ask_ticks)
        batch["type_codes"].append(EVENT_NEW)
        batch["side_codes"].append(SIDE_ASK)

    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        next(reader, None)
        current: _Snapshot | None = None
        for row in reader:
            if len(row) < 4:
                continue
            timestamp_ns = _parse_utc_ns(row[0])
            percent = int(round(float(row[1])))
            if current is None or current.timestamp_ns != timestamp_ns:
                if current is not None:
                    emit(current)
                    if len(batch["order_id"]) >= batch_size:
                        yield _finish_batch(batch)
                        batch = _empty_batch()
                current = _Snapshot(timestamp_ns)
            current.depth[percent] = float(row[2])
            current.notional[percent] = float(row[3])
        if current is not None:
            emit(current)
    if batch["order_id"]:
        yield _finish_batch(batch)


def iter_tick_arrays(
    source: str | pathlib.Path,
    *,
    table: str | None = None,
    price_scale: float = 100.0,
    quantity_scale: float = 1e8,
    asset: str | None = None,
    exchange: str | None = None,
    batch_size: int = 131_072,
) -> Iterator[dict[str, np.ndarray]]:
    """Yield engine-ready NumPy arrays from a local data file.

    Supported inputs are tick CSVs, Binance trade archives, Binance bookDepth
    archives, crypto-lob depth Parquet/CSV and crypto-lob trade Parquet.
    """
    path = pathlib.Path(source).expanduser()
    if not path.exists():
        raise FileNotFoundError(path)
    kind = detect_source(path, table=table)
    if kind == "tick":
        from . import _core

        yield _core.read_csv(str(path))
    elif kind == "binance":
        yield from _iter_binance_csv(
            path, price_scale=price_scale, quantity_scale=quantity_scale,
            batch_size=batch_size,
        )
    elif kind == "bookdepth":
        yield from _iter_binance_bookdepth(
            path, price_scale=price_scale, quantity_scale=quantity_scale,
            batch_size=batch_size,
        )
    elif kind == "depth":
        if path.suffix.lower() == ".parquet":
            yield from _iter_parquet_depth(
                path, price_scale=price_scale,
                quantity_scale=quantity_scale, asset=asset, exchange=exchange,
                batch_size=batch_size,
            )
        else:
            yield from _iter_crypto_csv_depth(
                path, price_scale=price_scale,
                quantity_scale=quantity_scale, asset=asset, exchange=exchange,
                batch_size=batch_size,
            )
    elif kind == "trades":
        yield from _iter_parquet_trades(
            path, price_scale=price_scale, quantity_scale=quantity_scale,
            asset=asset, exchange=exchange, batch_size=batch_size,
        )
    else:
        raise ValueError(
            "snapshots are not converted directly; use table='depth' instead"
        )


def to_tick_csv(
    source: str | pathlib.Path,
    output: str | pathlib.Path,
    **kwargs: Any,
) -> pathlib.Path:
    """Normalize any supported source into the SDK's tick CSV format."""
    destination = pathlib.Path(output).expanduser()
    destination.parent.mkdir(parents=True, exist_ok=True)
    type_names = np.asarray(["NEW", "MODIFY", "CANCEL", "TRADE"], dtype=object)
    side_names = np.asarray(["NONE", "BID", "ASK", "BUY", "SELL"], dtype=object)
    wrote_header = False
    with destination.open("w", newline="") as handle:
        writer = csv.writer(handle)
        for batch in iter_tick_arrays(source, **kwargs):
            if not wrote_header:
                writer.writerow([
                    "timestamp_ns", "event_type", "side", "order_id",
                    "price_ticks", "quantity", "modifications",
                    "bid_price_ticks", "ask_price_ticks",
                ])
                wrote_header = True
            rows = np.column_stack([
                batch["timestamp_ns"],
                type_names[batch["type_codes"]],
                side_names[batch["side_codes"]],
                batch["order_id"],
                batch["price_ticks"],
                batch["quantity"],
                np.zeros(len(batch["order_id"]), dtype=np.int64),
                batch["bid_price_ticks"],
                batch["ask_price_ticks"],
            ])
            writer.writerows(rows.tolist())
    return destination
