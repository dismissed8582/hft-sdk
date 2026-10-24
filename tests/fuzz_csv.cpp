#include "hft/TickData.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string path = "/tmp/hft_fuzz_ticks.csv";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return 0;
        }
        output.write(reinterpret_cast<const char*>(data),
                     static_cast<std::streamsize>(size));
    }
    try {
        (void)hft::data::load_ticks_csv(path);
    } catch (...) {
        // Malformed input is expected to throw; the fuzzer checks for crashes,
        // hangs, and sanitizer findings, not for successful parsing.
    }
    return 0;
}
