/**
 * @brief Builds the text of a long export in chunks on several threads and passes the chunks on in order.
 *
 * Writing a row as text takes far longer than handing it to a file, so exports spread the rows over threads. The rows
 * are cut into chunks of equal size, each thread turns a chunk into text with its own writer, and one thread at a time
 * passes the finished chunks to the sink in the order of the rows. The output is the same as writing the rows one after
 * the other.
 *
 * This is part of the output module. It is used by the row exporters and the session bundle writer.
 */
#pragma once

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_pipeline.h>
#include <oneapi/tbb/task_arena.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace loglib::internal
{

/**
 * @brief Cuts total rows into chunks, builds the text of each chunk and hands the text on in row order.
 *
 * beforeChunk runs on one thread before a chunk is cut, so it is the place to look for a stop request. Throwing from it
 * ends the export after the chunks already under way. makeWorker creates one Worker for each thread that builds text,
 * on that thread, and the worker is reused for all of that thread's chunks. build writes the rows first to first plus
 * count minus one into the text it is given. consume receives each chunk's text in row order, one chunk at a time. An
 * exception from any of them stops the export and is thrown from this function. A single chunk runs on the calling
 * thread without a pipeline.
 */
template <typename Worker, typename MakeWorker, typename BeforeChunk, typename Build, typename Consume>
void WriteInOrderedChunks(
    std::size_t total,
    std::size_t chunkSize,
    MakeWorker &&makeWorker,
    BeforeChunk &&beforeChunk,
    Build &&build,
    Consume &&consume
)
{
    if (total == 0)
    {
        return;
    }
    if (total <= chunkSize)
    {
        beforeChunk();
        Worker worker = makeWorker();
        std::string text;
        build(worker, std::size_t{0}, total, text);
        consume(std::size_t{0}, total, text);
        return;
    }

    struct Chunk
    {
        std::size_t first = 0;
        std::size_t count = 0;
        std::string text;
    };

    std::size_t next = 0;
    oneapi::tbb::enumerable_thread_specific<std::unique_ptr<Worker>> workers;
    const auto liveChunks = static_cast<std::size_t>(oneapi::tbb::this_task_arena::max_concurrency()) * 2;

    oneapi::tbb::parallel_pipeline(
        liveChunks,
        oneapi::tbb::make_filter<void, std::shared_ptr<Chunk>>(
            oneapi::tbb::filter_mode::serial_in_order,
            [&](oneapi::tbb::flow_control &control) -> std::shared_ptr<Chunk> {
                if (next >= total)
                {
                    control.stop();
                    return nullptr;
                }
                beforeChunk();
                auto chunk = std::make_shared<Chunk>();
                chunk->first = next;
                chunk->count = std::min(chunkSize, total - next);
                next += chunk->count;
                return chunk;
            }
        ) &
            oneapi::tbb::make_filter<std::shared_ptr<Chunk>, std::shared_ptr<Chunk>>(
                oneapi::tbb::filter_mode::parallel,
                [&](std::shared_ptr<Chunk> chunk) {
                    std::unique_ptr<Worker> &worker = workers.local();
                    if (!worker)
                    {
                        worker = std::make_unique<Worker>(makeWorker());
                    }
                    build(*worker, chunk->first, chunk->count, chunk->text);
                    return chunk;
                }
            ) &
            oneapi::tbb::make_filter<std::shared_ptr<Chunk>, void>(
                oneapi::tbb::filter_mode::serial_in_order,
                [&](const std::shared_ptr<Chunk> &chunk) { consume(chunk->first, chunk->count, chunk->text); }
            )
    );
}

} // namespace loglib::internal
