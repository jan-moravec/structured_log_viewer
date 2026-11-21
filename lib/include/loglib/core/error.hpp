/**
 * @brief The exception types the library throws.
 *
 * Opening, reading and writing files, pipes and sockets, decompressing, and reading saved files such as configurations
 * and bundles throw a subclass of Error. A failed file, pipe or socket operation throws IoError, which carries the
 * system error code when there is one. Parsing user data never throws, and validating user input returns a result
 * instead. OperationCancelled and BrokenPipe are control exceptions and deliberately do not derive from Error, so a
 * handler for Error does not swallow a cancel or a closed pipe. The full rules are in CONTRIBUTING.md, section Error
 * handling.
 *
 * This is part of the core module.
 */
#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace loglib
{

/** Base of every failure the library reports by exception for a resource or a saved file. */
class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * A file, pipe or socket operation failed. The message names the path or endpoint and the system error. When the
 * failure has a system error code, Code returns it.
 */
class IoError : public Error
{
public:
    using Error::Error;

    /** Builds the message as the message, a colon and the text of the system error code. */
    IoError(const std::string &message, std::error_code code)
        : Error(code ? message + ": " + code.message() : message), mCode(code)
    {
    }

    /** The system error code of the failure, or an empty code when the failure has none. */
    [[nodiscard]] std::error_code Code() const noexcept
    {
        return mCode;
    }

private:
    std::error_code mCode;
};

/** A saved file or bundle cannot be read: it is malformed, incomplete or has an unsupported version. */
class FormatError : public Error
{
public:
    using Error::Error;
};

/** A limit was reached: a configured one, such as the decompression size cap, or a fixed capacity, such as the number
 * of distinct keys. */
class LimitError : public Error
{
public:
    using Error::Error;
};

/**
 * Thrown by a long operation that stopped because its StopToken fired.
 *
 * Deliberately not an Error or a std::runtime_error, so error handlers do not swallow a cancellation. Decompression,
 * exporting and bundle writing all throw it.
 */
class OperationCancelled : public std::exception
{
public:
    explicit OperationCancelled(std::string message = "Operation cancelled")
        : mMessage(std::move(message))
    {
    }

    [[nodiscard]] const char *what() const noexcept override
    {
        return mMessage.c_str();
    }

private:
    std::string mMessage;
};

/**
 * Thrown when a write reaches a pipe or socket that the reader has closed.
 *
 * It does not derive from Error, so a general I/O handler does not report it. Callers that treat a closed reader as
 * success, as in slp query piped into head, catch it and exit quietly.
 */
class BrokenPipe : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Broken pipe";
    }
};

} // namespace loglib
