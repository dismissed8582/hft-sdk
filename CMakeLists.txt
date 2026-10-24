cmake_minimum_required(VERSION 3.24)

project(hft_lob_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HFT_BUILD_TESTS "Build correctness tests" ON)
option(HFT_BUILD_BENCHMARKS "Build Google Benchmark suite" ON)
option(HFT_BUILD_EVALUATOR "Build CSV signal evaluator" ON)
option(HFT_ARCH_NATIVE "Optimize for the local CPU (NEON on Apple silicon)" ON)
option(HFT_BUILD_REPLAY "Build the public-data replay tool" ON)
option(HFT_BUILD_FEED "Build the production feed runtime" ON)
option(HFT_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(HFT_WITH_ARROW "Enable the optional Apache Arrow/Parquet reader" OFF)
option(HFT_BUILD_SHARED_SDK "Build the compiled shared SDK library" ON)
option(HFT_BUILD_PYTHON "Build pybind11 Python bindings" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)
include(FetchContent)

find_package(xsimd CONFIG QUIET)
if(NOT xsimd_FOUND)
    FetchContent_Declare(
        xsimd
        URL https://github.com/xtensor-stack/xsimd/archive/refs/tags/13.2.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(XSIMD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(XSIMD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(xsimd)
endif()

if(TARGET xsimd::xsimd)
    set(HFT_XSIMD_TARGET xsimd::xsimd)
elseif(TARGET xsimd)
    set(HFT_XSIMD_TARGET xsimd)
else()
    message(FATAL_ERROR "xsimd target was not created")
endif()

add_library(hft_core INTERFACE)
add_library(hft::core ALIAS hft_core)
add_library(HftSdk::core ALIAS hft_core)
set_target_properties(hft_core PROPERTIES EXPORT_NAME core)
target_include_directories(hft_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(hft_core INTERFACE cxx_std_20)
target_link_libraries(hft_core INTERFACE ${HFT_XSIMD_TARGET})

if(HFT_BUILD_SHARED_SDK)
    add_library(hft_sdk SHARED src/sdk.cpp src/spsc_queue.cpp)
    add_library(HftSdk::sdk ALIAS hft_sdk)
    set_target_properties(hft_sdk PROPERTIES
        EXPORT_NAME sdk
        OUTPUT_NAME hft_sdk
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_compile_definitions(hft_sdk PRIVATE HFT_SDK_BUILD)
    target_link_libraries(hft_sdk PUBLIC hft::core)
endif()

find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(hft_core INTERFACE ZLIB::ZLIB)
    target_compile_definitions(hft_core INTERFACE HFT_WITH_ZLIB=1)
endif()

if(HFT_WITH_ARROW)
    find_package(Arrow REQUIRED)
    find_package(Parquet REQUIRED)
    target_link_libraries(hft_core INTERFACE Arrow::arrow_shared Parquet::parquet_shared)
    target_compile_definitions(hft_core INTERFACE HFT_WITH_ARROW=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hft_core INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -fno-math-errno
    )
    if(HFT_ARCH_NATIVE)
        target_compile_options(hft_core INTERFACE -march=native)
    endif()
endif()

if(HFT_BUILD_TESTS)
    enable_testing()
    add_executable(hft_tests tests/test_engine.cpp)
    target_link_libraries(hft_tests PRIVATE hft::core)
    add_test(NAME hft_tests COMMAND hft_tests)
endif()

if(HFT_BUILD_EVALUATOR)
    add_executable(hft_eval src/evaluate.cpp)
    target_link_libraries(hft_eval PRIVATE hft::core)
endif()

if(HFT_BUILD_REPLAY)
    add_executable(hft_replay src/replay.cpp)
    target_link_libraries(hft_replay PRIVATE hft::core Threads::Threads)
endif()

if(HFT_BUILD_EVALUATOR)
    add_executable(hft_paper_eval src/paper_eval.cpp)
    target_link_libraries(hft_paper_eval PRIVATE hft::core)
endif()

if(HFT_BUILD_FEED AND UNIX)
    add_executable(hft_feed src/feed_runner.cpp)
    target_link_libraries(hft_feed PRIVATE hft::core Threads::Threads)
endif()

if(HFT_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(hft_fuzz_csv tests/fuzz_csv.cpp)
    target_link_libraries(hft_fuzz_csv PRIVATE hft::core)
    target_compile_options(hft_fuzz_csv PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(hft_fuzz_csv PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()

if(HFT_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG QUIET)
    if(NOT benchmark_FOUND)
        FetchContent_Declare(
            google_benchmark
            URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(google_benchmark)
    endif()

    add_executable(hft_bench src/benchmark.cpp)
    target_link_libraries(hft_bench PRIVATE hft::core benchmark::benchmark Threads::Threads)
    target_compile_definitions(hft_bench PRIVATE
        HFT_CANONICAL_BENCH_OUT="${CMAKE_SOURCE_DIR}/benchmarks/results/latest_bench.json"
        HFT_CANONICAL_LATENCY_OUT="${CMAKE_SOURCE_DIR}/benchmarks/results/latency_samples.csv"
    )

    add_executable(hft_microbench src/microbench.cpp)
    target_link_libraries(hft_microbench PRIVATE hft::core benchmark::benchmark)

    add_executable(hft_threaded_bench src/threaded_bench.cpp)
    target_link_libraries(hft_threaded_bench PRIVATE hft::core Threads::Threads)
endif()

set(HFT_INSTALL_TARGETS hft_core)
if(HFT_BUILD_SHARED_SDK)
    list(APPEND HFT_INSTALL_TARGETS hft_sdk)
endif()

if(TARGET hft_feed)
    list(APPEND HFT_INSTALL_TARGETS hft_feed)
endif()

install(TARGETS ${HFT_INSTALL_TARGETS}
    EXPORT HftSdkTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT HftSdkTargets
    FILE HftSdkTargets.cmake
    NAMESPACE HftSdk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HftSdk)

configure_package_config_file(
    cmake/HftSdkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HftSdkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HftSdk)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/HftSdkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/HftSdkConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/HftSdkConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HftSdk)

configure_file(cmake/hft-sdk.pc.in ${CMAKE_CURRENT_BINARY_DIR}/hft-sdk.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hft-sdk.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(HFT_BUILD_PYTHON)
    set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
    find_package(Python COMPONENTS Interpreter Development.Module QUIET)
    if(Python_FOUND)
        find_package(pybind11 CONFIG QUIET)
        if(NOT pybind11_FOUND)
            FetchContent_Declare(
                pybind11
                URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            )
            FetchContent_MakeAvailable(pybind11)
        endif()
        pybind11_add_module(hft_sdk_py python/bindings.cpp)
        if(TARGET hft_sdk)
            target_link_libraries(hft_sdk_py PRIVATE hft_sdk)
        else()
            target_link_libraries(hft_sdk_py PRIVATE hft::core)
            target_sources(hft_sdk_py PRIVATE src/sdk.cpp src/spsc_queue.cpp)
            target_compile_definitions(hft_sdk_py PRIVATE HFT_SDK_BUILD)
        endif()
        target_compile_definitions(hft_sdk_py PRIVATE
            HFT_SDK_VERSION="${PROJECT_VERSION}")
        set_target_properties(hft_sdk_py PROPERTIES
            OUTPUT_NAME "_core"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/hft_sdk")
        file(COPY
            python/hft_sdk/__init__.py
            python/hft_sdk/__main__.py
            python/hft_sdk/data.py
            python/hft_sdk/live.py
            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/hft_sdk")
        # The module installs as <prefix>/lib/pythonX.Y/site-packages/hft_sdk/,
        # while libhft_sdk lives in <prefix>/lib, hence three parent hops.
        if(APPLE)
            set_target_properties(hft_sdk_py PROPERTIES INSTALL_RPATH "@loader_path/../../..")
        elseif(UNIX)
            set_target_properties(hft_sdk_py PROPERTIES INSTALL_RPATH "$ORIGIN/../../..")
        endif()
        if(DEFINED PYTHON_VERSION)
            string(REGEX MATCH "^[0-9]+\\.[0-9]+" HFT_PYTHON_XY "${PYTHON_VERSION}")
        else()
            set(HFT_PYTHON_XY "${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}")
        endif()
        set(HFT_PYTHON_INSTALL_DIR
            "${CMAKE_INSTALL_LIBDIR}/python${HFT_PYTHON_XY}/site-packages/hft_sdk"
            CACHE STRING "Install directory for the hft_sdk Python package")
        message(STATUS "pybind11 module hft_sdk._core will install to ${HFT_PYTHON_INSTALL_DIR}")
        install(TARGETS hft_sdk_py
            LIBRARY DESTINATION ${HFT_PYTHON_INSTALL_DIR})
        install(FILES
            python/hft_sdk/__init__.py
            python/hft_sdk/__main__.py
            python/hft_sdk/data.py
            python/hft_sdk/live.py
            DESTINATION ${HFT_PYTHON_INSTALL_DIR})
    else()
        message(WARNING "Python development files not found; skipping pybind11 bindings")
    endif()
endif()
