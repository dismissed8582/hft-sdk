#pragma once

#include "hft/Types.hpp"
#include "hft/ingest/Csv.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hft::ingest {

// Normalized CSV emitted by tools/parquet_to_csv.py from the Hugging Face
// crypto-lob-stream Parquet files.
//
// depth/snapshots:
//   timestamp_ms,exchange,asset,side,price,quantity,last_update_id
// trades:
//   trade_id,price,qty,quoteQty,time,isBuyerMaker,isBestMatch
//   (consumed by read_binance_trades_csv)

struct CryptoLobLevelRow {
    std::int64_t timestamp_ns{0};
    std::int64_t sequence{0};
    Side side{Side::None};
    double price{0.0};
    double quantity{0.0};
    std::string_view exchange{};
    std::string_view asset{};
};

template <typename Sink>
inline std::uint64_t read_crypto_lob_depth_csv(const std::string& path,
                                               std::string_view exchange_filter,
                                               std::string_view asset_filter,
                                               Sink&& sink) {
    std::uint64_t emitted = 0;
    std::array<std::string_view, 16> fields{};
    bool header_skipped = false;

    for_each_line(path, [&](std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const std::size_t count = split_fields(line, fields);
        if (count < 7) {
            throw std::runtime_error("short crypto-lob depth row: " +
                                     std::string(line));
        }
        if (!header_skipped) {
            header_skipped = true;
            std::int64_t probe = 0;
            if (!parse_integer(fields[0], probe)) {
                return; // header
            }
        }
        std::int64_t timestamp_ms = 0;
        std::int64_t sequence = 0;
        CryptoLobLevelRow row{};
        if (!parse_integer(fields[0], timestamp_ms) ||
            !parse_double(fields[4], row.price) ||
            !parse_double(fields[5], row.quantity) ||
            !parse_integer(fields[6], sequence)) {
            throw std::runtime_error("malformed crypto-lob depth row: " +
                                     std::string(line));
        }
        if (timestamp_ms < 0 || timestamp_ms > std::numeric_limits<std::int64_t>::max() / 1'000'000) {
            throw std::runtime_error("crypto-lob timestamp is out of range");
        }
        row.timestamp_ns = timestamp_ms * 1'000'000;
        row.sequence = sequence;
        row.exchange = fields[1];
        row.asset = fields[2];
        if (fields[3] != "bid" && fields[3] != "ask") throw std::runtime_error("invalid crypto-lob side");
        row.side = fields[3] == "bid" ? Side::Bid : Side::Ask;
        if (!exchange_filter.empty() && row.exchange != exchange_filter) {
            return;
        }
        if (!asset_filter.empty() && row.asset != asset_filter) {
            return;
        }
        sink(row);
        ++emitted;
    });
    return emitted;
}

} // namespace hft::ingest

