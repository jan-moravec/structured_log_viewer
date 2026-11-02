"""Check loglib header comments against the rules in AGENTS.md.

Every header under lib/include must start with a /** */ file comment that
has an @brief, placed above #pragma once, and comments must not contain
backticks.

Invoked from pre-commit.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER_ROOT = ROOT / "lib" / "include"
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def check(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    problems = []
    head = text.lstrip()
    pragma = head.find("#pragma once")
    if not head.startswith("/**") or "@brief" not in head[: max(pragma, 0)]:
        problems.append("1: header does not start with a /** */ file comment with @brief above #pragma once")
    for match in COMMENT_RE.finditer(text):
        if "`" in match.group():
            line = text.count("\n", 0, match.start() + match.group().index("`")) + 1
            problems.append(f"{line}: backtick in comment")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args()
    files = args.files or sorted(HEADER_ROOT.rglob("*.hpp"))
    total = 0
    for path in files:
        problems = check(path.resolve())
        total += len(problems)
        shown = path.resolve().relative_to(ROOT).as_posix()
        for problem in problems[:3]:
            print(f"{shown}:{problem}")
        if len(problems) > 3:
            print(f"{shown}: ... and {len(problems) - 3} more")
    print(f"check-loglib-docs: {total} problem(s) in {len(files)} header(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
