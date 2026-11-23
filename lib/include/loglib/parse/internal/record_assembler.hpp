/**
 * @brief The record that is still open while continuation lines may join it.
 *
 * A record is one row that can span several physical lines: a header line decoded by a format, followed by indented or
 * unmatched lines that belong to one of its fields. RecordAssembler holds that open record for both drivers, the file
 * pipeline and the live-stream loop, so the rules for continuation text live in one place. It keeps the decoded values,
 * the lines the record covers, the continuation text collected so far and the blank lines that count only when another
 * continuation follows. When the record is complete the driver asks it to join the text into the target field and turns
 * the values into a row.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/parse/internal/line_decoder.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Adds continuation text, with a separator in front, to a field of a record.
 *
 * A value that lives in the mapped file needs mmapBytes and is first copied into ownedArena. A field that has no text
 * yet, such as a bare key=, gets the text without a separator.
 */
ContinuationSpliceOutcome ExtendContinuationTarget(
    std::vector<CompactLogValue> &values,
    std::string &ownedArena,
    KeyId targetKey,
    std::string_view continuationBytes,
    std::string_view mmapBytes = std::string_view{}
);

/**
 * @brief Tells whether continuation text can be added to a field, and why not when it cannot.
 *
 * A text field, a field with no value and a slice of the mapped file can take text. A missing field or a number cannot.
 */
ContinuationSpliceOutcome CheckContinuationTarget(std::span<const CompactLogValue> values, KeyId targetKey);

/** @brief The record a driver holds open until the next header, the end of the input or its age limit seals it. */
class RecordAssembler
{
public:
    /**
     * @brief Creates an assembler.
     *
     * With keepRawText it also keeps the lines of the continuation, with the blank lines between them, for a driver
     * that stores the raw text of every row.
     */
    explicit RecordAssembler(bool keepRawText = false) noexcept;

    /**
     * @brief Opens a record and takes its values. The assembler must be closed.
     *
     * target is the field that continuation lines extend. When it cannot take text, the continuation lines are counted
     * and dropped, so memory stays bounded.
     */
    void Open(std::vector<CompactLogValue> &values, size_t firstLine, KeyId target);

    [[nodiscard]] bool IsOpen() const noexcept
    {
        return mOpen;
    }

    /** @brief The line where the record starts. */
    [[nodiscard]] size_t FirstLine() const noexcept
    {
        return mFirstLine;
    }

    /** @brief The last line that holds text of the record: the header, or the last continuation line that was kept. */
    [[nodiscard]] size_t LastLine() const noexcept
    {
        return mLastLine;
    }

    [[nodiscard]] KeyId Target() const noexcept
    {
        return mTarget;
    }

    /** @brief Ok when continuation text can be kept, otherwise why it is dropped. Valid while the record is open. */
    [[nodiscard]] ContinuationSpliceOutcome TargetStatus() const
    {
        EnsureTargetStatus();
        return mTargetStatus;
    }

    /** @brief True when a continuation line would be kept. */
    [[nodiscard]] bool AcceptsContinuation() const
    {
        EnsureTargetStatus();
        return mAccepts;
    }

    /** @brief Notes a blank line after the record. It joins the record only if another continuation follows. */
    void AddBlank();

    /**
     * @brief Adds a continuation line.
     *
     * Returns false when the line is dropped, because the target cannot take text or because the text would reach
     * maxContinuationBytes. From then on the record drops every further line.
     */
    bool AddContinuation(
        std::string_view line, size_t lineNumber, size_t maxContinuationBytes = std::numeric_limits<size_t>::max()
    );

    /** @brief How many continuation lines were dropped. */
    [[nodiscard]] size_t DroppedContinuationLines() const noexcept
    {
        return mDropped;
    }

    /** @brief The text of the kept continuation lines, each with a newline in front. */
    [[nodiscard]] std::string_view ContinuationText() const noexcept
    {
        return mContinuation;
    }

    /** @brief The kept continuation lines as they stand in the input, with the blank lines between them. */
    [[nodiscard]] std::string_view RawContinuationText() const noexcept
    {
        return mRawText;
    }

    /** @brief The values of the open record. */
    [[nodiscard]] std::vector<CompactLogValue> &Values() noexcept
    {
        return mValues;
    }

    /**
     * @brief Joins the continuation text into the target field. Ok when there was none.
     *
     * ownedBytes receives the joined text. mmapBytes is the mapped file, needed when the field is a slice of it.
     */
    ContinuationSpliceOutcome JoinContinuation(std::string &ownedBytes, std::string_view mmapBytes);

    /** @brief Closes the record and gives its values to the caller, who gets the old vector back to reuse. */
    void Close(std::vector<CompactLogValue> &valuesOut);

private:
    /** Looks at the target field the first time it is needed, so a record without continuation lines never pays for it.
     */
    void EnsureTargetStatus() const;

    bool mKeepRawText;
    bool mOpen = false;
    mutable bool mStatusKnown = false;
    mutable bool mAccepts = false;
    mutable ContinuationSpliceOutcome mTargetStatus = ContinuationSpliceOutcome::MissingTarget;
    KeyId mTarget = INVALID_KEY_ID;
    size_t mFirstLine = 0;
    size_t mLastLine = 0;
    size_t mPendingBlanks = 0;
    size_t mDropped = 0;
    std::vector<CompactLogValue> mValues;
    std::string mContinuation;
    std::string mRawText;
};

} // namespace loglib::internal
