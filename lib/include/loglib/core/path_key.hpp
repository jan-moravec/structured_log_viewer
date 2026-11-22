/**
 * @brief One normalized form of a file path, for telling whether two paths name the same file.
 *
 * Saved sessions, anchors, rotation series and the application all need to ask whether two spellings of a path are the
 * same file. MakePathKey answers it in one place, so every part of the library and every program built on it agrees.
 * The key is a comparison value and is not meant to be shown to a user.
 *
 * This is part of the core module.
 */
#pragma once

#include <filesystem>
#include <string>

namespace loglib
{

/**
 * @brief Builds a key that is the same for different spellings of one path.
 *
 * The key is an absolute path in UTF-8 with forward slashes, normalized by text and without touching the file system,
 * so it works for files that do not exist any more and does not follow links. Letters are lower-cased on Windows only,
 * because Windows file systems ignore case by default. On Linux and macOS two names that differ in case are two files,
 * so the key keeps the case. Only ASCII letters are folded. A path that cannot be made absolute keeps its own form. An
 * empty path gives an empty key.
 */
[[nodiscard]] std::string MakePathKey(const std::filesystem::path &path);

} // namespace loglib
