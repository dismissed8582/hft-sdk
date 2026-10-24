#pragma once

#include "hft/Types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <xsimd/xsimd.hpp>

namespace hft::scoring {

struct WelfordStats {
    double count{0.0};
    double mean_x{0.0};
    double mean_y{0.0};
    double m2_x{0.0};
    double m2_y{0.0};
    double covariance{0.0};

    [[nodiscard]] double variance_x(bool sample = true) const noexcept {
        const double denominator = sample ? count - 1.0 : count;
        return denominator > 0.0 ? m2_x / denominator : 0.0;
    }

    [[nodiscard]] double variance_y(bool sample = true) const noexcept {
        const double denominator = sample ? count - 1.0 : count;
        return denominator > 0.0 ? m2_y / denominator : 0.0;
    }

    [[nodiscard]] double stddev_x(bool sample = true) const noexcept {
        return std::sqrt(variance_x(sample));
    }

    [[nodiscard]] double stddev_y(bool sample = true) const noexcept {
        return std::sqrt(variance_y(sample));
    }

    [[nodiscard]] double correlation() const noexcept {
        if (count < 2.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double denominator = std::sqrt(m2_x * m2_y);
        return denominator > 0.0 ? covariance / denominator
                                 : std::numeric_limits<double>::quiet_NaN();
    }

    [[nodiscard]] static WelfordStats merge(WelfordStats a, const WelfordStats& b) noexcept {
        if (b.count <= 0.0) {
            return a;
        }
        if (a.count <= 0.0) {
            return b;
        }
        const double total = a.count + b.count;
        const double dx = b.mean_x - a.mean_x;
        const double dy = b.mean_y - a.mean_y;
        const double cross_weight = a.count * b.count / total;
        a.m2_x += b.m2_x + dx * dx * cross_weight;
        a.m2_y += b.m2_y + dy * dy * cross_weight;
        a.covariance += b.covariance + dx * dy * cross_weight;
        a.mean_x += dx * (b.count / total);
        a.mean_y += dy * (b.count / total);
        a.count = total;
        return a;
    }
};

class SimdWelford {
public:
    using batch_type = xsimd::batch<double>;
    static constexpr std::size_t width = batch_type::size;

    SimdWelford() noexcept {
        reset();
    }

    void reset() noexcept {
        mean_x_ = batch_type(0.0);
        mean_y_ = batch_type(0.0);
        m2_x_ = batch_type(0.0);
        m2_y_ = batch_type(0.0);
        covariance_ = batch_type(0.0);
        lane_count_ = batch_type(0.0);
        scalar_ = WelfordStats{};
    }

    [[gnu::always_inline]] void add(double x, double y) noexcept {
        scalar_.count += 1.0;
        const double dx = x - scalar_.mean_x;
        const double dy = y - scalar_.mean_y;
        scalar_.mean_x += dx / scalar_.count;
        scalar_.mean_y += dy / scalar_.count;
        scalar_.m2_x += dx * (x - scalar_.mean_x);
        scalar_.m2_y += dy * (y - scalar_.mean_y);
        scalar_.covariance += dx * (y - scalar_.mean_y);
    }

    [[gnu::always_inline]] void add_batch(const double* x, const double* y,
                                          std::size_t count) noexcept {
        std::size_t index = 0;
        const batch_type one(1.0);
        for (; index + width <= count; index += width) {
            const batch_type xb = batch_type::load_unaligned(x + index);
            const batch_type yb = batch_type::load_unaligned(y + index);
            lane_count_ += one;

            const batch_type dx = xb - mean_x_;
            const batch_type dy = yb - mean_y_;
            mean_x_ += dx / lane_count_;
            mean_y_ += dy / lane_count_;
            m2_x_ += dx * (xb - mean_x_);
            m2_y_ += dy * (yb - mean_y_);
            covariance_ += dx * (yb - mean_y_);
        }
        for (; index < count; ++index) {
            add(x[index], y[index]);
        }
    }

    [[gnu::always_inline]] void add(std::span<const double> x,
                                    std::span<const double> y) noexcept {
        const std::size_t count = std::min(x.size(), y.size());
        add_batch(x.data(), y.data(), count);
    }

    [[nodiscard]] WelfordStats finalize() const noexcept {
        alignas(64) double means_x[width];
        alignas(64) double means_y[width];
        alignas(64) double m2x[width];
        alignas(64) double m2y[width];
        alignas(64) double cov[width];
        alignas(64) double counts[width];

        mean_x_.store_unaligned(means_x);
        mean_y_.store_unaligned(means_y);
        m2_x_.store_unaligned(m2x);
        m2_y_.store_unaligned(m2y);
        covariance_.store_unaligned(cov);
        lane_count_.store_unaligned(counts);

        WelfordStats total{};
        for (std::size_t lane = 0; lane < width; ++lane) {
            WelfordStats partial{counts[lane], means_x[lane], means_y[lane], m2x[lane],
                                 m2y[lane], cov[lane]};
            total = WelfordStats::merge(total, partial);
        }
        return WelfordStats::merge(total, scalar_);
    }

private:
    alignas(64) batch_type mean_x_;
    alignas(64) batch_type mean_y_;
    alignas(64) batch_type m2_x_;
    alignas(64) batch_type m2_y_;
    alignas(64) batch_type covariance_;
    alignas(64) batch_type lane_count_;
    WelfordStats scalar_{};
};

struct CorrelationScore {
    double pearson{0.0};
    double r2{0.0};
    std::size_t samples{0};
    bool valid{false};
};

inline CorrelationScore correlation_from_stats(const WelfordStats& stats) noexcept {
    const double correlation = stats.correlation();
    const bool valid = std::isfinite(correlation);
    return CorrelationScore{valid ? correlation : 0.0, valid ? correlation * correlation : 0.0,
                            static_cast<std::size_t>(stats.count), valid};
}

inline CorrelationScore correlation_score(std::span<const double> x,
                                          std::span<const double> y) noexcept {
    SimdWelford accumulator;
    accumulator.add(x, y);
    return correlation_from_stats(accumulator.finalize());
}

struct RollingMoments {
    double mean{0.0};
    double stddev{0.0};
    std::size_t samples{0};
    bool valid{false};
};

inline RollingMoments rolling_moments(std::span<const double> values) noexcept {
    SimdWelford accumulator;
    accumulator.add_batch(values.data(), values.data(), values.size());
    const WelfordStats stats = accumulator.finalize();
    const bool valid = stats.count > 0.0;
    return RollingMoments{stats.mean_x, stats.stddev_x(), static_cast<std::size_t>(stats.count),
                          valid};
}

struct RegimeDiscretizer {
    static constexpr int kOBIRegimes = 9;
    static constexpr int kReturnRegimes = 4;

    [[gnu::always_inline]] static int clamp_bucket(int bucket, int maximum) noexcept {
        const int negative_mask = bucket >> 31;
        int clamped = bucket & ~negative_mask;
        const int overflow_mask = (maximum - clamped) >> 31;
        clamped = (clamped & ~overflow_mask) | (maximum & overflow_mask);
        return clamped;
    }

    [[gnu::always_inline]] static int obi_regime(double value) noexcept {
        constexpr double scale = 4.5;
        const double clipped = std::fmin(1.0, std::fmax(-1.0, value));
        const int bucket = static_cast<int>((clipped + 1.0) * scale);
        return clamp_bucket(bucket, kOBIRegimes - 1);
    }

    [[gnu::always_inline]] static int return_regime(double value) noexcept {
        constexpr double scale = 2.0;
        const double clipped = std::fmin(1.0, std::fmax(-1.0, value));
        const int bucket = static_cast<int>((clipped + 1.0) * scale);
        return clamp_bucket(bucket, kReturnRegimes - 1);
    }

    [[gnu::always_inline]] static double regime_center_obi(int regime) noexcept {
        return -1.0 + (static_cast<double>(regime) + 0.5) * (2.0 / kOBIRegimes);
    }

    [[gnu::always_inline]] static double regime_center_return(int regime) noexcept {
        return -1.0 + (static_cast<double>(regime) + 0.5) * (2.0 / kReturnRegimes);
    }
};

// Basic AR(1) residualization. phi is the lag-1 autocorrelation estimated
// with the same Welford accumulator used for the Pearson scores. The first
// residual is undefined and is returned as zero; callers correlate from t=1.
inline std::vector<double> ar1_residuals(std::span<const double> values) {
    std::vector<double> residuals(values.size(), 0.0);
    if (values.size() < 2) {
        return residuals;
    }
    SimdWelford lag_one;
    lag_one.add(std::span<const double>(values.data(), values.size() - 1),
                std::span<const double>(values.data() + 1, values.size() - 1));
    const double phi_raw = lag_one.finalize().correlation();
    // A constant series has zero variance, so the lag-1 correlation is
    // undefined; the limiting AR(1) coefficient is 1 and residuals are zero.
    const double phi = std::isfinite(phi_raw) ? phi_raw : 1.0;
    for (std::size_t index = 1; index < values.size(); ++index) {
        residuals[index] = values[index] - phi * values[index - 1];
    }
    return residuals;
}

// Regime cross-correlation is computed only on AR(1) residuals so that
// persistence in the discretized state does not masquerade as excitation.
inline CorrelationScore regime_cross_correlation(std::span<const double> obi,
                                                 std::span<const double> returns) {
    const std::size_t count = std::min(obi.size(), returns.size());
    if (count < 3) {
        return CorrelationScore{};
    }
    std::vector<double> obi_centers(count, 0.0);
    std::vector<double> return_centers(count, 0.0);
    for (std::size_t index = 0; index < count; ++index) {
        obi_centers[index] = RegimeDiscretizer::regime_center_obi(
            RegimeDiscretizer::obi_regime(obi[index]));
        return_centers[index] = RegimeDiscretizer::regime_center_return(
            RegimeDiscretizer::return_regime(returns[index]));
    }
    const std::vector<double> obi_residuals = ar1_residuals(obi_centers);
    const std::vector<double> return_residuals = ar1_residuals(return_centers);
    SimdWelford accumulator;
    for (std::size_t index = 1; index < count; ++index) {
        accumulator.add(obi_residuals[index], return_residuals[index]);
    }
    return correlation_from_stats(accumulator.finalize());
}

}
