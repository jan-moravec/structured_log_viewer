#include "loglib/log_file.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace loglib
{

LogFileReference::LogFileReference(LogFile &logFile, size_t lineNumber) : mLogFile(logFile), mLineNumber(lineNumber)
{
}

const std::filesystem::path &LogFileReference::GetPath() const
{
    return mLogFile.GetPath();
}

size_t LogFileReference::GetLineNumber() const
{
    return mLineNumber;
}

std::string LogFileReference::GetLine() const
{
    return mLogFile.GetLine(mLineNumber);
}

LogFile::LogFile(std::filesystem::path filePath) : mPath(std::move(filePath)), mFile(mPath)
{
    if (!mFile.is_open())
    {
        throw std::runtime_error("Failed to open file: " + mPath.string());
    }

    mLineOffsets.push_back(0); // First line starts at byte 0
}

const std::filesystem::path &LogFile::GetPath() const
{
    return mPath;
}

std::string LogFile::GetLine(size_t lineNumber)
{
    if (lineNumber + 1 >= mLineOffsets.size())
    {
        throw std::out_of_range("Line number out of range: " + std::to_string(lineNumber));
    }

    const std::streampos startOffset = mLineOffsets[lineNumber];
    const std::streampos stopOffset = mLineOffsets[lineNumber + 1];
    std::string buffer(stopOffset - startOffset - 1, '\0');
    mFile.seekg(startOffset);
    mFile.read(&buffer[0], buffer.size()); // Read directly into the string
    if (!buffer.empty() && buffer.back() == '\n')
    {
        buffer.pop_back(); // Remove newline character
        if (!buffer.empty() && buffer.back() == '\r')
        {
            buffer.pop_back(); // Remove carriage return character
        }
    }
    return buffer;
}

LogFileReference LogFile::CreateReference(std::streampos position)
{
    LogFileReference reference(*this, mLineOffsets.size());
    if (position == std::streampos(-1))
    {
        // Invalid position can mean the end of the file, so convert it to the end stream position
        mFile.seekg(0, std::ios::end);
        position = std::streampos(static_cast<std::streamoff>(mFile.tellg()) + 1);
    }
    if (position - mLineOffsets.back() <= 0)
    {
        throw std::runtime_error("Invalid position to create reference: " + std::to_string(position));
    }
    mLineOffsets.push_back(std::move(position));
    return reference;
}

} // namespace loglib
