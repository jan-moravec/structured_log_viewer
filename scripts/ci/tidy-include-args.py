"""Prints clang-tidy arguments for checking a header file on its own.

Headers aren't in compile_commands.json, so clang-tidy has no include paths for them. This prints every include path
used in the compile database, made absolute, plus the C++ standard, on one line.

Usage: python3 tidy-include-args.py <build-dir> [<project-include-dir>...]
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
    # Project include paths first, so they win over same-named headers.
    for path in sys.argv[2:]:
        pair = ("-I", os.path.abspath(path))
        seen.add(pair)
        result.append(pair)
    for entry in entries:
        for pair in collect_include_args(entry):
            if pair not in seen:
                seen.add(pair)
                result.append(pair)

    # Joined form (`-isystem/path`), because the action splits the line on spaces.
    args = [f"{flag}{path}" for flag, path in result]
    args.append("-std=c++23")
    print(" ".join(args))


if __name__ == "__main__":
    main()
