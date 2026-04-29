#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace loglib
{

class StopSource;

/// Cooperative cancellation token. API mirrors `std::stop_token` (the only
/// surface we use is `stop_requested()` / `stop_possible()`), so call sites
/// read identically to the standard.
///
/// We ship our own type rather than depending on `std::stop_token` because
/// Apple's libc++ gates the standard type behind availability annotations on
/// some toolchain/SDK combinations (the type is removed from `namespace std`
/// entirely under `_LIBCPP_AVAILABILITY_HAS_SYNC == 0`), which produced hard
/// "no type named 'stop_token' in namespace 'std'" failures on the GitHub
/// Actions macOS runner. The implementation is a `shared_ptr<atomic<bool>>`
/// so a default-constructed token is unstoppable (matches `std::stop_token`).
class StopToken
{
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return mState != nullptr && mState->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept
    {
        return mState != nullptr;
    }

private:
    friend class StopSource;

    explicit StopToken(std::shared_ptr<std::atomic<bool>> state) noexcept : mState(std::move(state))
    {
    }

    std::shared_ptr<std::atomic<bool>> mState;
};

/// Cooperative cancellation source paired with `loglib::StopToken`. API
/// mirrors `std::stop_source` for the methods we use (`get_token()`,
/// `request_stop()`, `stop_requested()`). Default construction creates an
/// active source with shared state, matching `std::stop_source` (rather than
/// the `std::nostopstate` overload).
class StopSource
{
public:
    StopSource() : mState(std::make_shared<std::atomic<bool>>(false))
    {
    }

    [[nodiscard]] StopToken get_token() const noexcept
    {
        return StopToken{mState};
    }

    /// Returns `true` if this call flipped the state from "no stop requested"
    /// to "stop requested" (matches `std::stop_source::request_stop`).
    bool request_stop() noexcept
    {
        if (mState == nullptr)
        {
            return false;
        }
        return !mState->exchange(true, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return mState != nullptr && mState->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept
    {
        return mState != nullptr;
    }

private:
    std::shared_ptr<std::atomic<bool>> mState;
};

} // namespace loglib
