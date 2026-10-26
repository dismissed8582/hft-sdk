#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft::metrics {

class Metric {
public:
    explicit Metric(std::string help) : help_(std::move(help)) {}
    virtual ~Metric() = default;
    virtual void render(std::ostream& output, const std::string& name) const = 0;
    [[nodiscard]] virtual const char* type() const noexcept = 0;
    [[nodiscard]] const std::string& help() const noexcept { return help_; }

private:
    std::string help_;
};

class Counter final : public Metric {
public:
    explicit Counter(std::string help) : Metric(std::move(help)) {}
    void increment(std::uint64_t value = 1) noexcept {
        value_.fetch_add(value, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const char* type() const noexcept override { return "counter"; }
    void render(std::ostream& output, const std::string& name) const override {
        output << name << ' ' << value() << '\n';
    }

private:
    std::atomic<std::uint64_t> value_{0};
};

class Gauge final : public Metric {
public:
    explicit Gauge(std::string help) : Metric(std::move(help)) {}
    void set(std::int64_t value) noexcept {
        value_.store(value, std::memory_order_relaxed);
    }
    void add(std::int64_t delta) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const char* type() const noexcept override { return "gauge"; }
    void render(std::ostream& output, const std::string& name) const override {
        output << name << ' ' << value() << '\n';
    }

private:
    std::atomic<std::int64_t> value_{0};
};

class Histogram final : public Metric {
public:
    Histogram(std::vector<double> buckets, std::string help)
        : Metric(std::move(help)), buckets_(std::move(buckets)),
          counts_(buckets_.size() + 1, 0) {
        if (std::any_of(buckets_.begin(), buckets_.end(), [](double x) { return !std::isfinite(x); }) ||
            !std::is_sorted(buckets_.begin(), buckets_.end()) ||
            std::adjacent_find(buckets_.begin(), buckets_.end()) != buckets_.end()) {
            throw std::invalid_argument("histogram buckets must be finite and strictly increasing");
        }
    }

    void observe(double value) noexcept {
        if (!std::isfinite(value)) return;
        const std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index = 0;
        while (index < buckets_.size() && value > buckets_[index]) {
            ++index;
        }
        ++counts_[index];
        ++count_;
        sum_ += value;
    }

    [[nodiscard]] const char* type() const noexcept override { return "histogram"; }

    void render(std::ostream& output, const std::string& name) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < buckets_.size(); ++index) {
            cumulative += counts_[index];
            output << name << "_bucket{le=\"" << buckets_[index] << "\"} "
                   << cumulative << '\n';
        }
        output << name << "_bucket{le=\"+Inf\"} " << count_ << '\n';
        output << name << "_sum " << sum_ << '\n';
        output << name << "_count " << count_ << '\n';
    }

private:
    std::vector<double> buckets_;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> counts_;
    std::uint64_t count_{0};
    double sum_{0.0};
};

class Registry {
public:
    Counter& counter(const std::string& name, const std::string& help = {}) {
        return get<Counter>(name, help);
    }

    Gauge& gauge(const std::string& name, const std::string& help = {}) {
        return get<Gauge>(name, help);
    }

    Histogram& histogram(const std::string& name, std::vector<double> buckets,
                         const std::string& help = {}) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = metrics_.find(name);
        if (iterator != metrics_.end()) {
            auto* metric = dynamic_cast<Histogram*>(iterator->second.get());
            if (!metric) throw std::invalid_argument("metric already registered with another type: " + name);
            return *metric;
        }
        auto metric = std::make_unique<Histogram>(std::move(buckets), help);
        Histogram& reference = *metric;
        metrics_.emplace(name, std::move(metric));
        return reference;
    }

    [[nodiscard]] std::string render() const {
        std::ostringstream output;
        output << std::setprecision(12);
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, metric] : metrics_) {
            output << "# HELP " << name << ' ' << metric->help() << '\n';
            output << "# TYPE " << name << ' ' << metric->type() << '\n';
            metric->render(output, name);
        }
        return output.str();
    }

private:
    template <typename MetricType>
    MetricType& get(const std::string& name, const std::string& help) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = metrics_.find(name);
        if (iterator != metrics_.end()) {
            auto* metric = dynamic_cast<MetricType*>(iterator->second.get());
            if (!metric) throw std::invalid_argument("metric already registered with another type: " + name);
            return *metric;
        }
        auto metric = std::make_unique<MetricType>(help);
        MetricType& reference = *metric;
        metrics_.emplace(name, std::move(metric));
        return reference;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Metric>> metrics_;
};

} // namespace hft::metrics

