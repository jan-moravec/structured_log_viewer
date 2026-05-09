#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_value.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

class LineSource;

/// One log record, stored as a sorted-by-`KeyId` flat vector of
/// `(KeyId, internal::CompactLogValue)` pairs. Compact values are 16 B
/// (vs ~48 B for `LogValue`); strings are `(offset, length)` pairs
/// resolved through the owning `LineSource` (mmap or per-line arena).
///
/// `(LineSource*, lineId)` is the session-wide row identity: `lineId`
/// is in the source's id space (file: 0-based; stream: 1-based
/// monotonic from `AppendLine`). `GetValue` dispatches through the
/// source's virtuals — no static-vs-streaming branch in the ABI.
class LogLine
{
public:
    /// Cold-path ctor: convert a public-variant value list into
    /// compact storage. Owned strings are appended to the source's
    /// arena; views inside `StableBytes()` are stored zero-copy as
    /// `MmapSlice`.
    LogLine(
        std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LineSource &source, size_t lineId
    );

    /// Hot-path ctor: pre-built compact values, ascending on
    /// `pair::first`. `OwnedString` payloads must already be relative
    /// to the arena that ultimately owns them.
    LogLine(
        std::vector<std::pair<KeyId, internal::CompactLogValue>> sortedValues,
        const KeyIndex &keys,
        LineSource &source,
        size_t lineId
    );

    /// Cold-path convenience ctor.
    LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /// Returns `std::monostate` if @p id is not present on this line.
    LogValue GetValue(KeyId id) const;
    LogValue GetValue(const std::string &key) const;

    /// Debug builds assert @p value is not a `string_view`. Owned
    /// strings are appended to the source's arena.
    void SetValue(KeyId id, const LogValue &value);

    /// Caller promises any view in @p value outlives the `LogLine`.
    /// Views inside `StableBytes()` are stored zero-copy; others are
    /// copied into the arena.
    void SetValue(KeyId id, const LogValue &value, LogValueTrustView trust);

    /// Throws if @p key is unknown.
    void SetValue(const std::string &key, const LogValue &value);

    /// Replace (or insert) the slot for @p id with a `DictRef` for
    /// @p vid. Caller owns the dictionary lifetime. Cold-path
    /// helper: the encode hot path in `LogTable::EncodeColumnRange`
    /// goes through `FindCompactMutable` and writes the slot in
    /// place to skip the `KeyId` lookup performed here.
    void SetOrReplaceEnumDictRef(KeyId id, EnumValueId vid);

    std::vector<std::string> GetKeys() const;

    /// (KeyId, LogValue) pairs in ascending KeyId order. Materialises
    /// every compact value; cold path.
    std::vector<std::pair<KeyId, LogValue>> IndexedValues() const;

    /// Internal: span over the compact storage. Hot-path walkers
    /// (parser pipeline, `LogData::Merge`) use this directly.
    std::span<const std::pair<KeyId, internal::CompactLogValue>> CompactValues() const noexcept;

    LogMap Values() const;

    /// Used by `LogData::Merge` and `LogData` move-ops.
    void RebindKeys(const KeyIndex &keys);

    const KeyIndex &Keys() const;

    /// The owning `LineSource`. Never null post-construction.
    [[nodiscard]] LineSource *Source() const noexcept;

    /// Line id within the source's id space. File: 0-based; stream:
    /// 1-based monotonic.
    [[nodiscard]] size_t LineId() const noexcept;

    /// Stage C shifts per-batch relative ids to absolute.
    void ShiftLineId(size_t delta) noexcept;

    /// Cold-path mutator (e.g. overwrite a placeholder id).
    void SetLineId(size_t lineId) noexcept;

    /// Heap bytes owned directly by this line (capacity of `mValues`).
    /// Owned-string bytes live in the source's arena. Benchmark-only.
    size_t OwnedMemoryBytes() const;

    /// Number of compact values stored.
    size_t ValueCount() const noexcept;

    /// Add @p delta to every `OwnedString` payload. Used by parser
    /// Stage C / `BufferingSink::OnBatch` when concatenating per-batch
    /// arenas onto the canonical source arena.
    void RebaseOwnedStringOffsets(uint64_t delta) noexcept;

    /// True when @p id is stored as `MmapSlice` (zero-copy fast path).
    bool IsMmapSlice(KeyId id) const noexcept;

    /// True when @p id is stored as `OwnedString` (escape-decoded).
    bool IsOwnedString(KeyId id) const noexcept;

    /// True when @p id is stored as `DictRef`.
    bool IsDictRef(KeyId id) const noexcept;

    /// `EnumValueId` payload for a `DictRef` slot, else nullopt.
    /// Powers the model's `EnumValueRole` fast-filter path.
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(KeyId id) const noexcept;

    /// Linear forward scan over the (sorted) compact storage; returns
    /// nullptr if @p id is absent. Exposed for hot paths
    /// (`LogTable`'s enum encode/demote loops) that want to combine
    /// the lookup with a tag-dispatch + in-place update without
    /// repeating the scan.
    [[nodiscard]] const internal::CompactLogValue *FindCompact(KeyId id) const noexcept;

    /// Mutable counterpart of `FindCompact`. Same lookup semantics.
    /// Caller may overwrite `*slot` (e.g. with
    /// `CompactLogValue::MakeDictRef`) without a second binary search.
    [[nodiscard]] internal::CompactLogValue *FindCompactMutable(KeyId id) noexcept;

    /// Resolve a `MmapSlice` or `OwnedString` slot to its underlying
    /// bytes without constructing a `LogValue`. Returns `std::nullopt`
    /// for any other tag (`Monostate`, `DictRef`, numeric, timestamp)
    /// or when the slot is absent. `nullptr` source is safe.
    ///
    /// Used by `LogTable`'s enum candidate scan to skip non-string
    /// slots before paying for a `LogValue` variant construction.
    [[nodiscard]] std::optional<std::string_view> PeekStringView(KeyId id) const noexcept;

    /// Slot-pointer overload: callers that already located the slot
    /// (e.g. via `FindCompactMutable` in the enum encode loop, or via
    /// `FindCompact` in the candidate scan) can skip the second
    /// linear scan.
    [[nodiscard]] std::optional<std::string_view> PeekStringView(const internal::CompactLogValue &slot) const noexcept;

private:
    /// Replace (or insert) the slot for @p id with @p compact. Shared
    /// by `SetValue` and `SetOrReplaceEnumDictRef`. May allocate.
    void SetCompact(KeyId id, internal::CompactLogValue compact);

    internal::CompactLineFields mValues;
    const KeyIndex *mKeys = nullptr;
    LineSource *mSource = nullptr;
    size_t mLineId = 0;
};

} // namespace loglib
