// Entry point of the library benchmarks. It refuses to run unoptimized builds, registers every benchmark, runs the
// ones selected by the command line and exits non-zero when a benchmark reported a wrong result.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"

#include <benchmark/benchmark.h>

#include <cstdio>
#include <exception>
#include <iostream>

namespace
{
// CTest reports this exit code as skipped.
constexpr int SKIPPED_EXIT_CODE = 4;
} // namespace

int main(int argc, char **argv)
{
#ifndef NDEBUG
    std::fputs(
        "Benchmarks require a release build (Debug disables IPO/LTO and leaves assertions enabled, so numbers are not "
        "comparable). Rebuild with: cmake --preset release  (or relwithdebinfo).\n",
        stderr
    );
    return SKIPPED_EXIT_CODE;
#else
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
    {
        return 1;
    }

    bench::RegisterCsvBenchmarks();
    bench::RegisterDecompressionBenchmarks();
    bench::RegisterHistogramBucketIndexBenchmarks();
    bench::RegisterJsonBenchmarks();
    bench::RegisterLogFilterBenchmarks();
    bench::RegisterLogfmtBenchmarks();
    bench::RegisterRegexBenchmarks();
    bench::RegisterSessionBundleBenchmarks();
    bench::RegisterStreamBenchmarks();

    try
    {
        benchmark::RunSpecifiedBenchmarks();
    }
    catch (const std::exception &e)
    {
        // An input that cannot be built is a failed run, not a crash.
        std::cerr << "benchmark setup failed: " << e.what() << '\n';
        return 1;
    }
    benchmark::Shutdown();
    return bench::AnyFailure() ? 1 : 0;
#endif
}
