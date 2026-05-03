#pragma once

#include <chrono>
#include <cstdlib>

namespace loglib_test
{

/// Read-once test-time multiplier for polling-fallback waits. Slow CI
/// runners (Linux containerised, Windows MSVC matrix, macOS shared arm
/// runners) can blow past the wall-clock budgets the harness picks for
/// a developer-class workstation; rather than scattering ad-hoc `*= 2`
/// factors through every `DrainUntil` / `wait_for` call, we read the
/// `LOGLIB_TEST_TIME_SCALE` env var once, parse it as a double (default
/// `1.0`), and multiply every test-only deadline through `ScaledMs`.
/// Fixed constants like `pollInterval` deliberately do **not** scale --
/// only the deadlines waiting for the worker to do its thing.
inline double LoadTimeScale() noexcept
{
    // MSVC flags `std::getenv` as `unsafe` (C4996) and recommends
    // `_dupenv_s`; the value here is read once at process start and
    // treated as untrusted input (parsed via `strtod`, default-on-
    // failure), so the safer-API pattern would only add noise. Suppress
    // the warning locally rather than blanket-disable C4996 for the TU.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *raw = std::getenv("LOGLIB_TEST_TIME_SCALE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!raw || !*raw)
    {
        return 1.0;
    }
    char *end = nullptr;
    const double parsed = std::strtod(raw, &end);
    if (end == raw || parsed <= 0.0)
    {
        return 1.0;
    }
    return parsed;
}

/// Cached test-time multiplier — read the env var once per process.
inline double TimeScale() noexcept
{
    static const double kScale = LoadTimeScale();
    return kScale;
}

/// Scale a `std::chrono::milliseconds` deadline by
/// `LOGLIB_TEST_TIME_SCALE`, rounding so a tight 25 ms budget never
/// collapses to 0 on a 0.5x scale (we never shrink below 1 ms).
inline std::chrono::milliseconds ScaledMs(std::chrono::milliseconds base) noexcept
{
    const double scaled = static_cast<double>(base.count()) * TimeScale();
    const auto rounded = static_cast<long long>(scaled + 0.5);
    return std::chrono::milliseconds(rounded < 1 ? 1 : rounded);
}

} // namespace loglib_test
