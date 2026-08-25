"""Enforce loglib public/internal header policy.

library/src and test/lib may include internals. Supported headers,
app/, test/app/, and test/consumer/ must not. Supported headers must
also stay Qt-free and must not include implementation libraries
(`mio`, `date`, simdjson, glaze, fmt, TBB, PCRE2).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONSUMER_TREES = (ROOT / "app", ROOT / "test" / "app", ROOT / "test" / "consumer")
PUBLIC_HEADER_ROOT = ROOT / "library" / "include" / "loglib"
SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc"}

INTERNAL_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?:loglib/)?(?:\.\./)*internal/[^>"]+[>"]',
    re.M,
)
QT_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<Q(?:t)?[A-Za-z0-9_/]+>", re.M)
VENDOR_INCLUDE_RE = re.compile(
    r"^\s*#\s*include\s*[<\"](?:mio/|date/|simdjson|glaze/|fmt/|oneapi/|pcre2)[^>\"]*[>\"]",
    re.M,
)


def iter_sources(tree: Path) -> list[Path]:
    if not tree.is_dir():
        return []
    return [path for path in tree.rglob("*") if path.suffix in SOURCE_SUFFIXES]


def public_headers() -> list[Path]:
    if not PUBLIC_HEADER_ROOT.is_dir():
        return []
    return [
        path
        for path in PUBLIC_HEADER_ROOT.rglob("*.hpp")
        if "internal" not in path.relative_to(PUBLIC_HEADER_ROOT).parts
    ]


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT)).replace("\\", "/")


def main() -> int:
    violations: list[str] = []

    for tree in CONSUMER_TREES:
        for path in iter_sources(tree):
            text = path.read_text(encoding="utf-8")
            if INTERNAL_INCLUDE_RE.search(text):
                violations.append(f"{rel(path)}: includes loglib/internal")

    for path in public_headers():
        text = path.read_text(encoding="utf-8")
        if INTERNAL_INCLUDE_RE.search(text):
            violations.append(f"{rel(path)}: supported header includes loglib/internal")
        if QT_INCLUDE_RE.search(text):
            violations.append(f"{rel(path)}: supported header includes a Qt header")
        if VENDOR_INCLUDE_RE.search(text):
            violations.append(f"{rel(path)}: supported header includes an implementation library")

    if violations:
        print("loglib public/internal header policy violations:", file=sys.stderr)
        for item in sorted(violations):
            print(f"  {item}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
