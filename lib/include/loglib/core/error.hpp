/**
 * @brief The exception types the library throws for failed resources and unreadable saved files.
 *
 * Opening, reading and writing files and sockets, decompressing, and reading saved files such as configurations and
 * bundles throw a subclass of Error. Parsing user data never throws, and validating user input returns a result
 * instead. OperationCancelled and BrokenPipe are control exceptions and deliberately do not derive from Error, so a
 * handler for Error does not swallow a cancel or a closed pipe. The full rules are in CONTRIBUTING.md, section Error
 * handling.
 *
 * This is part of the core module.
 */
#pragma once

#include <stdexcept>

namespace loglib
{

/** Base of every failure the library reports by exception for a resource or a saved file. */
class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/** A file or socket operation failed. The message names the path or endpoint and the system error. */
class IoError : public Error
{
public:
    using Error::Error;
};

/** A saved file or bundle cannot be read: it is malformed, incomplete or has an unsupported version. */
class FormatError : public Error
{
public:
    using Error::Error;
};

/** A configured limit was reached, such as the decompression size cap. */
class LimitError : public Error
{
public:
    using Error::Error;
};

} // namespace loglib
