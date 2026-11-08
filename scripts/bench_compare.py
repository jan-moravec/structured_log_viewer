"""Compare benchmark results of two builds.

Usage:
    python -I scripts/bench_compare.py BEFORE AFTER [--threshold 3]
    python -I scripts/bench_compare.py --before B1 B2 --after A1 A2 [--threshold 3]

Each argument is a result file. With several files on a side, each figure is the median of that side's runs, which
smooths run-to-run variation on the machine. The second form also prints the spread of each side.

Two kinds of file are read:

- Google Benchmark JSON (a ".json" file, written by loglib_benchmarks with --benchmark_out): one figure per benchmark,
  the median aggregate of the real time, lower is better. Where a benchmark reports bytes or items per second, the
  median of that rate is compared as well, higher is better. The suffixes Google Benchmark adds to a name (iterations,
  repeats, min time and the time kind) are dropped so the same benchmark matches across builds.
- A CTest log (any other file, UTF-8 or UTF-16): the GUI benchmarks print one QDEBUG line per figure. The name is the
  test function plus the text before the first ": <number>", and the figure is the first number followed by "ms" (a
  time) or "MB/s" (a rate). QDEBUG lines with no such number are skipped.

The script prints the change of every figure in percent, sorted from worst to best. Positive means better: less time,
or more throughput. Figures worse than the threshold are marked with "!!" and the exit code is 1, so a run can gate a
PR. It also lists benchmarks that failed or that appear on only one side.
"""

from __future__ import annotations

import argparse
import codecs
import json
import re
import statistics
import sys
from pathlib import Path

PREFIX = re.compile(r"^\s*\d+: ?")
NUMBER = r"([0-9][0-9.]*(?:e[+-]?[0-9]+)?)"
GUI_LINE = re.compile(r"QDEBUG\s*:\s*(\S.*)$")
GUI_NAME_END = re.compile(r":\s+(?=[0-9])")
GUI_FIGURE = re.compile(NUMBER + r"\s*(ms|MB/s)(?![A-Za-z/])")
FAILED = re.compile(r"Test\s+#\d+: (.+?) \.+\*+\s*Failed")
# Suffixes that Google Benchmark appends to a benchmark name.
NAME_SUFFIX = re.compile(
    r"(/(iterations|repeats|min_time|min_warmup_time|threads):[^/]*|/real_time|/process_time|/manual_time)+$"
)
TO_MILLISECONDS = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}

Figures = dict[str, tuple[float, str]]


def read_text(path: Path) -> str:
    raw = path.read_bytes()
    # Windows PowerShell 5 writes redirected output as UTF-16 with a byte order mark; PowerShell 7 writes UTF-8.
    encoding = "utf-16" if raw.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)) else "utf-8-sig"
    return raw.decode(encoding, errors="replace")


def json_figures(text: str) -> Figures:
    """Returns the median real time and rates of each benchmark in a Google Benchmark JSON file."""
    medians: Figures = {}
    iterations: dict[str, list[dict]] = {}
    for entry in json.loads(text).get("benchmarks", []):
        if entry.get("error_occurred"):
            continue
        name = NAME_SUFFIX.sub("", entry.get("run_name") or entry["name"])
        if entry.get("run_type") == "aggregate":
            if entry.get("aggregate_name") == "median":
                add_entry(medians, name, entry)
        else:
            iterations.setdefault(name, []).append(entry)
    # A run without repetitions has no aggregates; its single iteration entry is the figure.
    for name, entries in iterations.items():
        if f"{name}" not in medians and len(entries) == 1:
            add_entry(medians, name, entries[0])
    return medians


def add_entry(figures: Figures, name: str, entry: dict) -> None:
    scale = TO_MILLISECONDS.get(entry.get("time_unit", "ns"), 1e-6)
    figures[name] = (entry["real_time"] * scale, "time")
    if "bytes_per_second" in entry:
        figures[f"{name} [bytes/s]"] = (entry["bytes_per_second"], "rate")
    if "items_per_second" in entry:
        figures[f"{name} [items/s]"] = (entry["items_per_second"], "rate")


def gui_figures(text: str) -> Figures:
    """Returns the time or rate of each QDEBUG line of a GUI benchmark in a CTest log."""
    figures: Figures = {}
    for raw in text.splitlines():
        line = PREFIX.sub("", raw, count=1)
        if not (m := GUI_LINE.search(line)):
            continue
        parts = GUI_NAME_END.split(m.group(1).strip(), maxsplit=1)
        if len(parts) != 2:
            continue
        name, rest = parts[0].strip(), parts[1]
        if found := GUI_FIGURE.match(rest):
            key = name
            n = 2
            while key in figures:
                key = f"{name} #{n}"
                n += 1
            figures[key] = (float(found.group(1)), "time" if found.group(2) == "ms" else "rate")
    return figures


def load(path: Path) -> tuple[Figures, list[str]]:
    text = read_text(path)
    if path.suffix.lower() == ".json":
        return json_figures(text), []
    return gui_figures(text), FAILED.findall(text)


def median_figures(paths: list[Path]) -> tuple[Figures, list[str], dict[str, list[float]]]:
    """Returns the median figure of each benchmark over several files, the failures, and every file's value."""
    runs: dict[str, list[float]] = {}
    kinds: dict[str, str] = {}
    failed: list[str] = []
    for path in paths:
        figures, fails = load(path)
        failed.extend(fails)
        for name, (value, kind) in figures.items():
            runs.setdefault(name, []).append(value)
            kinds[name] = kind
    medians = {name: (statistics.median(values), kinds[name]) for name, values in runs.items()}
    return medians, failed, runs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path, nargs="?")
    parser.add_argument("after", type=Path, nargs="?")
    parser.add_argument("--before", dest="before_runs", type=Path, nargs="+", help="several files, medians are used")
    parser.add_argument("--after", dest="after_runs", type=Path, nargs="+", help="several files, medians are used")
    parser.add_argument("--threshold", type=float, default=3.0, help="percent worse that fails the gate")
    args = parser.parse_args()

    before_paths = args.before_runs or ([args.before] if args.before else [])
    after_paths = args.after_runs or ([args.after] if args.after else [])
    if not before_paths or not after_paths:
        parser.error("give BEFORE AFTER, or --before and --after")
    before, failed_before, before_runs = median_figures(before_paths)
    after, failed_after, after_runs = median_figures(after_paths)
    several = len(before_paths) > 1 or len(after_paths) > 1

    rows = []
    for name in before.keys() & after.keys():
        (old, kind), (new, _) = before[name], after[name]
        if old == 0 or new == 0:
            continue
        change = (old / new - 1) * 100 if kind == "time" else (new / old - 1) * 100
        rows.append((change, name, old, new, kind))
    rows.sort()

    worse = 0
    for change, name, old, new, kind in rows:
        mark = "!!" if change < -args.threshold else "  "
        worse += mark == "!!"
        spread = ""
        if several:
            spread = (
                f"  before {[round(v, 4) for v in before_runs[name]]} after {[round(v, 4) for v in after_runs[name]]}"
            )
        print(f"{mark}{change:+7.1f}%  {name}  [{old:g} -> {new:g} ({kind})]{spread}")

    for label, names in (("only before", before.keys() - after.keys()), ("only after", after.keys() - before.keys())):
        for name in sorted(names):
            print(f"  {label}: {name}")
    for label, names in (("failed before", failed_before), ("failed after", failed_after)):
        for name in names:
            print(f"  {label}: {name}")

    print(f"\n{len(rows)} compared, {worse} worse than -{args.threshold:g}%")
    return 1 if worse else 0


if __name__ == "__main__":
    sys.exit(main())
