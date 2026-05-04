#pragma once

#include <chrono>
#include <cstdlib>

namespace loglib_test
{

/// Read-once test-time multiplier for polling-fallback waits, so slow
/// CI runners can scale wait deadlines without touching every test.
/// Set via `LOGLIB_TEST_TIME_SCALE` (double, default 1.0). Fixed
/// constants like `pollInterval` are deliberately NOT scaled.
inline double LoadTimeScale() noexcept
{
    // MSVC C4996: `getenv` is "unsafe". The value is read once at
    // startup and parsed via `strtod` with a default-on-failure, so
    // the safer-API alternative would only add noise here.
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
