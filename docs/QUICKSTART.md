# Quickstart

## C++ SDK

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHFT_ARCH_NATIVE=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/hft
```

Consume it:

```cmake
find_package(HftSdk REQUIRED)
target_link_libraries(app PRIVATE HftSdk::sdk)
```

A complete external application is in [`examples/consumer`](../examples/consumer).
After installing to a writable prefix:

```bash
cmake --install build --prefix "$PWD/install"
cmake -S examples/consumer -B build-consumer -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build-consumer -j2
./build-consumer/consumer
```

It feeds deterministic events and checks imbalance, rolling retention, counters,
and reset. Add `-DHFT_CONSUMER_HEADER_ONLY=ON` to exercise `HftSdk::core` instead.
Run `./tools/test_consumer.sh` for installation and both consumer modes together.


## Python SDK

```bash
python3 -m pip install .
python3 -m pip install ".[live]"      # websocket adapters
python3 -m pip install ".[pandas]"    # DataFrame helpers
```

```python
import hft_sdk

result = hft_sdk.run_csv("data/sample_ticks.csv", scheme="MTF", horizon=10)
print(result.metrics_dataframe())
print(result.backtest().total_return)
```

## Production feed runtime

```bash
hft_feed --config config/hft_feed.conf --replay data/sample_ticks.csv
curl -s http://127.0.0.1:9100/health
curl -s http://127.0.0.1:9100/metrics
```

## Live crypto perpetuals

```bash
python3 -m pip install ".[live]"
python3 -m hft_sdk live --venue binance --symbol BTCUSDT --scheme MTF
python3 -m hft_sdk live --venue bybit --symbol BTCUSDT
```

