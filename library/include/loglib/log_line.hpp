#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_file.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace loglib
{

/// Microsecond-precision timestamp.
using TimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/// One field's value. The variant order is part of the on-disk-stable contract:
/// `string_view` is index 0, `string` is index 1, etc. New alternatives may be
/// added only at the end so existing `index()` consumers do not silently shift.
///
/// `string_view` lets the parser hand out values that point directly into the
/// memory-mapped log file. Owned `string` is kept for values that needed
/// unescaping or were promoted to a different type later.
using LogValue = std::variant<std::string_view, std::string, int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

/// Cold-path snapshot type that maps human-readable keys to log values. The
/// streaming pipeline never materialises one of these; it is for tests, debug
/// dumps, and the convenience constructor below.
using LogMap = std::unordered_map<std::string, LogValue>;

/// Tag that opts a `SetValue` caller into the `string_view` alternative without
/// tripping the debug-only contract assertion. Use only when the caller can
/// prove the view's bytes outlive the `LogLine` (e.g. point into the mmap).
struct LogValueTrustView
{
};

/// Returns the underlying string bytes if @p value carries a string-like
/// alternative (`string_view` or `string`), otherwise `std::nullopt`.
std::optional<std::string_view> AsStringView(const LogValue &value);

/// Returns true if @p value holds either alternative of the string family.
bool HoldsString(const LogValue &value);

/// Returns @p value with any `string_view` alternative converted to an owning
/// `string`, leaving every other alternative unchanged.
LogValue ToOwnedLogValue(const LogValue &value);

/// Returns true if @p lhs and @p rhs hold equivalent values, treating the two
/// string alternatives as equal when their bytes match.
bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs);

/// One log line / record, stored as a sorted-by-`KeyId` flat vector of
/// (KeyId, LogValue) pairs. The flat layout avoids per-line hash-map allocation
/// and turns field lookup into a tight contiguous loop.
class LogLine
{
public:
    /// Pre-sorted-by-`KeyId` constructor used by parsers. Caller must ensure
    /// `sortedValues` is sorted ascending on `pair::first`; debug builds
    /// assert this. The `keys` reference must outlive the `LogLine`.
    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LogFileReference fileReference);

    /// Cold-path convenience constructor that takes a `LogMap` snapshot and
    /// inserts/looks up each key in @p keys.
    LogLine(const LogMap &values, KeyIndex &keys, LogFileReference fileReference);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /// Returns the value associated with @p id, or `std::monostate` if the
    /// line does not carry that key.
    LogValue GetValue(KeyId id) const;

    /// Slow-path accessor: resolves @p key through the back-pointer to the
    /// `KeyIndex`, then delegates to the KeyId overload. Returns
    /// `std::monostate` if the key is unknown or not present on this line.
    LogValue GetValue(const std::string &key) const;

    /// Sets or updates the value for @p id. In debug builds, asserts that
    /// @p value is not a `string_view`; use the `LogValueTrustView` overload
    /// when the caller can guarantee the view outlives the line.
    void SetValue(KeyId id, LogValue value);

    /// `string_view`-friendly setter. Caller promises that any view in
    /// @p value points into storage that outlives the `LogLine`.
    void SetValue(KeyId id, LogValue value, LogValueTrustView trust);

    /// Slow-path setter that resolves @p key through the back-pointer.
    /// Throws if the key is unknown.
    void SetValue(const std::string &key, LogValue value);

    /// Returns the keys present on this line, in lexical order. Cold path.
    std::vector<std::string> GetKeys() const;

    /// Returns a read-only span over the (KeyId, LogValue) pairs in ascending
    /// KeyId order.
    std::span<const std::pair<KeyId, LogValue>> IndexedValues() const;

    /// Cold-path snapshot of the line's key/value pairs as a `LogMap`.
    LogMap Values() const;

    /// Replaces the back-pointer to the `KeyIndex`. Used by `LogData::Merge`.
    void RebindKeys(const KeyIndex &keys);

    /// Back-pointer to the `KeyIndex` used to look up keys.
    const KeyIndex &Keys() const;

    const LogFileReference &FileReference() const;
    LogFileReference &FileReference();

private:
    std::vector<std::pair<KeyId, LogValue>> mValues;
    const KeyIndex *mKeys = nullptr;
    LogFileReference mFileReference;
};

} // namespace loglib
