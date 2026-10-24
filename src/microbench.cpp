#include <benchmark/benchmark.h>
#include <xsimd/xsimd.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int factorial_constexpr(int value) {
    return value <= 1 ? 1 : value * factorial_constexpr(value - 1);
}

int factorial_runtime(int value) {
    int result = 1;
    for (int index = 2; index <= value; ++index) {
        result *= index;
    }
    return result;
}

void BM_ConstexprFactorial(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(factorial_constexpr(10));
    }
}
BENCHMARK(BM_ConstexprFactorial)->Name("Micro/Constexpr");

void BM_RuntimeFactorial(benchmark::State& state) {
    volatile int input = 10;
    for (auto _ : state) {
        benchmark::DoNotOptimize(factorial_runtime(input));
    }
}
BENCHMARK(BM_RuntimeFactorial)->Name("Micro/RuntimeFactorial");

void BM_LoopPlain(benchmark::State& state) {
    std::vector<int> values(4096, 1);
    for (auto _ : state) {
        std::int64_t total = 0;
        for (const int value : values) {
            total += value;
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_LoopPlain)->Name("Micro/LoopPlain");

void BM_LoopUnrolled(benchmark::State& state) {
    std::vector<int> values(4096, 1);
    for (auto _ : state) {
        std::int64_t total = 0;
        std::size_t index = 0;
        for (; index + 4 <= values.size(); index += 4) {
            total += values[index];
            total += values[index + 1];
            total += values[index + 2];
            total += values[index + 3];
        }
        for (; index < values.size(); ++index) {
            total += values[index];
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_LoopUnrolled)->Name("Micro/LoopUnrolled");

void BM_NoPrefetch(benchmark::State& state) {
    std::vector<int> values(1 << 20, 1);
    for (auto _ : state) {
        std::int64_t total = 0;
        for (const int value : values) {
            total += value;
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_NoPrefetch)->Name("Micro/NoPrefetch");

void BM_WithPrefetch(benchmark::State& state) {
    std::vector<int> values(1 << 20, 1);
    for (auto _ : state) {
        std::int64_t total = 0;
        std::size_t index = 0;
        for (; index + 16 < values.size(); ++index) {
            __builtin_prefetch(&values[index + 16], 0, 3);
            total += values[index];
        }
        for (; index < values.size(); ++index) {
            total += values[index];
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_WithPrefetch)->Name("Micro/WithPrefetch");

void BM_SignedCompare(benchmark::State& state) {
    volatile std::int32_t value = -1;
    for (auto _ : state) {
        benchmark::DoNotOptimize(value < 0);
    }
}
BENCHMARK(BM_SignedCompare)->Name("Micro/SignedCompare");

void BM_UnsignedCompare(benchmark::State& state) {
    volatile std::uint32_t value = 1;
    for (auto _ : state) {
        benchmark::DoNotOptimize(value > 0);
    }
}
BENCHMARK(BM_UnsignedCompare)->Name("Micro/UnsignedCompare");

void BM_FloatOnly(benchmark::State& state) {
    float value = 1.0f;
    for (auto _ : state) {
        value = value * 1.0000001f + 0.5f;
        benchmark::DoNotOptimize(value);
    }
}
BENCHMARK(BM_FloatOnly)->Name("Micro/FloatOnly");

void BM_MixedFloatDouble(benchmark::State& state) {
    float value = 1.0f;
    for (auto _ : state) {
        value = static_cast<float>(static_cast<double>(value) * 1.0000001 + 0.5);
        benchmark::DoNotOptimize(value);
    }
}
BENCHMARK(BM_MixedFloatDouble)->Name("Micro/MixedFloatDouble");

__attribute__((always_inline)) inline int add_inline(int left, int right) {
    return left + right;
}

__attribute__((noinline)) int add_noinline(int left, int right) {
    return left + right;
}

void BM_AlwaysInline(benchmark::State& state) {
    int value = 0;
    for (auto _ : state) {
        value = add_inline(value, 1);
        benchmark::DoNotOptimize(value);
    }
}
BENCHMARK(BM_AlwaysInline)->Name("Micro/AlwaysInline");

void BM_NoInline(benchmark::State& state) {
    int value = 0;
    for (auto _ : state) {
        value = add_noinline(value, 1);
        benchmark::DoNotOptimize(value);
    }
}
BENCHMARK(BM_NoInline)->Name("Micro/NoInline");

void BM_BranchBaseline(benchmark::State& state) {
    std::int64_t total = 0;
    for (auto _ : state) {
        for (int index = 0; index < 1024; ++index) {
            if (index % 10 == 0) {
                total += 3;
            } else {
                total += 1;
            }
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_BranchBaseline)->Name("Micro/BranchBaseline");

void BM_BranchReduced(benchmark::State& state) {
    std::int64_t total = 0;
    for (auto _ : state) {
        for (int index = 0; index < 1024; ++index) {
            total += 1 + 2 * static_cast<std::int64_t>(index % 10 == 0);
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_BranchReduced)->Name("Micro/BranchReduced");

void BM_ArrayScalar(benchmark::State& state) {
    std::vector<float> left(4096, 1.0f);
    std::vector<float> right(4096, 2.0f);
    std::vector<float> output(4096, 0.0f);
    for (auto _ : state) {
        for (std::size_t index = 0; index < left.size(); ++index) {
            output[index] = left[index] + right[index];
        }
        benchmark::DoNotOptimize(output.data());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ArrayScalar)->Name("Micro/ArrayScalar");

void BM_ArraySimd(benchmark::State& state) {
    using batch = xsimd::batch<float>;
    std::vector<float> left(4096, 1.0f);
    std::vector<float> right(4096, 2.0f);
    std::vector<float> output(4096, 0.0f);
    for (auto _ : state) {
        std::size_t index = 0;
        for (; index + batch::size <= left.size(); index += batch::size) {
            const batch a = batch::load_unaligned(left.data() + index);
            const batch b = batch::load_unaligned(right.data() + index);
            (a + b).store_unaligned(output.data() + index);
        }
        for (; index < left.size(); ++index) {
            output[index] = left[index] + right[index];
        }
        benchmark::DoNotOptimize(output.data());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ArraySimd)->Name("Micro/ArraySimd");


void BM_PairsRollingScalar(benchmark::State& state) {
    constexpr std::size_t count = 4096;
    constexpr std::size_t window = 256;
    std::vector<double> values(count);
    for (std::size_t index = 0; index < count; ++index) {
        values[index] = std::sin(static_cast<double>(index) * 0.01);
    }
    for (auto _ : state) {
        double total = 0.0;
        for (std::size_t index = window; index < count; ++index) {
            double sum = 0.0;
            double squares = 0.0;
            for (std::size_t offset = 0; offset < window; ++offset) {
                const double value = values[index - offset];
                sum += value;
                squares += value * value;
            }
            total += sum + squares;
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_PairsRollingScalar)->Name("Micro/PairsRollingScalar");

void BM_PairsRollingSimd(benchmark::State& state) {
    using batch = xsimd::batch<double>;
    constexpr std::size_t count = 4096;
    constexpr std::size_t window = 256;
    std::vector<double> values(count);
    for (std::size_t index = 0; index < count; ++index) {
        values[index] = std::sin(static_cast<double>(index) * 0.01);
    }
    for (auto _ : state) {
        double total = 0.0;
        for (std::size_t index = window; index < count; ++index) {
            batch sum(0.0);
            batch squares(0.0);
            for (std::size_t offset = 0; offset + batch::size <= window;
                 offset += batch::size) {
                const batch value = batch::load_unaligned(
                    values.data() + index - offset - batch::size + 1);
                sum += value;
                squares += value * value;
            }
            total += xsimd::reduce_add(sum) + xsimd::reduce_add(squares);
        }
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_PairsRollingSimd)->Name("Micro/PairsRollingSimd");

} // namespace

BENCHMARK_MAIN();
