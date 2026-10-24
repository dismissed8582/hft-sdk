# Cointegration Pairs-Trading Backtest

Source: `synthetic(points=5000, seed=7)`

## Strategy

- OLS hedge ratio of `y` on `x`.
- Rolling spread z-score with the configured window.
- Enter long spread at `z <= -entry`, short at `z >= entry`.
- Exit when `|z| <= exit`.
- Per-side transaction cost in bps of gross leg value.

## Results

| Metric | Value |
| --- | --- |
| ADF p-value | 0.000000 |
| Hedge ratio | 2.002073 |
| Trades | 116 |
| Total PnL | 259.218870 |
| Max drawdown | -5.689380 |
| Sharpe (annualized) | 1.986 |
| Win rate | 99.14% |

![Pairs trading equity and z-score](benchmarks/plots/pairs/pairs_equity.png)

## Limitations

- This is a research backtest, not a production execution model.
- Synthetic data is used when no CSV is supplied; it validates the
  implementation rather than demonstrating market alpha.
- The paper's latency optimisation is covered separately by the
  microbenchmark suite; this script focuses on the strategy math.

