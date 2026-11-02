# Research reproduction and interpretation

The product is a market-data processing SDK with research diagnostics. The
following mapping describes executable methods and their limits. Reproduction
does not imply that a strategy predicts future markets or earns trading profits.

| Method | Implementation | Data / behavior | Interpretation |
| --- | --- | --- | --- |
| Streaming lifetime/update filters | `OBIFilter.hpp`, `SignalPipeline.hpp` | Uses order state observed so far; quantities on NEW/MODIFY are absolute remaining size | Causal approximation; differs from final lifecycle filters |
| Ex-post order filters | `Paper.hpp::build_filter_masks` | Masks use final observed lifecycle and apply to the whole order; IDs must be unique within a replay session | Retrospective diagnostic; contains future lifecycle information |
| Count and volume OBI | `OBIFilter.hpp` | Event-flow counts/weights, not reconstruction of outstanding book depth; rolling buckets have finite time resolution | Cannot be equated with resting-depth imbalance |
| Parent-filtered trade OBI | `Paper.hpp`, `OBIFilter.hpp` | Requires explicit `parent_order_id`; absent public trade linkage remains unfiltered | Public aggregated feeds cannot reproduce parent-filtered trade results |
| Paper sampling | `Paper.hpp::evaluate` | Backward 10-second OBI, forward first-to-last trade return, 15-second anchors; equal-time events grouped | Temporal sampling regression is covered by `test_paper_eval_timing.py` |
| Regime scores | `paper_analysis.py` | 9 OBI and 3 return regimes; correlations across window count vectors | The paper's description is ambiguous; this is the documented interpretation |
| Hawkes diagnostics | `paper_analysis.py::_ct_hawkes_mle` | Continuous-time exponential basis, fixed positive half-lives, nonnegative amplitudes, ridge regularization; objective and convergence reported | Fixed decay grid rather than joint decay optimization; inspect convergence before using results |
| Return transition timing | `paper_analysis.py::_ct_transition_events` | One-second return regimes timestamped at t+1s, when the window is observable | Excitation remains observational; does not prove causal trading alpha |
| Pairs backtest | `pairs_trading.py` | Fees at the observed signal quote, both entry/exit charged, final liquidation, PnL in spread-price units | Same-quote execution is an idealized model; no spread, slippage, borrowing or queue fills |
| Pairs chronological evaluation | `pairs_trading.py --fit-points N` | Fit only on initial N observations, freeze coefficients and trade subsequently | Otherwise full-series OLS is explicitly in-sample; neither mode demonstrates market alpha |
| Low-latency design | `Disruptor.hpp`, `SPSCQueue.hpp`, `src/*bench.cpp` | Separate transport and optimization experiments | Inspired by Bilokon/Gunduz; universal speedups or integrated strategy latency are not reproduced |

Overlapping horizons share observations. The reported Pearson p-values,
ordinary bootstrap intervals and split-half Hawkes comparisons are descriptive.
For formal inference use independent/purged periods, block resampling, multiple
instruments and multiple-testing control. Ex-post masks must never be used as
deployable causal features. Current public crypto feeds use synthetic level IDs;
they are a different dataset from the order-level futures data in the paper.

Historical results predate quantity/linkage, sampling, accounting and estimator
corrections. Re-run the desired experiment and retain its revision, input data
hash, command, build options, raw results and host description together.

Sources: [Anantha/Jain/Maiti, arXiv:2507.22712](https://arxiv.org/abs/2507.22712)
and [Bilokon/Gunduz, arXiv:2309.04259](https://arxiv.org/abs/2309.04259).
The v1 and later paper versions have different wording; the mapping
above documents repository choices rather than silently assuming equivalence.
