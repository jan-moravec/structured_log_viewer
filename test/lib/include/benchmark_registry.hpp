/**
 * @brief Registration entry points of the library benchmarks.
 *
 * Each benchmark source file exposes one function that registers its benchmarks with Google Benchmark. The benchmark
 * executable calls them all from main, so nothing registers during static initialization and a filtered run builds
 * only the inputs of the benchmarks it runs.
 */
#pragma once

namespace bench
{

void RegisterCsvBenchmarks();
void RegisterJsonBenchmarks();

} // namespace bench
