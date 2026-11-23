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
void RegisterDecompressionBenchmarks();
void RegisterExportBenchmarks();
void RegisterTimeHistogramBenchmarks();
void RegisterJsonBenchmarks();
void RegisterRowFilterBenchmarks();
void RegisterLogfmtBenchmarks();
void RegisterMemoryBenchmarks();
void RegisterNetworkBenchmarks();
void RegisterRegexBenchmarks();
void RegisterSessionBundleBenchmarks();
void RegisterStreamBenchmarks();
void RegisterTableBenchmarks();
void RegisterTimeConversionBenchmarks();
void RegisterTimestampBenchmarks();

} // namespace bench
