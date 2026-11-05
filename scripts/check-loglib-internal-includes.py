"""Enforce loglib public/internal header policy and the module map.

lib/src and test/lib may include internals. Supported headers,
app/, test/app/, cli/, and test/cli/ must not. cli/ and test/cli/
must also stay Qt-free. Supported headers must stay Qt-free and
must not include implementation libraries (`mio`, `date`, simdjson,
glaze, fmt, TBB, PCRE2).

Each loglib header and source file belongs to the module named by its
directory under lib/include/loglib and lib/src. A file may include headers
only from its own module and the modules listed for it in MODULE_DEPENDENCIES.

The library never reads the process-wide time zone. A call to ProcessDefaultTimeZone() in lib/src or lib/include is
allowed only in the time zone context itself, which defines it. The application layer owns that setting and passes the
zone to the library as a value.

Invoked from pre-commit.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONSUMER_TREES = (ROOT / "app", ROOT / "test" / "app", ROOT / "cli", ROOT / "test" / "cli")
QT_FREE_TREES = (ROOT / "cli", ROOT / "test" / "cli")
LIB_ROOT = ROOT / "lib"
PUBLIC_HEADER_ROOT = LIB_ROOT / "include" / "loglib"
SOURCE_ROOT = LIB_ROOT / "src"

# Module -> modules it may include. Keep in step with lib/README.md.
MODULE_DEPENDENCIES: dict[str, frozenset[str]] = {
    "core": frozenset(),
    "schema": frozenset({"core"}),
    "time": frozenset({"core"}),
    "ingest": frozenset({"core"}),
    "parse": frozenset({"core", "schema", "time"}),
    "table": frozenset({"core", "schema", "time", "parse"}),
    "query": frozenset({"core", "schema", "time", "table"}),
    "output": frozenset({"core", "schema", "time", "parse", "table", "query", "ingest"}),
    "support": frozenset({"core"}),
}
SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc"}

# Files that may mention a call to ProcessDefaultTimeZone(): its declaration and its definition.
PROCESS_ZONE_ALLOWED = frozenset(
    {
        "lib/include/loglib/time/time_zone_context.hpp",
        "lib/src/time/time_zone_context.cpp",
    }
)
# A word boundary keeps SetProcessDefaultTimeZone out of the match.
PROCESS_ZONE_CALL_RE = re.compile(r"\bProcessDefaultTimeZone\s*\(")

INTERNAL_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]loglib/[a-z]+/(?:[a-z_]+/)*internal/[^>"]+[>"]',
    re.M,
)
LOGLIB_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]loglib/([a-z]+)/[^>"]+[>"]', re.M)
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


def module_of(path: Path) -> str | None:
    """Returns the module a file under lib/include/loglib or lib/src belongs to."""
    for root in (PUBLIC_HEADER_ROOT, SOURCE_ROOT):
        if root in path.parents:
            module = path.relative_to(root).parts[0]
            return module if module in MODULE_DEPENDENCIES else None
    return None


def module_violations() -> list[str]:
    violations: list[str] = []
    for root in (PUBLIC_HEADER_ROOT, SOURCE_ROOT):
        for path in iter_sources(root):
            module = module_of(path)
            if module is None:
                continue
            text = path.read_text(encoding="utf-8")
            for match in LOGLIB_INCLUDE_RE.finditer(text):
                target = match.group(1)
                if target == module:
                    continue
                if target not in MODULE_DEPENDENCIES:
                    violations.append(f"{rel(path)}: includes unknown module {target}")
                elif target not in MODULE_DEPENDENCIES[module]:
                    violations.append(f"{rel(path)}: module {module} must not include module {target}")
    return violations


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT)).replace("\\", "/")


def process_zone_violations() -> list[str]:
    violations: list[str] = []
    for root in (PUBLIC_HEADER_ROOT, SOURCE_ROOT):
        for path in iter_sources(root):
            if rel(path) in PROCESS_ZONE_ALLOWED:
                continue
            text = path.read_text(encoding="utf-8")
            if PROCESS_ZONE_CALL_RE.search(text):
                violations.append(f"{rel(path)}: calls ProcessDefaultTimeZone(); take a TimeZoneContext as a parameter")
    return violations


def main() -> int:
    violations: list[str] = []

    for tree in CONSUMER_TREES:
        for path in iter_sources(tree):
            text = path.read_text(encoding="utf-8")
            if INTERNAL_INCLUDE_RE.search(text):
                violations.append(f"{rel(path)}: includes loglib/internal")

    for tree in QT_FREE_TREES:
        for path in iter_sources(tree):
            text = path.read_text(encoding="utf-8")
            if QT_INCLUDE_RE.search(text):
                violations.append(f"{rel(path)}: includes a Qt header")

    for path in public_headers():
        text = path.read_text(encoding="utf-8")
        if INTERNAL_INCLUDE_RE.search(text):
            violations.append(f"{rel(path)}: supported header includes loglib/internal")
        if QT_INCLUDE_RE.search(text):
            violations.append(f"{rel(path)}: supported header includes a Qt header")
        if VENDOR_INCLUDE_RE.search(text):
            violations.append(f"{rel(path)}: supported header includes an implementation library")

    violations.extend(module_violations())
    violations.extend(process_zone_violations())

    if violations:
        print("loglib public/internal header policy violations:", file=sys.stderr)
        for item in sorted(violations):
            print(f"  {item}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
