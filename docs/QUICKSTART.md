# Quickstart

## C++ SDK

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/hft
```

Consume it:

```cmake
find_package(HftSdk REQUIRED)
target_link_libraries(app PRIVATE HftSdk::sdk)
```

```cpp
#include <hft/HftSdk.hpp>

hft::sdk::OBIFilterEngine engine;
engine.process(events);
auto metrics = engine.metrics();
```

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
