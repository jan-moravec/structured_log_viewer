/**
 * @brief Interface for sources of raw bytes that arrive over time.
 *
 * A BytesProducer hands out bytes as they appear, for example from a tailed file, a network socket or standard input.
 * StreamLineSource reads from one and splits the bytes into lines. The concrete producers live in the ingest module.
 * The header also defines the status values a producer reports and the default longest line a producer buffers.
 *
 * This is part of the core module.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace loglib
{

/** @brief Default for ParserOptions::maxLineBytes and the producers' line limits. */
inline constexpr std::size_t DEFAULT_MAX_LINE_BYTES = std::size_t{64} * 1024 * 1024;

/** @brief Coarse state a producer reports through SetStatusCallback, for a status display. */
enum class SourceStatus
{
    /** @brief The producer can produce bytes. Finite producers stay in this state. */
    Running,

    /**
     * @brief The producer cannot produce bytes for now, for example because a watched file was deleted and not yet
     * recreated. It keeps retrying and reports Running again when bytes flow.
     */
    Waiting,
};

/**
 * @brief Supplies bytes to the streaming pipeline through StreamLineSource.
 *
 * Implementations are TailingBytesProducer, TcpServerProducer, UdpServerProducer and StdinBytesProducer. The parser
 * drives and the producer supplies bytes; a producer knows nothing about LogParseSink or ParserOptions. Static files do
 * not use this interface. They go through FileLineSource.
 */
class BytesProducer
{
public:
    BytesProducer() = default;
    virtual ~BytesProducer() = default;

    BytesProducer(const BytesProducer &) = delete;
    BytesProducer &operator=(const BytesProducer &) = delete;

    BytesProducer(BytesProducer &&) = delete;
    BytesProducer &operator=(BytesProducer &&) = delete;

    /**
     * @brief Copies available bytes into buffer and returns how many were copied.
     *
     * Zero means no bytes right now, or the end of the stream; IsClosed tells them apart. After zero on an open
     * producer, call WaitForBytes and read again. An empty buffer returns zero.
     */
    virtual size_t Read(std::span<char> buffer) = 0;

    /**
     * @brief Blocks until a byte is available, the timeout passes, or Stop is called.
     *
     * It may return early, so always read again to find out whether bytes arrived.
     */
    virtual void WaitForBytes(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Wakes any blocked Read or WaitForBytes and makes later calls report the end of the stream.
     *
     * Safe from any thread and safe to call twice. It releases the producer's I/O so the parser can reach its next
     * batch boundary and see its own stop token; it does not replace that token.
     */
    virtual void Stop() noexcept = 0;

    /** @brief True once the stream has ended, either because it ran out or because Stop was called. */
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;

    /** @brief Short name of the source for display, such as the base name of a file. */
    [[nodiscard]] virtual std::string DisplayName() const = 0;

    /**
     * @brief Installs a callback that the producer's worker thread calls when it detects a rotation of the source.
     *
     * Producers that cannot rotate ignore it. An empty callback removes the previous one.
     */
    virtual void SetRotationCallback(const std::function<void()> &callback);

    /**
     * @brief Installs a callback that is called when the status changes between Running and Waiting.
     *
     * Producers with a fixed status ignore it. An empty callback removes the previous one.
     */
    virtual void SetStatusCallback(const std::function<void(SourceStatus)> &callback);
};

} // namespace loglib
