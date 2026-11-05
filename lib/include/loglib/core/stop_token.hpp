/**
 * @brief Cooperative cancellation token and its source.
 *
 * Long operations such as parsing, filtering and exporting check a StopToken and stop early when it is triggered. The
 * API follows std::stop_token but is implemented here, because some standard libraries restrict the standard type by
 * platform version. The header also defines the exception a cancelled operation throws and the row progress callback
 * shared by exporting and bundle writing.
 *
 * This is part of the core module.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace loglib
{

class StopSource;

/**
 * @brief Read side of a cancellation request. Hand it to an operation, which polls it.
 *
 * It mirrors std::stop_token for the two calls the library uses. It is a separate type because some Apple toolchains do
 * not provide std::stop_token. A default-constructed token can never be stopped, so pass it where cancellation is not
 * wanted.
 */
class StopToken
{
public:
    StopToken() noexcept = default;

    /** @brief True once the paired StopSource has requested a stop. */
    [[nodiscard]] bool StopRequested() const noexcept
    {
        return mState != nullptr && mState->load(std::memory_order_acquire);
    }

    /** @brief False for a default-constructed token, which has no source and never stops. */
    [[nodiscard]] bool StopPossible() const noexcept
    {
        return mState != nullptr;
    }

private:
    friend class StopSource;

    explicit StopToken(std::shared_ptr<std::atomic<bool>> state) noexcept
        : mState(std::move(state))
    {
    }

    std::shared_ptr<std::atomic<bool>> mState;
};

/**
 * @brief Write side of a cancellation request. Keep it, give its tokens to the operation, and call RequestStop to
 * cancel.
 *
 * It mirrors std::stop_source for the calls the library uses. A new source is ready to use. Its tokens share one flag,
 * which is safe to read and set from any thread.
 */
class StopSource
{
public:
    StopSource()
        : mState(std::make_shared<std::atomic<bool>>(false))
    {
    }

    /** @brief Returns a token that observes this source. */
    [[nodiscard]] StopToken GetToken() const noexcept
    {
        return StopToken{mState};
    }

    /** @brief Requests a stop. Returns true only for the call that actually changed the state. */
    bool RequestStop() noexcept
    {
        if (mState == nullptr)
        {
            return false;
        }
        return !mState->exchange(true, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool StopRequested() const noexcept
    {
        return mState != nullptr && mState->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool StopPossible() const noexcept
    {
        return mState != nullptr;
    }

private:
    std::shared_ptr<std::atomic<bool>> mState;
};

/**
 * @brief Thrown by a long operation that stopped because its StopToken fired.
 *
 * Deliberately not a loglib::Error or a std::runtime_error, so error handlers do not swallow a cancellation. Decompression,
 * exporting and bundle writing all throw it.
 */
class OperationCancelled : public std::exception
{
public:
    explicit OperationCancelled(std::string message = "Operation cancelled")
        : mMessage(std::move(message))
    {
    }

    [[nodiscard]] const char *what() const noexcept override
    {
        return mMessage.c_str();
    }

private:
    std::string mMessage;
};

/**
 * @brief Reports how many rows an operation has handled out of a total.
 *
 * Used by exporting and bundle writing. The counts only grow. Cancellation goes through the StopToken, not through the
 * return value.
 */
using RowProgressCallback = std::function<void(std::uint64_t rowsDone, std::uint64_t rowsTotal)>;

} // namespace loglib
