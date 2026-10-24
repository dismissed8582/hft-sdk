#pragma once

#include "hft/Types.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(HFT_WITH_ARROW)
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#endif

namespace hft::ingest {

// crypto-lob-stream (Hugging Face: Goooddy/crypto-lob-stream) native schema.
//
// snapshots: timestamp_ms, exchange, asset, side, price, quantity, last_update_id
// depth:     timestamp_ms, exchange_ts, exchange, asset, side, price,
//            quantity, first_update_id, last_update_id
// trades:    timestamp_ms, exchange_ts, exchange, asset, trade_id, price,
//            quantity, buyer_maker
//
// The files are Snappy-compressed Parquet. This reader is compiled only when
// the project is configured with HFT_WITH_ARROW=ON. For a zero-dependency
// workflow use tools/parquet_to_csv.py, then feed the CSV export through the
// normal Binance/Hyperliquid readers.

struct CryptoLobTrade {
    std::int64_t timestamp_ns{0};
    std::uint64_t trade_id{0};
    double price{0.0};
    double quantity{0.0};
    Side aggressor{Side::None};
    std::string_view asset{};
};

struct CryptoLobDepthDiff {
    std::int64_t timestamp_ns{0};
    std::uint64_t sequence{0};
    Side side{Side::None};
    double price{0.0};
    double quantity{0.0}; // new total size; 0.0 removes the level
    std::string_view asset{};
};

struct CryptoLobSnapshot {
    std::int64_t timestamp_ns{0};
    std::string_view asset{};
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;

    [[nodiscard]] bool best(double& bid, double& ask) const noexcept {
        if (bids.empty() || asks.empty()) {
            return false;
        }
        bid = bids.front().first;
        ask = asks.front().first;
        return bid > 0.0 && ask > 0.0 && bid <= ask;
    }

    [[nodiscard]] double obi(std::size_t depth = 10) const noexcept {
        const std::size_t bid_count = std::min(depth, bids.size());
        const std::size_t ask_count = std::min(depth, asks.size());
        double bid_quantity = 0.0;
        double ask_quantity = 0.0;
        for (std::size_t index = 0; index < bid_count; ++index) {
            bid_quantity += bids[index].second;
        }
        for (std::size_t index = 0; index < ask_count; ++index) {
            ask_quantity += asks[index].second;
        }
        const double total = bid_quantity + ask_quantity;
        return total > 0.0 ? (bid_quantity - ask_quantity) / total : 0.0;
    }
};

#if defined(HFT_WITH_ARROW)

namespace detail {

inline std::shared_ptr<arrow::Table> open_parquet(const std::string& path) {
    auto input = arrow::io::ReadableFile::Open(path);
    if (!input.ok()) {
        throw std::runtime_error("unable to open parquet: " + path);
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    const auto status =
        parquet::arrow::OpenFile(input.ValueOrDie(), arrow::default_memory_pool(),
                                 &reader);
    if (!status.ok()) {
        throw std::runtime_error("unable to open parquet reader: " + path);
    }
    std::shared_ptr<arrow::Table> table;
    if (!reader->ReadTable(&table).ok()) {
        throw std::runtime_error("unable to read parquet table: " + path);
    }
    return table->CombineChunks().ValueOrDie();
}

inline std::shared_ptr<arrow::ChunkedArray> column(
    const std::shared_ptr<arrow::Table>& table, const char* name) {
    auto column_data = table->GetColumnByName(name);
    if (column_data == nullptr) {
        throw std::runtime_error(std::string("missing parquet column: ") + name);
    }
    return column_data;
}

inline std::int64_t i64_at(const std::shared_ptr<arrow::ChunkedArray>& column,
                           std::int64_t row) {
    return std::static_pointer_cast<arrow::Int64Array>(column->chunk(0))
        ->Value(row);
}

inline double f64_at(const std::shared_ptr<arrow::ChunkedArray>& column,
                     std::int64_t row) {
    return std::static_pointer_cast<arrow::DoubleArray>(column->chunk(0))
        ->Value(row);
}

inline std::string_view str_at(const std::shared_ptr<arrow::ChunkedArray>& column,
                               std::int64_t row) {
    const auto value =
        std::static_pointer_cast<arrow::StringArray>(column->chunk(0))->GetString(
            row);
    return std::string_view(value.data(), value.size());
}

} // namespace detail

template <typename Sink>
inline std::uint64_t read_crypto_lob_trades(const std::string& path,
                                            std::string_view exchange,
                                            Sink&& sink) {
    const auto table = detail::open_parquet(path);
    const auto timestamp = detail::column(table, "timestamp_ms");
    const auto price = detail::column(table, "price");
    const auto quantity = detail::column(table, "quantity");
    const auto trade_id = detail::column(table, "trade_id");
    const auto buyer_maker = detail::column(table, "buyer_maker");
    const auto exchange_column = detail::column(table, "exchange");
    const auto asset = detail::column(table, "asset");

    std::uint64_t emitted = 0;
    for (std::int64_t row = 0; row < table->num_rows(); ++row) {
        if (!exchange.empty() &&
            detail::str_at(exchange_column, row) != exchange) {
            continue;
        }
        CryptoLobTrade trade{};
        trade.timestamp_ns = detail::i64_at(timestamp, row) * 1'000'000;
        trade.trade_id =
            static_cast<std::uint64_t>(detail::i64_at(trade_id, row));
        trade.price = detail::f64_at(price, row);
        trade.quantity = detail::f64_at(quantity, row);
        trade.aggressor =
            std::static_pointer_cast<arrow::BooleanArray>(buyer_maker->chunk(0))
                        ->Value(row)
                ? Side::Sell
                : Side::Buy;
        trade.asset = detail::str_at(asset, row);
        sink(trade);
        ++emitted;
    }
    return emitted;
}

template <typename Sink>
inline std::uint64_t read_crypto_lob_depth(const std::string& path,
                                           std::string_view exchange,
                                           Sink&& sink) {
    const auto table = detail::open_parquet(path);
    const auto timestamp = detail::column(table, "timestamp_ms");
    const auto price = detail::column(table, "price");
    const auto quantity = detail::column(table, "quantity");
    const auto side = detail::column(table, "side");
    const auto sequence = detail::column(table, "last_update_id");
    const auto exchange_column = detail::column(table, "exchange");
    const auto asset = detail::column(table, "asset");

    std::uint64_t emitted = 0;
    for (std::int64_t row = 0; row < table->num_rows(); ++row) {
        if (!exchange.empty() &&
            detail::str_at(exchange_column, row) != exchange) {
            continue;
        }
        CryptoLobDepthDiff diff{};
        diff.timestamp_ns = detail::i64_at(timestamp, row) * 1'000'000;
        diff.sequence =
            static_cast<std::uint64_t>(detail::i64_at(sequence, row));
        diff.price = detail::f64_at(price, row);
        diff.quantity = detail::f64_at(quantity, row);
        diff.side = detail::str_at(side, row) == "bid" ? Side::Bid : Side::Ask;
        diff.asset = detail::str_at(asset, row);
        sink(diff);
        ++emitted;
    }
    return emitted;
}

template <typename Sink>
inline std::uint64_t read_crypto_lob_snapshots(const std::string& path,
                                               std::string_view exchange,
                                               Sink&& sink) {
    const auto table = detail::open_parquet(path);
    const auto timestamp = detail::column(table, "timestamp_ms");
    const auto price = detail::column(table, "price");
    const auto quantity = detail::column(table, "quantity");
    const auto side = detail::column(table, "side");
    const auto exchange_column = detail::column(table, "exchange");
    const auto asset = detail::column(table, "asset");

    CryptoLobSnapshot snapshot{};
    bool active = false;
    std::int64_t current_timestamp = 0;
    std::string_view current_asset{};
    std::uint64_t emitted = 0;

    const auto flush = [&]() {
        if (!active) {
            return;
        }
        std::sort(snapshot.bids.begin(), snapshot.bids.end(),
                  [](const auto& left, const auto& right) {
                      return left.first > right.first;
                  });
        std::sort(snapshot.asks.begin(), snapshot.asks.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
        sink(snapshot);
        ++emitted;
        snapshot.bids.clear();
        snapshot.asks.clear();
        active = false;
    };

    for (std::int64_t row = 0; row < table->num_rows(); ++row) {
        if (!exchange.empty() &&
            detail::str_at(exchange_column, row) != exchange) {
            continue;
        }
        const std::int64_t timestamp_ms = detail::i64_at(timestamp, row);
        const std::string_view asset = detail::str_at(asset, row);
        if (active &&
            (timestamp_ms != current_timestamp || asset != current_asset)) {
            flush();
        }
        if (!active) {
            snapshot.timestamp_ns = timestamp_ms * 1'000'000;
            snapshot.asset = asset;
            current_timestamp = timestamp_ms;
            current_asset = asset;
            active = true;
        }
        const double level_price = detail::f64_at(price, row);
        const double level_quantity = detail::f64_at(quantity, row);
        if (detail::str_at(side, row) == "bid") {
            snapshot.bids.emplace_back(level_price, level_quantity);
        } else {
            snapshot.asks.emplace_back(level_price, level_quantity);
        }
    }
    flush();
    return emitted;
}

#endif // HFT_WITH_ARROW

} // namespace hft::ingest
