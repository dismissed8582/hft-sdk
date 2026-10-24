"""Convenience layer for the HFT LOB SDK.

The compiled extension lives in ``hft_sdk._core``; everything it exposes is
re-exported here so ``import hft_sdk`` is still the only import you need.

Typical use::

    import hft_sdk

    result = hft_sdk.run_csv("data/sample_ticks.csv", scheme="MTF", horizon=10)
    print(result.metrics_dataframe())
    print(result.backtest().total_return)
"""

from __future__ import annotations

import pathlib
from collections.abc import Iterable, Mapping, Sequence
from typing import Any

from . import _core
from . import data as data
from ._core import *  # noqa: F401,F403
from ._core import (
    BacktestConfig,
    BacktestSummary,
    EngineConfig,
    FilterScheme,
    MarketEvent,
    OBIFilterEngine,
    SignalSnapshot,
    SignalWeight,
)

__version__ = _core.__version__

HORIZONS: tuple[int, ...] = (1, 10, 30, 50)

_SCHEMES = {
    "UF": FilterScheme.Unfiltered,
    "UNFILTERED": FilterScheme.Unfiltered,
    "LF": FilterScheme.Lifetime,
    "LIFETIME": FilterScheme.Lifetime,
    "MF": FilterScheme.ModificationCount,
    "MODIFICATIONCOUNT": FilterScheme.ModificationCount,
    "MTF": FilterScheme.ModificationTime,
    "MODIFICATIONTIME": FilterScheme.ModificationTime,
}

SCHEME_CODES = {
    FilterScheme.Unfiltered: "UF",
    FilterScheme.Lifetime: "LF",
    FilterScheme.ModificationCount: "MF",
    FilterScheme.ModificationTime: "MTF",
}

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

OPTIONAL_ARRAY_KEYS = ("parent_order_ids",)


def resolve_scheme(name: str | int | FilterScheme) -> FilterScheme:
    """Accept ``"UF"``/``"MTF"``, the enum, or its integer value."""
    if isinstance(name, FilterScheme):
        return name
    if isinstance(name, int):
        return FilterScheme(name)
    try:
        return _SCHEMES[str(name).upper()]
    except KeyError as error:
        raise ValueError(
            f"unknown scheme {name!r}; expected one of UF, LF, MF, MTF"
        ) from error


def scheme_code(scheme: str | int | FilterScheme) -> str:
    """Return the short code (``"UF"``/``"LF"``/``"MF"``/``"MTF"``)."""
    return SCHEME_CODES[resolve_scheme(scheme)]


def resolve_horizon(seconds: int | str) -> int:
    """Map a horizon in seconds (1, 10, 30, 50) to the engine's index."""
    value = int(seconds)
    try:
        return HORIZONS.index(value)
    except ValueError as error:
        raise ValueError(
            f"unsupported horizon {value}; expected one of {HORIZONS}"
        ) from error


def load_csv(path: str) -> dict[str, Any]:
    """Load a tick CSV into the NumPy arrays expected by ``process_arrays``."""
    return _core.read_csv(str(path))


class RunResult:
    """Engine plus the signal/backtest settings used for this run."""

    def __init__(
        self,
        engine: OBIFilterEngine,
        scheme: FilterScheme = FilterScheme.Unfiltered,
        horizon: int = 0,
        volume: bool = True,
    ) -> None:
        self.engine = engine
        self.scheme = scheme
        self.horizon = horizon
        self.volume = volume

    @property
    def metrics(self) -> list[Any]:
        return self.engine.metrics()

    @property
    def metrics_table(self) -> dict[str, Any]:
        return self.engine.metrics_table()

    def metrics_dataframe(self):
        """Metrics as a pandas DataFrame (requires pandas)."""
        import pandas as pd

        return pd.DataFrame(self.engine.metrics_table())

    @property
    def snapshots(self) -> list[SignalSnapshot]:
        return self.engine.snapshots()

    def regimes(
        self,
        scheme: str | int | FilterScheme | None = None,
        horizon: int | str | None = None,
        volume: bool | None = None,
    ):
        scheme_value = self.scheme if scheme is None else resolve_scheme(scheme)
        horizon_value = self.horizon if horizon is None else resolve_horizon(horizon)
        volume_value = self.volume if volume is None else volume
        return self.engine.regimes(scheme_value, horizon_value, volume_value)

    def backtest(self, config: BacktestConfig | None = None) -> BacktestSummary:
        """Run the PnL backtest using this run's scheme/horizon by default."""
        if config is None:
            config = BacktestConfig()
            config.scheme = self.scheme
            config.horizon = self.horizon
            config.weight = (
                SignalWeight.Volume if self.volume else SignalWeight.Count
            )
        return self.engine.backtest(config)

    def reset(self) -> None:
        self.engine.reset()


def run(
    events: str | Mapping[str, Any] | Sequence[MarketEvent],
    *,
    config: EngineConfig | None = None,
    scheme: str | int | FilterScheme = "UF",
    horizon: int | str = 1,
    volume: bool = True,
) -> RunResult:
    """Process events from a CSV path, an array dict, or ``MarketEvent`` list."""
    engine = OBIFilterEngine(config) if config is not None else OBIFilterEngine()

    if isinstance(events, str):
        arrays = load_csv(events)
    elif isinstance(events, Mapping):
        arrays = events
    else:
        engine.process(list(events))
        arrays = None

    if arrays is not None:
        missing = [key for key in ARRAY_KEYS if key not in arrays]
        if missing:
            raise ValueError(f"event arrays are missing: {', '.join(missing)}")
        kwargs = {key: arrays[key] for key in ARRAY_KEYS}
        for key in OPTIONAL_ARRAY_KEYS:
            if key in arrays:
                kwargs[key] = arrays[key]
        engine.process_arrays(**kwargs)

    return RunResult(
        engine,
        scheme=resolve_scheme(scheme),
        horizon=resolve_horizon(horizon),
        volume=volume,
    )


def run_csv(
    path: str,
    *,
    config: EngineConfig | None = None,
    scheme: str | int | FilterScheme = "UF",
    horizon: int | str = 1,
    volume: bool = True,
) -> RunResult:
    """Load a tick CSV, run the signal engine, and return a ``RunResult``."""
    return run(
        path, config=config, scheme=scheme, horizon=horizon, volume=volume
    )


def run_sources(
    sources: Iterable[str | pathlib.Path],
    *,
    config: EngineConfig | None = None,
    scheme: str | int | FilterScheme = "UF",
    horizon: int | str = 1,
    volume: bool = True,
    table: str | None = None,
    price_scale: float = 100.0,
    quantity_scale: float = 1e8,
    asset: str | None = None,
    exchange: str | None = None,
    batch_size: int = 131_072,
) -> RunResult:
    """Run one or more local files/URLs through a single engine instance."""
    engine = OBIFilterEngine(config) if config is not None else OBIFilterEngine()
    for source in sources:
        path = data.resolve_source(source)
        for batch in data.iter_tick_arrays(
            path,
            table=table,
            price_scale=price_scale,
            quantity_scale=quantity_scale,
            asset=asset,
            exchange=exchange,
            batch_size=batch_size,
        ):
            engine.process_arrays(**batch)
    return RunResult(
        engine,
        scheme=resolve_scheme(scheme),
        horizon=resolve_horizon(horizon),
        volume=volume,
    )


def run_source(
    source: str | pathlib.Path,
    **kwargs: Any,
) -> RunResult:
    """Run a single file or URL; downloads URLs into the local cache first."""
    return run_sources([source], **kwargs)


def run_binance(
    symbol: str,
    start: str,
    end: str | None = None,
    *,
    market: str = "spot",
    kind: str = "trades",
    monthly: bool = False,
    out_dir: str | pathlib.Path | None = None,
    force: bool = False,
    jobs: int = 4,
    price_scale: float = 100.0,
    quantity_scale: float = 1e8,
    config: EngineConfig | None = None,
    scheme: str | int | FilterScheme = "UF",
    horizon: int | str = 1,
    volume: bool = True,
) -> RunResult:
    """Download Binance archives and run the engine on them."""
    paths = data.fetch_binance(
        symbol, start, end, market=market, kind=kind, monthly=monthly,
        out_dir=out_dir, force=force, jobs=jobs,
    )
    return run_sources(
        paths, config=config, scheme=scheme, horizon=horizon, volume=volume,
        price_scale=price_scale, quantity_scale=quantity_scale,
    )


def run_crypto_lob(
    symbol: str,
    period: str,
    *,
    table: str = "depth",
    exchange: str = "binance",
    out_dir: str | pathlib.Path | None = None,
    force: bool = False,
    price_scale: float = 100.0,
    quantity_scale: float = 1e8,
    asset: str | None = None,
    config: EngineConfig | None = None,
    scheme: str | int | FilterScheme = "UF",
    horizon: int | str = 1,
    volume: bool = True,
) -> RunResult:
    """Download one crypto-lob-stream shard and run the engine on it."""
    path = data.fetch_crypto_lob(
        symbol, period, table=table, exchange=exchange,
        out_dir=out_dir, force=force,
    )
    return run_source(
        path, config=config, scheme=scheme, horizon=horizon, volume=volume,
        table=table, price_scale=price_scale,
        quantity_scale=quantity_scale, asset=symbol if asset is None else asset,
    )


download = data.download
fetch = data.fetch
fetch_binance = data.fetch_binance
fetch_crypto_lob = data.fetch_crypto_lob
resolve_source = data.resolve_source
iter_tick_arrays = data.iter_tick_arrays
to_tick_csv = data.to_tick_csv
detect_source = data.detect_source
default_data_dir = data.default_data_dir

__all__ = [
    "ARRAY_KEYS",
    "HORIZONS",
    "OPTIONAL_ARRAY_KEYS",
    "BacktestConfig",
    "BacktestSummary",
    "EngineConfig",
    "FilterScheme",
    "MarketEvent",
    "OBIFilterEngine",
    "RunResult",
    "SignalSnapshot",
    "SCHEME_CODES",
    "data",
    "default_data_dir",
    "detect_source",
    "download",
    "fetch",
    "fetch_binance",
    "fetch_crypto_lob",
    "iter_tick_arrays",
    "load_csv",
    "resolve_horizon",
    "resolve_scheme",
    "resolve_source",
    "run",
    "run_binance",
    "run_csv",
    "run_crypto_lob",
    "run_source",
    "run_sources",
    "scheme_code",
    "to_tick_csv",
]
