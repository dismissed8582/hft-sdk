#include "hft/Paper.hpp"
#include "hft/TickData.hpp"
#include "hft/ingest/Csv.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Options {
    std::string ticks{"data/sample_ticks.csv"};
    std::string out{"benchmarks/results/paper/paper_ticks.csv"};
    bool streaming{false};
    std::int64_t lifetime_ms{100};
    std::uint32_t max_modifications{3};
    std::int64_t mod_time_ms{50};
    std::int64_t obi_window_ms{10'000};
    std::int64_t anchor_ms{15'000};
    std::int64_t sample_ms{250};
    std::size_t limit{0};
    std::string parent_map{};
};

[[nodiscard]] std::int64_t parse_i64(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer for ") + name + ": " +
                                 value);
    }
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: hft_paper_eval [options]\n"
                << "  --ticks FILE           input tick CSV\n"
                << "  --out FILE             output paper-observation CSV\n"
                << "  --streaming            use causal running filters instead of\n"
                << "                         the paper's ex-post order filters\n"
                << "  --lifetime-ms N        lifetime filter threshold (default 100)\n"
                << "  --max-mods N           modification-count threshold (default 3)\n"
                << "  --mod-time-ms N        modification-time threshold (default 50)\n"
                << "  --obi-window-ms N      OBI lookback window (default 10000)\n"
                << "  --anchor-ms N          regime anchor spacing (default 15000)\n"
                << "  --sample-ms N          observation spacing (default 250)\n"
                << "  --limit N              stop after N input events (0 = all)\n"
                << "  --parent-map FILE      CSV mapping trade_id,parent_order_id\n";
            std::exit(0);
        }
        if (argument == "--streaming") {
            options.streaming = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--ticks") {
            options.ticks = value;
        } else if (argument == "--out") {
            options.out = value;
        } else if (argument == "--lifetime-ms") {
            options.lifetime_ms = parse_i64(value, "lifetime-ms");
        } else if (argument == "--max-mods") {
            options.max_modifications = static_cast<std::uint32_t>(
                parse_i64(value, "max-mods"));
        } else if (argument == "--mod-time-ms") {
            options.mod_time_ms = parse_i64(value, "mod-time-ms");
        } else if (argument == "--obi-window-ms") {
            options.obi_window_ms = parse_i64(value, "obi-window-ms");
        } else if (argument == "--anchor-ms") {
            options.anchor_ms = parse_i64(value, "anchor-ms");
        } else if (argument == "--sample-ms") {
            options.sample_ms = parse_i64(value, "sample-ms");
        } else if (argument == "--limit") {
            options.limit = static_cast<std::size_t>(
                parse_i64(value, "limit"));
        } else if (argument == "--parent-map") {
            options.parent_map = value;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (options.lifetime_ms < 0 || options.mod_time_ms < 0 ||
        options.obi_window_ms <= 0 || options.anchor_ms <= 0 ||
        options.sample_ms <= 0) {
        throw std::runtime_error("thresholds and windows must be non-negative");
    }
    return options;
}

std::unordered_map<std::uint64_t, std::uint64_t> load_parent_map(
    const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open parent map: " + path);
    }
    std::unordered_map<std::uint64_t, std::uint64_t> mapping;
    std::string line;
    std::array<std::string_view, 8> fields{};
    bool first = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t count = hft::ingest::split_fields(line, fields);
        if (count < 2) {
            continue;
        }
        std::uint64_t trade_id = 0;
        std::uint64_t parent_id = 0;
        if (!hft::ingest::parse_integer(fields[0], trade_id) ||
            !hft::ingest::parse_integer(fields[1], parent_id)) {
            if (first) {
                first = false;
                continue;
            }
            throw std::runtime_error("malformed parent map row: " + line);
        }
        first = false;
        mapping[trade_id] = parent_id;
    }
    return mapping;
}

void ensure_parent(const std::string& path) {
    const std::filesystem::path output(path);
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::vector<hft::MarketEvent> events =
            hft::data::load_ticks_csv(options.ticks);
        if (options.limit != 0 && events.size() > options.limit) {
            events.resize(options.limit);
        }
        if (events.empty()) {
            throw std::runtime_error("tick file contains no events");
        }
        if (!options.parent_map.empty()) {
            const auto mapping = load_parent_map(options.parent_map);
            for (hft::MarketEvent& event : events) {
                if (event.type != hft::EventType::Trade) {
                    continue;
                }
                const auto iterator = mapping.find(event.order_id);
                if (iterator != mapping.end()) {
                    event.parent_order_id = iterator->second;
                }
            }
        }
        std::stable_sort(events.begin(), events.end(),
                         [](const hft::MarketEvent& left,
                            const hft::MarketEvent& right) {
                             return left.timestamp_ns < right.timestamp_ns;
                         });

        const hft::obi::FilterThresholds thresholds{
            options.lifetime_ms * hft::kNanosecondsPerMillisecond,
            options.max_modifications,
            options.mod_time_ms * hft::kNanosecondsPerMillisecond};
        hft::paper::PaperConfig config{};
        config.obi_window_ns =
            options.obi_window_ms * hft::kNanosecondsPerMillisecond;
        config.anchor_ns = options.anchor_ms * hft::kNanosecondsPerMillisecond;
        config.sample_ns = options.sample_ms * hft::kNanosecondsPerMillisecond;
        config.paper_filters = !options.streaming;

        const std::vector<hft::paper::PaperTick> ticks =
            hft::paper::evaluate(events, thresholds, config);

        ensure_parent(options.out);
        std::ofstream output(options.out);
        if (!output) {
            throw std::runtime_error("unable to write " + options.out);
        }
        output << "timestamp_ns,scheme,is_anchor,lob_obi,lob_valid,trade_obi,"
                  "trade_valid,ret_1s,ret_10s,ret_20s,ret_30s,ret_50s,ret_80s,"
                  "ret_100s,ret_1s_valid,ret_10s_valid,ret_20s_valid,"
                  "ret_30s_valid,ret_50s_valid,ret_80s_valid,ret_100s_valid,"
                  "obi_regime,trade_obi_regime,return_regime_1s,"
                  "return_regime_10s,return_regime_20s\n";
        output << std::setprecision(12);
        for (const hft::paper::PaperTick& tick : ticks) {
            output << tick.timestamp_ns << ','
                   << hft::obi::kFilterSchemeNames[tick.scheme] << ','
                   << (tick.is_anchor ? 1 : 0) << ','
                   << tick.lob_obi << ',' << (tick.lob_valid ? 1 : 0) << ','
                   << tick.trade_obi << ',' << (tick.trade_valid ? 1 : 0);
            for (std::size_t index = 0;
                 index < hft::paper::kHorizonNs.size(); ++index) {
                output << ',';
                if (tick.forward_valid[index] != 0) {
                    output << tick.forward_returns[index];
                }
            }
            for (std::size_t index = 0;
                 index < hft::paper::kHorizonNs.size(); ++index) {
                output << ',' << static_cast<int>(tick.forward_valid[index]);
            }
            output << ',' << static_cast<int>(tick.obi_regime)
                   << ',' << static_cast<int>(tick.trade_obi_regime)
                   << ',' << static_cast<int>(tick.return_regime[0])
                   << ',' << static_cast<int>(tick.return_regime[1])
                   << ',' << static_cast<int>(tick.return_regime[2]) << '\n';
        }
        std::cout << "events=" << events.size() << " ticks=" << ticks.size()
                  << " out=" << options.out << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
