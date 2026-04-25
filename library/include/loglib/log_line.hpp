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

/**
 * @brief Represents a timestamp for a log entry.
 *
 * The timestamp is stored with a precision of microseconds, which is generally
 * the highest precision achievable on modern systems for inter-process communication.
 */
using TimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/**
 * @brief Represents a value in a log entry.
 *
 * The variant order is part of the on-disk-stable contract (PRD req. 4.1.6 /
 * Decision 30): `std::string_view` is alternative `0`, `std::string` is `1`,
 * etc. Adding new alternatives is allowed only at the end so existing
 * `std::variant::index()` consumers do not silently shift.
 *
 * `std::string_view` lets the parser hand out values that point directly into
 * the memory-mapped log file, avoiding a `std::string` copy per field. Owned
 * `std::string` is still kept for values that needed unescaping (the parser
 * falls back to it for keys/values containing backslashes) and for values
 * promoted to a different type later (e.g. an originally-string timestamp
 * promoted to `TimeStamp`).
 */
using LogValue = std::variant<std::string_view, std::string, int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

/**
 * @brief Cold-path snapshot type that maps human-readable keys to log values.
 *
 * The streaming pipeline never materialises a `LogMap`; it builds the
 * KeyId-indexed pair vector directly. `LogMap` is kept as a public alias for
 * cold paths (debug dumps, configuration UI, tests, ad-hoc value collection
 * before a `KeyIndex` is available) and for the convenience constructor below.
 */
using LogMap = std::unordered_map<std::string, LogValue>;

/**
 * @brief Tag type that opts a `SetValue(KeyId, LogValue, LogValueTrustView)` caller
 *        into the `std::string_view` value alternative without tripping the
 *        debug-only contract assertion that `SetValue(KeyId, LogValue)` adds.
 *
 * Use only when the caller can prove the view points into the log file's mmap
 * (i.e. the lifetime is tied to the owning `LogFile`). The streaming JSON
 * parser is the canonical caller.
 */
struct LogValueTrustView
{
};

/**
 * @brief Returns the underlying string bytes if @p value carries a string-like
 *        alternative (`std::string_view` or `std::string`), otherwise
 *        `std::nullopt`.
 *
 * This is the recommended accessor for code that wants to read string values
 * regardless of whether the parser emitted them as a view or as an owned copy
 * (PRD req. 4.1.6).
 */
std::optional<std::string_view> AsStringView(const LogValue &value);

/**
 * @brief Returns true if @p value holds either alternative of the string family
 *        (`std::string_view` or `std::string`).
 */
bool HoldsString(const LogValue &value);

/**
 * @brief Returns @p value with any `std::string_view` alternative converted to
 *        an owning `std::string`, leaving every other alternative unchanged.
 *
 * Used at sink boundaries where the caller wants to detach the value from the
 * mmap-backed lifetime (e.g. before writing it back over a stream).
 */
LogValue ToOwnedLogValue(const LogValue &value);

/**
 * @brief Returns true if @p lhs and @p rhs are equivalent log values.
 *
 * Equivalent under this relation if either:
 * - they hold the same alternative and compare equal under `operator==`, or
 * - they hold different string alternatives (`string_view` / `string`) but the
 *   underlying bytes compare byte-equal.
 *
 * Used by the parity test (`Parallel parse parity vs. single-thread`) to
 * compare the single-thread and pipeline outputs without caring whether each
 * value happened to land in the view or owned alternative.
 */
bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs);

/**
 * @brief Represents a single line in a log or a single log record, consisting
 *        of key-value pairs stored as a sorted-by-`KeyId` flat vector.
 *
 * The flat-vector representation (vs. a hash map per line) is the single
 * largest source of memory savings in the streaming refactor: every line saves
 * at least a `tsl::robin_map` allocation, and field lookups become a tight
 * loop over a small contiguous run of pairs. See PRD req. 4.1.5–4.1.10 and
 * §4.1.8 for the linear-vs-binary search benchmarking note.
 */
class LogLine
{
public:
    /**
     * @brief Constructs a LogLine from a *pre-sorted-by-`KeyId`* vector of pairs.
     *
     * The caller must sort `sortedValues` by `pair::first` (ascending). In
     * debug builds an assertion enforces this; in release the contract is
     * silently trusted to keep the parser hot path branch-free. The
     * `keys` reference must outlive the `LogLine`.
     */
    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LogFileReference fileReference);

    /**
     * @brief Cold-path convenience constructor that takes a `LogMap` snapshot
     *        and inserts/looks up each key in @p keys.
     *
     * Provided so test code, ad-hoc tooling and any caller that already has a
     * `LogMap` can build a `LogLine` without spelling out the
     * `(KeyId, LogValue)` vector by hand. The hot-path streaming parser uses
     * the pre-sorted constructor instead — this one allocates a temporary
     * vector and sorts it, which is fine outside the parser.
     */
    LogLine(const LogMap &values, KeyIndex &keys, LogFileReference fileReference);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /**
     * @brief Returns the value associated with @p id, or `std::monostate` if
     *        the line does not carry that key.
     *
     * Performs a linear scan with an early exit when the current entry's id
     * already exceeds @p id. For realistic field counts (5–50) the linear scan
     * is competitive with or faster than `std::lower_bound` due to better
     * branch prediction and zero comparison-function overhead — this is the
     * `[get_value_micro]` benchmark (PRD §4.1.8).
     */
    LogValue GetValue(KeyId id) const;

    /**
     * @brief Slow-path accessor: looks up the canonical KeyId via the back-pointer
     *        to the `KeyIndex`, then delegates to the KeyId overload.
     *
     * Returns `std::monostate` if the key is not registered or not present on
     * this line. Provided for compatibility with callers that still hold raw
     * key strings (e.g. configuration code, tests).
     */
    LogValue GetValue(const std::string &key) const;

    /**
     * @brief Sets or updates the value for a given KeyId.
     *
     * In debug builds, asserts that @p value is *not* the `std::string_view`
     * alternative. Pipeline code that wants to inject a view value without
     * paying the copy cost should use the `LogValueTrustView`-tagged overload
     * to make the lifetime promise explicit at the call site.
     */
    void SetValue(KeyId id, LogValue value);

    /**
     * @brief Sets or updates the value for a given KeyId without the
     *        anti-`string_view` debug assertion (see `LogValueTrustView`).
     *
     * The caller promises that any `std::string_view` carried by @p value
     * points into storage that outlives the `LogLine`.
     */
    void SetValue(KeyId id, LogValue value, LogValueTrustView trust);

    /**
     * @brief Slow-path setter: looks up the canonical KeyId via the back-pointer
     *        to the `KeyIndex` (inserting a new id only if the back-pointer is
     *        non-`const` — which it currently is not, so this overload throws
     *        if the key is unknown).
     */
    void SetValue(const std::string &key, LogValue value);

    /**
     * @brief Returns the keys present on this line, in lexical order of the
     *        registered key strings.
     *
     * Slow-path accessor for callers that want a `std::vector<std::string>`
     * snapshot. The hot-path equivalent is `IndexedValues()`.
     */
    std::vector<std::string> GetKeys() const;

    /**
     * @brief Returns a read-only span over the (KeyId, LogValue) pairs in
     *        ascending KeyId order.
     */
    std::span<const std::pair<KeyId, LogValue>> IndexedValues() const;

    /**
     * @brief Cold-path snapshot of the line's key/value pairs as a `LogMap`.
     *
     * Allocates and copies; use only outside hot paths (debug dumps,
     * configuration UI, tests). The hot-path equivalents are `IndexedValues()`
     * and `GetValue(KeyId)`.
     */
    LogMap Values() const;

    /**
     * @brief Replaces the back-pointer to the `KeyIndex`.
     *
     * Used by `LogData::Merge` when relocating a `LogLine` from one
     * `LogData`'s key dictionary into another.
     */
    void RebindKeys(const KeyIndex &keys);

    /**
     * @brief Returns the back-pointer to the `KeyIndex` used to look up keys.
     */
    const KeyIndex &Keys() const;

    /**
     * @brief Retrieves the file reference associated with this log line.
     */
    const LogFileReference &FileReference() const;

    /**
     * @brief Mutable file reference accessor used by Stage C of the streaming
     *        pipeline to back-fill the absolute line number after Stage B
     *        emitted the line with a placeholder.
     */
    LogFileReference &FileReference();

private:
    std::vector<std::pair<KeyId, LogValue>> mValues;
    const KeyIndex *mKeys = nullptr;
    LogFileReference mFileReference;
};

} // namespace loglib
