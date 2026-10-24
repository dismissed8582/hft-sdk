#pragma once

#include <string_view>

// Keep these in sync with project(... VERSION ...) in CMakeLists.txt. The
// release workflow checks the values before publishing artifacts.
#define HFT_SDK_VERSION_MAJOR 1
#define HFT_SDK_VERSION_MINOR 0
#define HFT_SDK_VERSION_PATCH 0

namespace hft {

inline constexpr int version_major = HFT_SDK_VERSION_MAJOR;
inline constexpr int version_minor = HFT_SDK_VERSION_MINOR;
inline constexpr int version_patch = HFT_SDK_VERSION_PATCH;
inline constexpr std::string_view version_string = "1.0.0";

} // namespace hft
