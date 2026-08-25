"""Fail if app/ or test/app/ include loglib/internal headers.

library/ and test/lib/ may include internals.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREES = (ROOT / "app", ROOT / "test" / "app")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]loglib/internal/[^>"]+[>"]', re.M)


def main() -> int:
    violations: list[str] = []
    for tree in TREES:
        if not tree.is_dir():
            continue
        for path in tree.rglob("*"):
            if path.suffix not in {".hpp", ".h", ".cpp", ".cc"}:
                continue
            text = path.read_text(encoding="utf-8")
            if INCLUDE_RE.search(text):
                violations.append(str(path.relative_to(ROOT)).replace("\\", "/"))
    if violations:
        print("app/ and test/app/ must not include loglib/internal/ headers:", file=sys.stderr)
        for rel in sorted(violations):
            print(f"  {rel}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
