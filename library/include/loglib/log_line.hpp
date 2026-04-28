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

/// One field's value. The variant order is on-disk-stable: `string_view` is
/// index 0, `string` is index 1, etc. New alternatives may be added only at
/// the end. `string_view` lets the parser hand out values pointing directly
/// into the memory-mapped log file.
using LogValue =
    std::variant<std::string_view, std::string, int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

/// Cold-path key->value map for tests and debug dumps.
using LogMap = std::unordered_map<std::string, LogValue>;

/// Tag opting a `SetValue` caller into `string_view` storage. Use only when
/// the view's bytes outlive the `LogLine` (e.g. point into the mmap).
struct LogValueTrustView
{
};

std::optional<std::string_view> AsStringView(const LogValue &value);
bool HoldsString(const LogValue &value);
LogValue ToOwnedLogValue(const LogValue &value);

/// Treats `string_view` and `string` alternatives as equal when bytes match.
bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs);

/// One log record, stored as a sorted-by-`KeyId` flat vector of (KeyId, value)
/// pairs.
class LogLine
{
public:
    /// Pre-sorted ctor used by parsers. `sortedValues` must be ascending on
    /// `pair::first` (debug-asserted). @p keys must outlive the `LogLine`.
    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LogFileReference fileReference);

    /// Cold-path convenience ctor.
    LogLine(const LogMap &values, KeyIndex &keys, LogFileReference fileReference);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /// Returns `std::monostate` if @p id is not present on this line.
    LogValue GetValue(KeyId id) const;
    LogValue GetValue(const std::string &key) const;

    /// Debug builds assert that @p value is not a `string_view`.
    void SetValue(KeyId id, LogValue value);

    /// Caller promises any view in @p value outlives the `LogLine`.
    void SetValue(KeyId id, LogValue value, LogValueTrustView trust);

    /// Throws if @p key is unknown.
    void SetValue(const std::string &key, LogValue value);

    std::vector<std::string> GetKeys() const;

    /// (KeyId, LogValue) pairs in ascending KeyId order.
    std::span<const std::pair<KeyId, LogValue>> IndexedValues() const;

    LogMap Values() const;

    /// Used by `LogData::Merge`.
    void RebindKeys(const KeyIndex &keys);

    const KeyIndex &Keys() const;

    const LogFileReference &FileReference() const;
    LogFileReference &FileReference();

private:
    std::vector<std::pair<KeyId, LogValue>> mValues;
    const KeyIndex *mKeys = nullptr;
    LogFileReference mFileReference;
};

} // namespace loglib
