#!/usr/bin/env python3
"""Paper-faithful diagnostics for the HFT LOB SDK.

The input is the observation CSV produced by ``hft_paper_eval``. The script
implements the three-layer ladder from arXiv:2507.22712 as closely as the
paper's description allows:

1. Pearson association with realized traded returns (raw and ARMA-residualized).
2. 9x3 discretized regime scores with the directional weight matrix
   ``W_ij = 1 + b_i a_j`` and the regression score.
3. A sum-of-exponentials multivariate Hawkes fit with integrated-kernel norms.

Where the paper is ambiguous or internally inconsistent, the choices are
documented in the generated report.
"""

from __future__ import annotations

import argparse
import json
import math
import warnings
from pathlib import Path

import numpy as np
import pandas as pd

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import seaborn as sns
except ImportError:  # pragma: no cover
    plt = None
    sns = None

try:
    from statsmodels.tsa.arima.model import ARIMA
except ImportError:  # pragma: no cover - optional dependency
    ARIMA = None

try:
    from scipy.stats import pearsonr
except ImportError:  # pragma: no cover
    pearsonr = None

SCHEMES = ["UF", "LF", "MF", "MTF"]
OBI_REGIMES = 9
RETURN_REGIMES = 3
PEARSON_HORIZONS = [1, 10, 30, 50, 80, 100]
REGIME_HORIZONS = [1, 10, 20]
DARK_BACKGROUND = "#0d1117"
DARK_AXES = "#161b22"
DARK_GRID = "#30363d"
DARK_TEXT = "#e6edf3"
ACCENTS = ["#58a6ff", "#3fb950", "#f0883e", "#f85149"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--ticks-csv",
        type=Path,
        default=Path("benchmarks/results/paper/paper_ticks.csv"),
    )
    parser.add_argument("--output-dir", type=Path, default=Path("benchmarks/results/paper"))
    parser.add_argument("--plots-dir", type=Path, default=Path("benchmarks/plots/paper"))
    parser.add_argument("--report", type=Path, default=Path("benchmarks/results/paper/PAPER_REPORT.md"))
    parser.add_argument("--obi-window-ns", type=int, default=10_000_000_000)
    parser.add_argument("--gamma", type=float, default=0.2)
    parser.add_argument("--regime-lags", type=int, default=8)
    parser.add_argument("--bootstrap-samples", type=int, default=200)
    parser.add_argument("--train-fraction", type=float, default=0.7)
    parser.add_argument("--hawkes-estimator", choices=["ct-mle", "poisson-basis"],
                        default="ct-mle",
                        help="continuous-time sum-of-exponentials MLE or discrete Poisson basis")
    parser.add_argument("--hawkes-lags", type=int, default=16)
    parser.add_argument("--hawkes-half-lives", default="1,4,16",
                        help="exponential half-lives in anchor steps")
    parser.add_argument("--hawkes-ridge", type=float, default=1e-6)
    parser.add_argument("--stabilize", action="store_true",
                        help="rescale the Hawkes kernel to spectral radius 0.99")
    parser.add_argument("--dataset", default="data/sample_ticks.csv")
    return parser.parse_args()


def arma_residuals(values: np.ndarray) -> np.ndarray:
    values = np.asarray(values, dtype=float)
    if values.size < 12 or not np.isfinite(values).all() or np.nanstd(values) == 0.0:
        return values - np.nanmean(values)
    if ARIMA is not None:
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                fitted = ARIMA(
                    values, order=(1, 0, 1),
                    enforce_stationarity=False,
                    enforce_invertibility=False,
                ).fit()
            residuals = np.asarray(fitted.resid, dtype=float)
            if residuals.size == values.size and np.isfinite(residuals).all():
                return residuals
        except Exception:  # noqa: BLE001 - fall back to AR(1)
            pass
    lagged = np.corrcoef(values[:-1], values[1:])[0, 1]
    phi = float(lagged) if np.isfinite(lagged) else 1.0
    residuals = np.zeros_like(values)
    residuals[1:] = values[1:] - phi * values[:-1]
    return residuals


def correlation_matrix(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    """Correlation between every column of right and every column of left."""
    left = np.asarray(left, dtype=float)
    right = np.asarray(right, dtype=float)
    if left.shape[0] < 3:
        return np.zeros((right.shape[1], left.shape[1]))
    left = left - left.mean(axis=0)
    right = right - right.mean(axis=0)
    left_norm = np.sqrt(np.sum(left * left, axis=0))
    right_norm = np.sqrt(np.sum(right * right, axis=0))
    denominator = np.outer(right_norm, left_norm)
    numerator = right.T @ left
    result = np.divide(numerator, denominator, out=np.zeros_like(numerator),
                       where=denominator > 0.0)
    return result


def regression_r2(design: np.ndarray, target: np.ndarray) -> float:
    design = np.asarray(design, dtype=float)
    target = np.asarray(target, dtype=float)
    if design.shape[0] < design.shape[1] + 2:
        return float("nan")
    augmented = np.column_stack([np.ones(design.shape[0]), design])
    coefficients, *_ = np.linalg.lstsq(augmented, target, rcond=None)
    prediction = augmented @ coefficients
    ss_res = np.sum((target - prediction) ** 2, axis=0)
    ss_tot = np.sum((target - target.mean(axis=0)) ** 2, axis=0)
    r2 = np.divide(ss_res, ss_tot, out=np.ones_like(ss_res), where=ss_tot > 0.0)
    return float(np.mean(1.0 - r2))


def directional_weights(gamma: float) -> np.ndarray:
    # Return rows: negative, neutral, positive. OBI columns: negative -> positive.
    a = np.arange(OBI_REGIMES) - (OBI_REGIMES - 1) / 2.0
    b = np.array([-gamma, 0.0, gamma])
    return 1.0 + np.outer(b, a)


def load_ticks(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise SystemExit(f"missing paper observation file: {path}")
    frame = pd.read_csv(path)
    required = {
        "timestamp_ns", "scheme", "is_anchor", "lob_obi", "lob_valid",
        "trade_obi", "trade_valid", "obi_regime", "trade_obi_regime",
        "return_regime_1s", "return_regime_10s", "return_regime_20s",
    }
    missing = required - set(frame.columns)
    if missing:
        raise SystemExit(f"paper observation file is missing columns: {sorted(missing)}")
    for horizon in PEARSON_HORIZONS:
        for column in (f"ret_{horizon}s", f"ret_{horizon}s_valid"):
            if column not in frame.columns:
                raise SystemExit(f"paper observation file is missing {column}")
    frame["scheme"] = pd.Categorical(frame["scheme"], categories=SCHEMES, ordered=True)
    return frame


def _pearson_slice(x: np.ndarray, y: np.ndarray) -> dict[str, float]:
    if x.size < 8 or np.std(x) == 0.0 or np.std(y) == 0.0:
        return {"pearson": float("nan"), "r2": float("nan"), "p": float("nan"),
                "ci_low": float("nan"), "ci_high": float("nan")}
    raw = float(np.corrcoef(x, y)[0, 1])
    x_residual = arma_residuals(x)
    y_residual = arma_residuals(y)
    residual = float(np.corrcoef(x_residual, y_residual)[0, 1])
    p_value = float(pearsonr(x, y).pvalue) if pearsonr is not None else float("nan")
    p_residual = (
        float(pearsonr(x_residual, y_residual).pvalue)
        if pearsonr is not None else float("nan")
    )

    def fisher_ci(value: float) -> tuple[float, float]:
        if not np.isfinite(value) or x.size <= 3:
            return float("nan"), float("nan")
        z = math.atanh(max(-0.999999, min(0.999999, value)))
        half = 1.96 / math.sqrt(x.size - 3)
        return math.tanh(z - half), math.tanh(z + half)

    low, high = fisher_ci(raw)
    residual_low, residual_high = fisher_ci(residual)
    return {"pearson": raw, "r2": raw * raw, "p": p_value,
            "ci_low": low, "ci_high": high,
            "residual": residual, "residual_r2": residual * residual,
            "p_residual": p_residual,
            "residual_ci_low": residual_low, "residual_ci_high": residual_high}


def pearson_scores(frame: pd.DataFrame, train_fraction: float = 0.7) -> pd.DataFrame:
    rows: list[dict] = []
    for scheme in SCHEMES:
        subset = frame[(frame["scheme"] == scheme) & (frame["is_anchor"] == 1)]
        subset = subset.sort_values("timestamp_ns")
        if subset.empty:
            continue
        split = int(round(len(subset) * train_fraction))
        for horizon in PEARSON_HORIZONS:
            value_column = f"ret_{horizon}s"
            valid_column = f"ret_{horizon}s_valid"
            valid = subset[subset[valid_column] == 1]
            if len(valid) < 8:
                continue
            x = valid["lob_obi"].to_numpy(dtype=float)
            y = valid[value_column].to_numpy(dtype=float)
            finite = np.isfinite(x) & np.isfinite(y)
            x = x[finite]
            y = y[finite]
            if x.size < 8:
                continue
            full = _pearson_slice(x, y)
            split = max(8, min(split, x.size - 8))
            train = _pearson_slice(x[:split], y[:split]) if split >= 8 else {}
            test = _pearson_slice(x[split:], y[split:]) if x.size - split >= 8 else {}
            rows.append({
                "scheme": scheme,
                "horizon_s": horizon,
                "samples": int(x.size),
                "pearson_raw": full["pearson"],
                "r2_raw": full["r2"],
                "p_raw": full["p"],
                "ci_raw_low": full["ci_low"],
                "ci_raw_high": full["ci_high"],
                "pearson_residual": full.get("residual", float("nan")),
                "r2_residual": full.get("residual_r2", float("nan")),
                "p_residual": full.get("p_residual", float("nan")),
                "ci_residual_low": full.get("residual_ci_low", float("nan")),
                "ci_residual_high": full.get("residual_ci_high", float("nan")),
                "pearson_train": train.get("pearson", float("nan")),
                "pearson_test": test.get("pearson", float("nan")),
                "pearson_residual_train": train.get("residual", float("nan")),
                "pearson_residual_test": test.get("residual", float("nan")),
            })
    return pd.DataFrame(rows)


def window_vectors(frame: pd.DataFrame, scheme: str, horizon: int,
                   obi_window_ns: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    subset = frame[frame["scheme"] == scheme]
    anchors = np.sort(subset.loc[subset["is_anchor"] == 1, "timestamp_ns"].unique())
    q_rows: list[np.ndarray] = []
    r_rows: list[np.ndarray] = []
    kept_anchors: list[int] = []
    valid_column = f"ret_{horizon}s_valid"
    regime_column = f"return_regime_{horizon}s"
    if regime_column not in subset.columns:
        return np.empty((0, OBI_REGIMES)), np.empty((0, RETURN_REGIMES)), np.empty(0)
    for anchor in anchors:
        window = subset[(subset["timestamp_ns"] > anchor - obi_window_ns) &
                        (subset["timestamp_ns"] <= anchor)]
        window = window[(window["lob_valid"] == 1) & (window[valid_column] == 1)]
        if window.empty:
            continue
        q_rows.append(np.bincount(window["obi_regime"].to_numpy(dtype=int),
                                  minlength=OBI_REGIMES).astype(float))
        r_rows.append(np.bincount(window[regime_column].to_numpy(dtype=int),
                                  minlength=RETURN_REGIMES).astype(float))
        kept_anchors.append(int(anchor))
    return (np.vstack(q_rows) if q_rows else np.empty((0, OBI_REGIMES)),
            np.vstack(r_rows) if r_rows else np.empty((0, RETURN_REGIMES)),
            np.asarray(kept_anchors))


def _bootstrap_score(source: np.ndarray, target: np.ndarray,
                     weights: np.ndarray, samples: int,
                     rng: np.random.Generator) -> tuple[float, float]:
    if samples <= 0 or source.shape[0] < 5:
        return float("nan"), float("nan")
    rho_values = []
    r2_values = []
    for _ in range(samples):
        indices = rng.integers(0, source.shape[0], source.shape[0])
        rho = correlation_matrix(source[indices], target[indices])
        rho_values.append(float(np.sum(weights * rho)))
        r2_values.append(regression_r2(source[indices], target[indices]))
    rho_values = np.asarray(rho_values, dtype=float)
    r2_values = np.asarray(r2_values, dtype=float)
    rho_values = rho_values[np.isfinite(rho_values)]
    r2_values = r2_values[np.isfinite(r2_values)]
    rho_ci = (float(np.percentile(rho_values, 2.5)),
              float(np.percentile(rho_values, 97.5))) if rho_values.size else (float("nan"), float("nan"))
    r2_ci = (float(np.percentile(r2_values, 2.5)),
             float(np.percentile(r2_values, 97.5))) if r2_values.size else (float("nan"), float("nan"))
    return rho_ci, r2_ci


def regime_scores(frame: pd.DataFrame, args: argparse.Namespace) -> tuple[pd.DataFrame, pd.DataFrame]:
    weights = directional_weights(args.gamma)
    score_rows: list[dict] = []
    matrix_rows: list[dict] = []
    rng = np.random.default_rng(12345)
    for scheme in SCHEMES:
        for horizon in REGIME_HORIZONS:
            q, r, _ = window_vectors(frame, scheme, horizon, args.obi_window_ns)
            if q.shape[0] < 8:
                continue
            split = max(5, min(int(round(q.shape[0] * args.train_fraction)),
                               q.shape[0] - 5))
            for variant in ("raw", "residual"):
                q_variant = q
                r_variant = r
                if variant == "residual":
                    q_variant = np.column_stack([arma_residuals(q[:, index])
                                                 for index in range(q.shape[1])])
                    r_variant = np.column_stack([arma_residuals(r[:, index])
                                                 for index in range(r.shape[1])])
                slices = [("full", q_variant, r_variant)]
                if variant == "raw":
                    slices.extend([
                        ("train", q_variant[:split], r_variant[:split]),
                        ("test", q_variant[split:], r_variant[split:]),
                    ])
                for sample_name, source_full, target_full in slices:
                    max_lag = args.regime_lags if sample_name == "full" else 0
                    for lag in range(max_lag + 1):
                        if lag >= target_full.shape[0] - 3:
                            break
                        target = target_full[lag:]
                        source = source_full[: target_full.shape[0] - lag]
                        if source.shape[0] < 4:
                            continue
                        rho = correlation_matrix(source, target)
                        directional = float(np.sum(weights * rho))
                        r2 = regression_r2(source, target)
                        if sample_name == "full" and lag == 0:
                            rho_ci, r2_ci = _bootstrap_score(
                                source, target, weights, args.bootstrap_samples, rng)
                        else:
                            rho_ci = (float("nan"), float("nan"))
                            r2_ci = (float("nan"), float("nan"))
                        score_rows.append({
                            "scheme": scheme,
                            "horizon_s": horizon,
                            "variant": variant,
                            "sample": sample_name,
                            "lag": lag,
                            "windows": int(source.shape[0]),
                            "S_rho": directional,
                            "S_rho_ci_low": rho_ci[0],
                            "S_rho_ci_high": rho_ci[1],
                            "S_R": r2,
                            "S_R_ci_low": r2_ci[0],
                            "S_R_ci_high": r2_ci[1],
                        })
                        if lag == 0 and sample_name == "full":
                            for return_index in range(RETURN_REGIMES):
                                for obi_index in range(OBI_REGIMES):
                                    matrix_rows.append({
                                        "scheme": scheme,
                                        "horizon_s": horizon,
                                        "variant": variant,
                                        "return_regime": return_index,
                                        "obi_regime": obi_index,
                                        "correlation": float(rho[return_index, obi_index]),
                                    })
    return pd.DataFrame(score_rows), pd.DataFrame(matrix_rows)


def _kernel_basis(lags: int, half_lives: list[float]) -> np.ndarray:
    lag_index = np.arange(1, lags + 1, dtype=float)
    basis = []
    for half_life in half_lives:
        decay = math.log(2.0) / max(half_life, 1e-9)
        weights = (1.0 - math.exp(-decay)) * np.exp(-decay * (lag_index - 1.0))
        basis.append(weights)
    return np.vstack(basis)  # basis x lags


def _convolve(events: np.ndarray, basis: np.ndarray) -> np.ndarray:
    lags = basis.shape[1]
    rows = events.shape[0]
    result = np.empty((rows, events.shape[1] * basis.shape[0]), dtype=float)
    for source in range(events.shape[1]):
        for basis_index in range(basis.shape[0]):
            weights = basis[basis_index]
            column = np.zeros(rows, dtype=float)
            for lag in range(1, lags + 1):
                column[lag:] += weights[lag - 1] * events[:-lag, source]
            result[:, source * basis.shape[0] + basis_index] = column
    return result


def _poisson_fit(design: np.ndarray, target: np.ndarray, ridge: float) -> np.ndarray:
    from scipy.optimize import minimize

    augmented = np.column_stack([np.ones(design.shape[0]), design])
    target = np.asarray(target, dtype=float)

    def objective(coefficients: np.ndarray) -> tuple[float, np.ndarray]:
        intensity = augmented @ coefficients
        intensity = np.clip(intensity, 1e-9, None)
        negative_log_likelihood = float(np.sum(intensity - target * np.log(intensity)))
        penalty = 0.5 * ridge * float(np.sum(coefficients[1:] ** 2))
        gradient = np.empty_like(coefficients)
        residual = 1.0 - target / intensity
        gradient[0] = float(np.sum(residual))
        gradient[1:] = augmented[:, 1:].T @ residual + ridge * coefficients[1:]
        return negative_log_likelihood + penalty, gradient

    start = np.full(augmented.shape[1], 1e-3)
    start[0] = max(float(target.mean()), 1e-6)
    result = minimize(
        objective,
        start,
        method="L-BFGS-B",
        jac=True,
        bounds=[(0.0, None)] * augmented.shape[1],
        options={"maxiter": 2000, "ftol": 1e-12},
    )
    if not result.success:
        # Robust fallback: nonnegative least squares on the same design.
        from scipy.optimize import lsq_linear

        fitted = lsq_linear(augmented, target, bounds=(0.0, np.inf)).x
        return fitted
    return np.asarray(result.x, dtype=float)


def _ct_transition_events(data: pd.DataFrame, regime_column: str) -> list[tuple[float, int]]:
    times = data["timestamp_ns"].to_numpy(dtype=float) / 1e9
    regimes = data[regime_column].to_numpy(dtype=int)
    returns = data["return_regime_1s"].to_numpy(dtype=int)
    events: list[tuple[float, int]] = []
    previous_obi: int | None = None
    previous_return: int | None = None
    for timestamp, obi_regime, return_regime in zip(times, regimes, returns):
        if previous_obi is None or obi_regime != previous_obi:
            events.append((float(timestamp), int(obi_regime)))
        if previous_return is None or return_regime != previous_return:
            events.append((float(timestamp), OBI_REGIMES + int(return_regime)))
        previous_obi = int(obi_regime)
        previous_return = int(return_regime)
    return events


def _ct_hawkes_mle(events: list[tuple[float, int]], state_count: int,
                   half_lives: list[float], ridge: float) -> dict | None:
    if len(events) <= state_count * 3:
        return None
    events = sorted(events, key=lambda item: item[0])
    times = np.asarray([item[0] for item in events], dtype=float)
    states = np.asarray([item[1] for item in events], dtype=int)
    origin = times[0]
    times = times - origin
    total_time = float(times[-1])
    if total_time <= 0.0:
        return None

    decay_rates = np.log(2.0) / np.maximum(np.asarray(half_lives, dtype=float), 1e-9)
    count = len(decay_rates)
    decay_matrix = decay_rates.reshape(1, -1)
    running = np.zeros((state_count, count), dtype=float)
    feature_rows: list[np.ndarray] = []
    targets: list[int] = []
    index = 0
    previous_time = 0.0
    while index < times.size:
        timestamp = float(times[index])
        running *= np.exp(-decay_matrix * (timestamp - previous_time))
        group: list[int] = []
        while index < times.size and float(times[index]) == timestamp:
            group.append(int(states[index]))
            index += 1
        for state in group:
            feature_rows.append((running * decay_matrix).reshape(-1))
            targets.append(state)
        for state in group:
            running[state, :] += 1.0
        previous_time = timestamp

    features = np.vstack(feature_rows)
    targets_array = np.asarray(targets, dtype=int)
    integral_terms = np.zeros((state_count, count), dtype=float)
    for state, timestamp in zip(states, times):
        integral_terms[state, :] += 1.0 - np.exp(-decay_rates * (total_time - timestamp))
    integral_flat = np.tile(integral_terms.reshape(-1), (state_count, 1))

    # Poisson multiplicative updates (EM/MM) for the nonnegative amplitudes.
    # For each target state this maximises the continuous-time likelihood with
    # exposure T for the baseline and the integrated exponential basis for the
    # kernel amplitudes.
    baseline_exposure = total_time
    kernel_exposure = integral_terms.reshape(-1)
    exposure = np.concatenate([[baseline_exposure], kernel_exposure])
    event_counts = np.bincount(targets_array, minlength=state_count)
    alpha_full = np.zeros((state_count, state_count, count), dtype=float)
    converged = True
    iterations = 0
    for target in range(state_count):
        mask = targets_array == target
        design = np.column_stack([np.ones(int(mask.sum())), features[mask]])
        beta = np.full(design.shape[1], 0.01, dtype=float)
        beta[0] = max(event_counts[target] / max(total_time, 1e-9), 1e-6)
        previous = float("inf")
        for iteration in range(2000):
            intensity = np.clip(design @ beta, 1e-12, None)
            negative = float(-np.sum(np.log(intensity)) + beta @ exposure)
            if not np.isfinite(negative):
                converged = False
                break
            ratio = (design.T @ (1.0 / intensity)) / np.maximum(exposure, 1e-12)
            updated = np.maximum(beta * ratio, 1e-12)
            # Damped step if the MM update violates monotonicity.
            candidate_intensity = np.clip(design @ updated, 1e-12, None)
            candidate = float(-np.sum(np.log(candidate_intensity)) +
                              updated @ exposure)
            if candidate > negative:
                updated = np.sqrt(beta * updated)
                candidate_intensity = np.clip(design @ updated, 1e-12, None)
                candidate = float(-np.sum(np.log(candidate_intensity)) +
                                  updated @ exposure)
            beta = updated
            iterations = max(iterations, iteration + 1)
            if abs(previous - candidate) <= 1e-10 * max(1.0, abs(previous)):
                previous = candidate
                break
            previous = candidate
        else:
            converged = False
        alpha_full[target] = beta[1:].reshape(state_count, count)
    kernel = alpha_full.sum(axis=2)
    return {
        "kernel": kernel,
        "success": bool(converged),
        "events": int(targets_array.size),
        "objective": float(previous) if np.isfinite(previous) else float("nan"),
        "iterations": int(iterations),
    }


def _kernel_summary(kernel: np.ndarray,
                    aligned_pairs: list[tuple[int, int]]) -> dict[str, float]:
    raw_radius = float(np.max(np.abs(np.linalg.eigvals(kernel))))
    cross = kernel[OBI_REGIMES:, :OBI_REGIMES]
    aligned_norm = float(np.sqrt(sum(cross[ret, obi] ** 2
                                     for ret, obi in aligned_pairs)))
    aligned_sum = float(sum(cross[ret, obi] for ret, obi in aligned_pairs))
    return {
        "spectral_radius": raw_radius,
        "aligned_norm": aligned_norm,
        "aligned_sum": aligned_sum,
    }


def hawkes_analysis(frame: pd.DataFrame, args: argparse.Namespace) -> dict | None:
    half_lives = [float(value) for value in args.hawkes_half_lives.split(",") if value]
    if not half_lives:
        return None
    basis = _kernel_basis(args.hawkes_lags, half_lives)
    state_count = OBI_REGIMES + RETURN_REGIMES
    results: dict[str, dict] = {}
    aligned_pairs = ([(0, obi) for obi in range(0, 4)] +
                     [(1, 4)] +
                     [(2, obi) for obi in range(5, 9)])
    signals = (
        ("lob", "lob_valid", "obi_regime"),
        ("trade", "trade_valid", "trade_obi_regime"),
    )
    for signal_name, valid_column, regime_column in signals:
        if args.hawkes_estimator == "ct-mle":
            subset = frame[(frame[valid_column] == 1) &
                           (frame["ret_1s_valid"] == 1)]
        else:
            subset = frame[(frame["is_anchor"] == 1) & (frame[valid_column] == 1) &
                           (frame["ret_1s_valid"] == 1)]
        if subset.empty:
            continue
        for scheme in SCHEMES:
            data = subset[subset["scheme"] == scheme]
            if len(data) <= args.hawkes_lags + 8:
                continue
            estimator = args.hawkes_estimator
            if estimator == "ct-mle":
                fit = _ct_hawkes_mle(
                    _ct_transition_events(data, regime_column), state_count,
                    half_lives, args.hawkes_ridge)
                if fit is None:
                    continue
                kernels = fit["kernel"]
                samples = fit["events"]
                fit_success = fit["success"]
            else:
                events = np.zeros((len(data), state_count), dtype=float)
                indices = np.arange(len(data))
                events[indices, data[regime_column].to_numpy(dtype=int)] = 1.0
                events[indices, OBI_REGIMES + data["return_regime_1s"].to_numpy(dtype=int)] = 1.0
                features = _convolve(events, basis)
                kernels = np.zeros((state_count, state_count), dtype=float)
                for target in range(state_count):
                    coefficients = _poisson_fit(features, events[:, target], args.hawkes_ridge)
                    amplitudes = coefficients[1:].reshape(state_count, len(half_lives))
                    kernels[target, :] = amplitudes.sum(axis=1)
                samples = int(len(data))
                fit_success = True

            summary = _kernel_summary(kernels, aligned_pairs)
            raw_radius = summary["spectral_radius"]
            stabilized = False
            reported = kernels
            if args.stabilize and raw_radius > 0.99:
                reported = kernels * (0.99 / raw_radius)
                stabilized = True
            reported_summary = _kernel_summary(reported, aligned_pairs)
            cross = reported[OBI_REGIMES:, :OBI_REGIMES]
            stability = None
            if estimator == "ct-mle" and len(data) >= 40:
                split_index = max(20, min(int(len(data) * args.train_fraction),
                                          len(data) - 20))
                halves = {}
                for half_name, part in (
                    ("train", data.iloc[:split_index]),
                    ("test", data.iloc[split_index:]),
                ):
                    half_fit = _ct_hawkes_mle(
                        _ct_transition_events(part, regime_column), state_count,
                        half_lives, args.hawkes_ridge)
                    if half_fit is not None:
                        halves[half_name] = _kernel_summary(
                            half_fit["kernel"], aligned_pairs)
                if "train" in halves and "test" in halves:
                    stability = {
                        "train_aligned_norm": halves["train"]["aligned_norm"],
                        "test_aligned_norm": halves["test"]["aligned_norm"],
                        "delta_aligned_norm": (halves["test"]["aligned_norm"] -
                                               halves["train"]["aligned_norm"]),
                        "train_radius": halves["train"]["spectral_radius"],
                        "test_radius": halves["test"]["spectral_radius"],
                    }
            results[f"{scheme}/{signal_name}"] = {
                "scheme": scheme,
                "signal": signal_name,
                "estimator": estimator,
                "fit_success": fit_success,
                "kernel": reported,
                "cross": cross,
                "samples": samples,
                "spectral_radius": raw_radius,
                "reported_spectral_radius": reported_summary["spectral_radius"],
                "stabilized": stabilized,
                "aligned_norm": reported_summary["aligned_norm"],
                "aligned_sum": reported_summary["aligned_sum"],
                "stability": stability,
            }
    return results or None


def plot_heatmap(matrix: np.ndarray, path: Path, title: str, xlabel: str,
                 ylabel: str, xticklabels: list[str], yticklabels: list[str]) -> None:
    if plt is None or sns is None:
        return
    sns.set_theme(style="darkgrid", context="talk")
    plt.rcParams.update({
        "figure.facecolor": DARK_BACKGROUND,
        "axes.facecolor": DARK_AXES,
        "axes.edgecolor": DARK_GRID,
        "axes.labelcolor": DARK_TEXT,
        "text.color": DARK_TEXT,
        "xtick.color": DARK_TEXT,
        "ytick.color": DARK_TEXT,
        "savefig.facecolor": DARK_BACKGROUND,
    })
    figure, axis = plt.subplots(figsize=(10, 8))
    sns.heatmap(matrix, ax=axis, cmap="vlag", center=0.0, annot=False,
                xticklabels=xticklabels, yticklabels=yticklabels,
                cbar_kws={"label": "Coefficient"})
    axis.set_title(title)
    axis.set_xlabel(xlabel)
    axis.set_ylabel(ylabel)
    figure.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=180)
    plt.close(figure)


def write_outputs(args: argparse.Namespace, frame: pd.DataFrame,
                  pearson: pd.DataFrame, regime: pd.DataFrame,
                  matrices: pd.DataFrame, hawkes: dict | None) -> None:
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.plots_dir.mkdir(parents=True, exist_ok=True)
    pearson.to_csv(args.output_dir / "pearson_scores.csv", index=False)
    regime.to_csv(args.output_dir / "regime_scores.csv", index=False)
    matrices.to_csv(args.output_dir / "regime_correlation.csv", index=False)

    if hawkes:
        for key, result in hawkes.items():
            scheme = result["scheme"]
            signal = result["signal"]
            stem = f"{scheme}_{signal}"
            labels = [f"OBI_{index}" for index in range(OBI_REGIMES)] + [
                f"RET_{index}" for index in range(RETURN_REGIMES)
            ]
            pd.DataFrame(result["kernel"], index=labels, columns=labels).to_csv(
                args.output_dir / f"hawkes_kernel_{stem}.csv")
            pd.DataFrame(result["cross"],
                         index=[f"RET_{index}" for index in range(RETURN_REGIMES)],
                         columns=[f"OBI_{index}" for index in range(OBI_REGIMES)]).to_csv(
                args.output_dir / f"hawkes_cross_{stem}.csv")
            plot_heatmap(result["kernel"], args.plots_dir / f"hawkes_kernel_{stem}.png",
                         f"{scheme} {signal} 12x12 integrated Hawkes kernel",
                         "Source state", "Target state",
                         labels, labels)
            plot_heatmap(result["cross"], args.plots_dir / f"hawkes_cross_{stem}.png",
                         f"{scheme} {signal} aligned OBI->return cross-excitation",
                         "OBI regime", "Return regime",
                         [f"OBI_{index}" for index in range(OBI_REGIMES)],
                         [f"RET_{index}" for index in range(RETURN_REGIMES)])
        summary = {
            key: {
                "scheme": result["scheme"],
                "signal": result["signal"],
                "estimator": result["estimator"],
                "fit_success": result["fit_success"],
                "samples": result["samples"],
                "spectral_radius": result["spectral_radius"],
                "reported_spectral_radius": result["reported_spectral_radius"],
                "stabilized": result["stabilized"],
                "aligned_norm": result["aligned_norm"],
                "aligned_sum": result["aligned_sum"],
            }
            for key, result in hawkes.items()
        }
        (args.output_dir / "hawkes_summary.json").write_text(
            json.dumps(summary, indent=2) + "\n")
        stability_rows = []
        for key, result in hawkes.items():
            stability = result.get("stability")
            if not stability:
                continue
            stability_rows.append({
                "scheme": result["scheme"],
                "signal": result["signal"],
                "estimator": result["estimator"],
                "train_aligned_norm": stability["train_aligned_norm"],
                "test_aligned_norm": stability["test_aligned_norm"],
                "delta_aligned_norm": stability["delta_aligned_norm"],
                "train_radius": stability["train_radius"],
                "test_radius": stability["test_radius"],
            })
        pd.DataFrame(stability_rows).to_csv(
            args.output_dir / "hawkes_stability.csv", index=False)

    if not pearson.empty:
        pivot = pearson.pivot_table(
            index="scheme", columns="horizon_s",
            values="pearson_residual", aggfunc="mean",
        ).reindex(index=SCHEMES, columns=PEARSON_HORIZONS)
        if pivot.notna().any().any():
            plot_heatmap(
                pivot.to_numpy(),
                args.plots_dir / "pearson_residual.png",
                "Residualized Pearson association (OBI vs realized trade return)",
                "Horizon (s)", "Filter",
                [str(value) for value in pivot.columns], SCHEMES)

    write_report(args, pearson, regime, hawkes)


def write_report(args: argparse.Namespace, pearson: pd.DataFrame,
                 regime: pd.DataFrame, hawkes: dict | None) -> None:
    lines = [
        "# Paper-Faithful Diagnostics",
        "",
        f"Dataset: `{args.dataset}`",
        "",
        "This report is generated by `benchmarks/paper_analysis.py` from the",
        "observations exported by `hft_paper_eval`. It follows the three-layer",
        "diagnostic ladder of arXiv:2507.22712.",
        "",
        "## Method notes",
        "",
        "- Filters are ex-post order-level masks: lifetime is final exit minus",
        "  first arrival, modification count is the final count, and",
        "  modification time is the final last-two-modification interval.",
        "- OBI is a 10 s backward-looking event-count window.",
        "- Returns are realized first-to-last trade prints in the forward",
        "  window; the paper's appendix horizons 1/10/30/50/80/100 s are used.",
        "- Regime counts use 9 OBI states and 3 return states (negative,",
        "  neutral, positive). The neutral band is 0.5 standard deviations.",
        "- The directional weight matrix uses gamma = "
        f"{args.gamma:.2f}.",
        "- ARMA(1,1) residualization is used when statsmodels is available;",
        "  otherwise the implementation falls back to AR(1).",
        "- The default Hawkes layer is a continuous-time sum-of-exponentials",
        "  maximum-likelihood fit with a fixed decay grid and nonnegative",
        "  amplitudes. The alternative `poisson-basis` estimator is a",
        "  discrete-time basis expansion at the anchor grid.",
        "- Bootstrap intervals are reported for the regime scores; Pearson",
        "  includes p-values, Fisher intervals and a chronological train/test",
        "  split. Hawkes includes a split-half stability table. Overlapping",
        "  windows make the significance numbers descriptive.",
        "",
    ]
    if not pearson.empty:
        lines += [
            "## 1. Pearson association",
            "",
            "| Scheme | Horizon (s) | Samples | Raw Pearson | Raw p | Raw 95% CI | Residual Pearson | Residual p | Train Pearson | Test Pearson |",
            "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
        ]
        for _, row in pearson.sort_values(["scheme", "horizon_s"]).iterrows():
            lines.append(
                f"| {row.scheme} | {int(row.horizon_s)} | {int(row.samples)} | "
                f"{row.pearson_raw:.6f} | {row.p_raw:.4f} | "
                f"[{row.ci_raw_low:.4f}, {row.ci_raw_high:.4f}] | "
                f"{row.pearson_residual:.6f} | {row.p_residual:.4f} | "
                f"{row.pearson_train:.6f} | {row.pearson_test:.6f} |"
            )
        lines += [
            "",
            "Pearson p-values are two-sided and assume independent observations.",
            "The paper uses overlapping windows, so these p-values are descriptive,",
            "not a formal inference correction.",
            "",
        ]

    if not regime.empty:
        lines += [
            "## 2. Discretized regime scores",
            "",
            "Lag 0, raw and residualized variants. `S_rho` is the weighted",
            "directional correlation score; `S_R` is the regression R2.",
            "",
            "| Scheme | Horizon (s) | Variant | Sample | Windows | S_rho | S_rho 95% CI | S_R | S_R 95% CI |",
            "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
        ]
        selected = regime[regime["lag"] == 0]
        for _, row in selected.sort_values(
                ["scheme", "horizon_s", "variant", "sample"]).iterrows():
            lines.append(
                f"| {row.scheme} | {int(row.horizon_s)} | {row.variant} | {row['sample']} | "
                f"{int(row.windows)} | {row.S_rho:.6f} | "
                f"[{row.S_rho_ci_low:.4f}, {row.S_rho_ci_high:.4f}] | "
                f"{row.S_R:.6f} | [{row.S_R_ci_low:.4f}, {row.S_R_ci_high:.4f}] |"
            )
        lines += [
            "",
            "Bootstrap intervals resample windows with replacement; the train/test",
            "split is chronological. Overlapping windows mean the intervals are",
            "descriptive rather than a formal multiple-testing correction.",
            "",
        ]

    if hawkes:
        lines += [
            "## 3. Hawkes excitation",
            "",
            "| Scheme | Signal | Estimator | Samples | Spectral radius | Reported radius | Aligned norm | Aligned sum |",
            "| --- | --- | --- | --- | --- | --- | --- | --- |",
        ]
        for result in hawkes.values():
            lines.append(
                f"| {result['scheme']} | {result['signal']} | {result['estimator']} | "
                f"{result['samples']} | {result['spectral_radius']:.6f} | "
                f"{result['reported_spectral_radius']:.6f} | "
                f"{result['aligned_norm']:.6f} | {result['aligned_sum']:.6f} |"
            )
        lines += [
            "",
            "### Split-half stability",
            "",
            "The first 70% of ticks are fitted separately from the last 30%. This",
            "is a stability check, not a conditional out-of-sample likelihood.",
            "",
            "| Scheme | Signal | Train norm | Test norm | Delta |",
            "| --- | --- | --- | --- | --- |",
        ]
        for result in hawkes.values():
            stability = result.get("stability")
            if not stability:
                continue
            lines.append(
                f"| {result['scheme']} | {result['signal']} | "
                f"{stability['train_aligned_norm']:.6f} | "
                f"{stability['test_aligned_norm']:.6f} | "
                f"{stability['delta_aligned_norm']:+.6f} |"
            )
        lines += [
            "",
            "Charts: `benchmarks/plots/paper/hawkes_kernel_<scheme>_<signal>.png`",
            "and `hawkes_cross_<scheme>_<signal>.png`.",
            "",
        ]

    lines += [
        "## Limitations",
        "",
        "- The paper's regime-score description is ambiguous about whether",
        "  correlations are formed within windows or across windows. This",
        "  implementation uses window-level Q/R count vectors and correlations",
        "  across windows.",
        "- The paper says three return regimes in the text but refers to a",
        "  13x13 kernel. This implementation uses 9 + 3 = 12 states.",
        "- The Hawkes estimator uses a fixed decay grid. It is a",
        "  profile-likelihood MLE over amplitudes, not a joint free optimisation",
        "  of every decay rate.",
        "- The public Binance/crypto-lob data used here does not provide",
        "  parent order IDs for trades, so parent-filtered OBI(T) is only",
        "  available on data that carries that linkage.",
        "",
    ]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    frame = load_ticks(args.ticks_csv)
    pearson = pearson_scores(frame, args.train_fraction)
    regime, matrices = regime_scores(frame, args)
    hawkes = hawkes_analysis(frame, args)
    write_outputs(args, frame, pearson, regime, matrices, hawkes)
    print(f"wrote {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
