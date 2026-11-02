#include "hft/TickData.hpp"
#include "hft/ingest/Csv.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    double number = 0;
    std::int64_t timestamp = 0;
    (void)hft::ingest::parse_double(text, number);
    (void)hft::ingest::parse_utc_ns(text, timestamp);
    std::istringstream input(std::string(reinterpret_cast<const char*>(data), size));
    try {
        (void)hft::data::load_ticks_csv(input);
    } catch (const std::exception&) {
        // Malformed input is expected to fail cleanly.
    }
    return 0;
}
