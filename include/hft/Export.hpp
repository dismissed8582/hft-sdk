#pragma once

#if defined(_WIN32)
#if defined(HFT_SDK_BUILD)
#define HFT_SDK_API __declspec(dllexport)
#else
#define HFT_SDK_API __declspec(dllimport)
#endif
#else
#define HFT_SDK_API __attribute__((visibility("default")))
#endif
