#include <log_generator/sinks.hpp>

#include <test_common/network_log_client.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace log_generator
{

namespace
{

// Writes to a file and rotates it in flight. A rotation that is due runs
// before the next line, so the last file of a run is never left empty.
class FileSink final : public Sink
{
public:
    FileSink(std::filesystem::path path, bool append, std::string header, RotationOptions rotation, std::ostream &diag)
        : mPath(std::move(path)), mHeader(std::move(header)), mRotation(rotation), mDiag(&diag)
    {
        mOut.open(mPath, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        if (!mOut.is_open())
        {
            throw std::runtime_error("cannot open output file " + mPath.string());
        }
        if (append)
        {
            // Count what is already on disk toward `--roll-size`. The line
            // count is not recovered, so `--roll-lines` counts new lines only.
            std::error_code ec;
            const auto existing = std::filesystem::file_size(mPath, ec);
            mBytesInFile = ec ? 0 : static_cast<std::uint64_t>(existing);
        }
        else
        {
            WriteHeader();
        }
    }

    bool WriteLine(std::string_view line) override
    {
        if (RotationDue())
        {
            Rotate();
        }
        mOut << line << '\n';
        if (!mOut.good())
        {
            throw std::runtime_error("write to " + mPath.string() + " failed");
        }
        const std::uint64_t bytes = line.size() + 1U;
        mBytesInFile += bytes;
        ++mLinesInFile;
        mTotalBytes += bytes;
        ++mTotalLines;
        return true;
    }

    void Flush() override
    {
        mOut.flush();
    }

    void Close() override
    {
        mOut.close();
    }

    std::size_t Rotations() const noexcept override
    {
        return mRotations;
    }

private:
    void WriteHeader()
    {
        if (!mHeader.empty())
        {
            mOut << mHeader << '\n';
            mBytesInFile += mHeader.size() + 1U;
        }
    }

    bool RotationDue() const noexcept
    {
        return (mRotation.maxBytes != 0 && mBytesInFile >= mRotation.maxBytes) ||
               (mRotation.maxLines != 0 && mLinesInFile >= mRotation.maxLines);
    }

    void Rotate()
    {
        mOut.close();
        RotateFile(mPath, mRotation.strategy, mRotation.keep);
        mOut.open(mPath, std::ios::binary | std::ios::trunc);
        if (!mOut.is_open())
        {
            throw std::runtime_error("cannot reopen " + mPath.string() + " after rotation");
        }
        mBytesInFile = 0;
        mLinesInFile = 0;
        WriteHeader();
        ++mRotations;
        *mDiag << "log_generator: rotated #" << mRotations << " (" << ToString(mRotation.strategy)
               << "), continuing into " << mPath.string() << " (total so far: " << mTotalLines << " lines, "
               << mTotalBytes << " bytes)\n";
    }

    std::filesystem::path mPath;
    std::string mHeader;
    RotationOptions mRotation;
    std::ostream *mDiag;
    std::ofstream mOut;
    std::uint64_t mBytesInFile = 0;
    std::uint64_t mLinesInFile = 0;
    std::uint64_t mTotalBytes = 0;
    std::uint64_t mTotalLines = 0;
    std::size_t mRotations = 0;
};

class StdoutSink final : public Sink
{
public:
    explicit StdoutSink(const std::string &header)
    {
#ifdef _WIN32
        // Text-mode stdout would turn every '\n' into "\r\n".
        (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
        if (!header.empty())
        {
            // Readers detect the format from the leading bytes.
            std::cout << header << '\n';
            std::cout.flush();
        }
    }

    bool WriteLine(std::string_view line) override
    {
        std::cout << line << '\n';
        // A reader that exits early closes the pipe; that ends the run cleanly.
        return std::cout.good();
    }

    void Flush() override
    {
        std::cout.flush();
    }

    void Close() override
    {
        std::cout.flush();
    }
};

template <typename Client> class NetworkSink final : public Sink
{
public:
    NetworkSink(std::unique_ptr<Client> client, const std::string &header)
        : mClient(std::move(client))
    {
        if (!header.empty())
        {
            mClient->Send(header);
        }
    }

    bool WriteLine(std::string_view line) override
    {
        mClient->Send(line);
        return true;
    }

    void Close() override
    {
        mClient->Close();
    }

private:
    std::unique_ptr<Client> mClient;
};

} // namespace

std::unique_ptr<Sink> OpenSink(const GeneratorOptions &options, const std::string &header, std::ostream &diag)
{
    const Target &target = options.target;
    switch (target.kind)
    {
    case TargetKind::File:
        return std::make_unique<FileSink>(target.path, options.append, header, options.rotation, diag);
    case TargetKind::Stdout:
        return std::make_unique<StdoutSink>(header);
    case TargetKind::Udp:
        return std::make_unique<NetworkSink<test_common::UdpLogClient>>(
            std::make_unique<test_common::UdpLogClient>(target.host, target.port), header
        );
    case TargetKind::Tcp:
    case TargetKind::TcpTls:
    {
        std::optional<test_common::TcpLogClient::TlsOptions> tls;
        if (target.kind == TargetKind::TcpTls)
        {
            tls = options.tls;
        }
        return std::make_unique<NetworkSink<test_common::TcpLogClient>>(
            std::make_unique<test_common::TcpLogClient>(target.host, target.port, std::move(tls)), header
        );
    }
    }
    throw std::logic_error("unhandled output kind");
}

} // namespace log_generator
