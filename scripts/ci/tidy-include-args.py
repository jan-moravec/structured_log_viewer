"""Prints clang-tidy extra arguments for checking headers on their own.

Headers are not entries in compile_commands.json, so clang-tidy run on a
changed header has no include paths. This collects every include directory
used by any translation unit in the database and prints them, together with
the language standard, as one space-separated line.

clang-tidy resolves relative `--extra-arg` paths against the build
directory of the proxy translation unit, so every path is made absolute.

Usage: python3 tidy-include-args.py <build-dir> [<source-include-dir>...]
"""

import json
import os
import shlex
import sys

INCLUDE_FLAGS = ("-I", "-isystem", "-iquote")


def collect_include_args(entry):
    """Yields (flag, absolute path) pairs from one compile command entry."""
    args = entry.get("arguments") or shlex.split(entry["command"])
    directory = entry["directory"]
    i = 0
    while i < len(args):
        arg = args[i]
        for flag in INCLUDE_FLAGS:
            if arg == flag and i + 1 < len(args):
                i += 1
                yield flag, os.path.normpath(os.path.join(directory, args[i]))
                break
            if arg.startswith(flag) and len(arg) > len(flag):
                path = arg[len(flag) :]
                yield flag, os.path.normpath(os.path.join(directory, path))
                break
        i += 1


def main():
    build_dir = sys.argv[1]
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        entries = json.load(f)

    result = []
    seen = set()
    # Project include roots first so they win over same-named headers.
    for path in sys.argv[2:]:
        pair = ("-I", os.path.abspath(path))
        seen.add(pair)
        result.append(pair)
    for entry in entries:
        for pair in collect_include_args(entry):
            if pair not in seen:
                seen.add(pair)
                result.append(pair)

    # Joined form (`-isystem/path`) so each item survives the action's
    # split on spaces as a single argument.
    args = [f"{flag}{path}" for flag, path in result]
    args.append("-std=c++23")
    print(" ".join(args))


if __name__ == "__main__":
    main()
