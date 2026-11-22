#include "loglib/parse/internal/record_assembler.hpp"

#include <cstdint>
#include <span>
#include <utility>

namespace loglib::internal
{

ContinuationSpliceOutcome ExtendContinuationTarget(
    std::vector<CompactLogValue> &values,
    std::string &ownedArena,
    KeyId targetKey,
    std::string_view continuationBytes,
    std::string_view mmapBytes
)
{
    if (targetKey == INVALID_KEY_ID)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    if (continuationBytes.empty())
    {
        return ContinuationSpliceOutcome::Ok;
    }

    for (CompactLogValue &v : values)
    {
        if (v.Key() != targetKey)
        {
            continue;
        }
        switch (v.tag)
        {
        case CompactTag::OwnedString:
        {
            const uint64_t offset = v.payload;
            const uint32_t length = v.aux;
            if (offset + length == ownedArena.size())
            {
                ownedArena.append(continuationBytes.data(), continuationBytes.size());
                v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
            }
            else
            {
                // Reserve before self-appending: reallocation would invalidate the source pointer, and overlap safety
                // is not guaranteed.
                ownedArena.reserve(ownedArena.size() + length + continuationBytes.size());
                const uint64_t newOffset = ownedArena.size();
                ownedArena.append(ownedArena.data() + offset, length);
                ownedArena.append(continuationBytes.data(), continuationBytes.size());
                v.payload = newOffset;
                v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
            }
            return ContinuationSpliceOutcome::Ok;
        }
        case CompactTag::Monostate:
        {
            // A bare key= has no preceding content to separate.
            std::string_view bytes = continuationBytes;
            if (!bytes.empty() && bytes.front() == '\n')
            {
                bytes.remove_prefix(1);
            }
            v.tag = CompactTag::OwnedString;
            v.payload = ownedArena.size();
            v.aux = static_cast<uint32_t>(bytes.size());
            ownedArena.append(bytes.data(), bytes.size());
            return ContinuationSpliceOutcome::Ok;
        }
        case CompactTag::MmapSlice:
        {
            // Materialise the mmap slice before extending it.
            const uint64_t offset = v.payload;
            const uint32_t length = v.aux;
            if (mmapBytes.empty() || offset + length > mmapBytes.size())
            {
                return ContinuationSpliceOutcome::NonStringTarget;
            }
            const uint64_t newOffset = ownedArena.size();
            ownedArena.append(mmapBytes.data() + offset, length);
            ownedArena.append(continuationBytes.data(), continuationBytes.size());
            v.tag = CompactTag::OwnedString;
            v.payload = newOffset;
            v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
            return ContinuationSpliceOutcome::Ok;
        }
        case CompactTag::DictRef:
        case CompactTag::Int64:
        case CompactTag::Uint64:
        case CompactTag::Double:
        case CompactTag::Bool:
        case CompactTag::Timestamp:
            return ContinuationSpliceOutcome::NonStringTarget;
        }
        return ContinuationSpliceOutcome::NonStringTarget;
    }
    return ContinuationSpliceOutcome::MissingTarget;
}

ContinuationSpliceOutcome CheckContinuationTarget(std::span<const CompactLogValue> values, KeyId targetKey)
{
    if (targetKey == INVALID_KEY_ID)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    for (const CompactLogValue &value : values)
    {
        if (value.Key() != targetKey)
        {
            continue;
        }
        switch (value.tag)
        {
        case CompactTag::OwnedString:
        case CompactTag::Monostate:
        case CompactTag::MmapSlice:
            return ContinuationSpliceOutcome::Ok;
        case CompactTag::DictRef:
        case CompactTag::Int64:
        case CompactTag::Uint64:
        case CompactTag::Double:
        case CompactTag::Bool:
        case CompactTag::Timestamp:
            return ContinuationSpliceOutcome::NonStringTarget;
        }
        return ContinuationSpliceOutcome::NonStringTarget;
    }
    return ContinuationSpliceOutcome::MissingTarget;
}

RecordAssembler::RecordAssembler(bool keepRawText) noexcept
    : mKeepRawText(keepRawText)
{
}

void RecordAssembler::Open(std::vector<CompactLogValue> &values, size_t firstLine, KeyId target)
{
    mValues.clear();
    mValues.swap(values);
    mOpen = true;
    mFirstLine = firstLine;
    mLastLine = firstLine;
    mTarget = target;
    mStatusKnown = false;
    mPendingBlanks = 0;
    mDropped = 0;
    mContinuation.clear();
    mRawText.clear();
}

void RecordAssembler::EnsureTargetStatus() const
{
    if (mStatusKnown)
    {
        return;
    }
    mTargetStatus = CheckContinuationTarget(mValues, mTarget);
    mAccepts = mTargetStatus == ContinuationSpliceOutcome::Ok;
    mStatusKnown = true;
}

void RecordAssembler::AddBlank()
{
    if (mOpen && AcceptsContinuation())
    {
        ++mPendingBlanks;
    }
}

bool RecordAssembler::AddContinuation(std::string_view line, size_t lineNumber, size_t maxContinuationBytes)
{
    EnsureTargetStatus();
    if (mAccepts && mContinuation.size() + line.size() >= maxContinuationBytes)
    {
        mAccepts = false;
    }
    if (!mAccepts)
    {
        // Count the line without keeping its bytes.
        ++mDropped;
        mPendingBlanks = 0;
        return false;
    }
    if (mKeepRawText)
    {
        // Blank lines stay in the raw text, but not in the target field.
        mRawText.append(mPendingBlanks, '\n');
        mRawText.push_back('\n');
        mRawText.append(line.data(), line.size());
    }
    mPendingBlanks = 0;
    mContinuation.push_back('\n');
    mContinuation.append(line.data(), line.size());
    mLastLine = lineNumber;
    return true;
}

ContinuationSpliceOutcome RecordAssembler::JoinContinuation(std::string &ownedBytes, std::string_view mmapBytes)
{
    if (mContinuation.empty())
    {
        return ContinuationSpliceOutcome::Ok;
    }
    return ExtendContinuationTarget(mValues, ownedBytes, mTarget, mContinuation, mmapBytes);
}

void RecordAssembler::Close(std::vector<CompactLogValue> &valuesOut)
{
    valuesOut.clear();
    valuesOut.swap(mValues);
    mOpen = false;
}

} // namespace loglib::internal
