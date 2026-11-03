#!/usr/bin/env python3
"""Build a synchronization submission and run A2 test cases."""

from __future__ import annotations

import argparse
import csv
import difflib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

OUTPUT_FILE = "results.csv"
MAX_CAPTURED_LINES = 100


def canonical_text(path: Path) -> str:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
    return text.rstrip("\n") + "\n"


def compile_submission(student_dir: Path) -> bool:
    try:
        clean = subprocess.run(
            ["make", "clean"], cwd=student_dir, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False,
        )
        build = subprocess.run(
            ["make"], cwd=student_dir, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"[BUILD FAILED] {error}")
        return False
    if build.returncode != 0:
        print("[BUILD FAILED]")
        if clean.stdout.strip():
            print(clean.stdout)
        print(build.stdout)
        return False
    print("[BUILD PASS]")
    return True


def csv_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    return (rows[0], rows[1:]) if rows else ([], [])


def describe_row(header: list[str], row: list[str] | None) -> str:
    if row is None:
        return "<no row>"
    return ", ".join(
        f"{header[index] if index < len(header) else f'column_{index + 1}'}="
        f"{value if value else '<blank>'}"
        for index, value in enumerate(row)
    )


def friendly_csv_diff(expected: Path, actual: Path, hint: str) -> str:
    expected_header, expected_rows = csv_rows(expected)
    actual_header, actual_rows = csv_rows(actual)
    lines: list[str] = []
    if expected_header != actual_header:
        lines.append("CSV header differs.")
        lines.append(f"  Expected: {', '.join(expected_header) or '<empty>'}")
        lines.append(f"  Actual:   {', '.join(actual_header) or '<empty>'}")
    else:
        shared = min(len(expected_rows), len(actual_rows))
        index = next((i for i in range(shared) if expected_rows[i] != actual_rows[i]), shared)
        expected_row = expected_rows[index] if index < len(expected_rows) else None
        actual_row = actual_rows[index] if index < len(actual_rows) else None
        lines.append(f"First difference at data row {index + 1} (CSV line {index + 2}).")
        lines.append(f"  Expected: {describe_row(expected_header, expected_row)}")
        lines.append(f"  Actual:   {describe_row(actual_header, actual_row)}")
        if expected_row is not None and actual_row is not None:
            lines.append("  Changed fields:")
            width = max(len(expected_header), len(expected_row), len(actual_row))
            for column in range(width):
                expected_value = expected_row[column] if column < len(expected_row) else "<missing>"
                actual_value = actual_row[column] if column < len(actual_row) else "<missing>"
                if expected_value != actual_value:
                    name = expected_header[column] if column < len(expected_header) else f"column_{column + 1}"
                    lines.append(f"    {name}: expected {expected_value}; actual {actual_value}")
        if len(expected_rows) != len(actual_rows):
            lines.append(f"  Row count: expected {len(expected_rows)}; actual {len(actual_rows)}.")
    if hint:
        lines.append(f"  Hint: {hint}")
    lines.append("  Use --unified-diff to display the complete line-by-line diff.")
    return "\n".join(lines) + "\n"


def complete_diff(expected: Path, actual: Path) -> str:
    return "".join(difflib.unified_diff(
        canonical_text(expected).splitlines(keepends=True),
        canonical_text(actual).splitlines(keepends=True),
        fromfile="expected/results.csv", tofile="actual/results.csv",
    ))


def return_code_message(return_code: int) -> str:
    if return_code >= 0:
        return f"program exited with status {return_code}"
    number = -return_code
    try:
        name = signal.Signals(number).name
    except ValueError:
        name = "unknown signal"
    explanations = {
        "SIGSEGV": "invalid memory access (segmentation fault)",
        "SIGABRT": "the program aborted, often after a failed assertion",
        "SIGFPE": "invalid arithmetic operation",
    }
    detail = f": {explanations[name]}" if name in explanations else ""
    return f"program terminated by signal {number} ({name}{detail})"


def print_captured(label: str, value: str | bytes | None) -> None:
    if not value:
        return
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    lines = value.rstrip().splitlines()
    if not lines:
        return
    print(f"{label}:")
    if len(lines) > MAX_CAPTURED_LINES:
        print("\n".join(lines[:75]))
        print(f"... {len(lines) - 100} lines omitted ...")
        print("\n".join(lines[-25:]))
    else:
        print("\n".join(lines))


def run_case(executable: Path, case_dir: Path, show_unified: bool) -> bool:
    manifest = json.loads((case_dir / "case.json").read_text(encoding="utf-8"))
    name = manifest.get("name", case_dir.name)
    repetitions = int(manifest.get("repetitions", 1))
    expected = case_dir / "expected" / OUTPUT_FILE
    for repetition in range(1, repetitions + 1):
        with tempfile.TemporaryDirectory(prefix="sysc4001_a2_") as temporary:
            output_dir = Path(temporary) / "output"
            command = [str(executable), *[str(value) for value in manifest["args"]],
                       "--output", str(output_dir)]
            try:
                completed = subprocess.run(
                    command, cwd=executable.parent, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    timeout=float(manifest.get("timeout", 10)), check=False,
                )
            except subprocess.TimeoutExpired as error:
                suffix = f" (repetition {repetition})" if repetitions > 1 else ""
                print(f"[FAIL] {name}{suffix}: execution timed out")
                print("  Likely cause: deadlock, a missed wakeup, or a lock that was not released.")
                print_captured("stdout", error.stdout)
                print_captured("stderr", error.stderr)
                return False
            if completed.returncode != 0:
                print(f"[FAIL] {name}: {return_code_message(completed.returncode)}")
                print_captured("stdout", completed.stdout)
                print_captured("stderr", completed.stderr)
                return False
            actual = output_dir / OUTPUT_FILE
            if not actual.exists():
                print(f"[FAIL] {name}: missing output file {OUTPUT_FILE}")
                print_captured("stdout", completed.stdout)
                print_captured("stderr", completed.stderr)
                return False
            if canonical_text(expected) != canonical_text(actual):
                print(f"[FAIL] {name}")
                print(friendly_csv_diff(expected, actual, manifest.get("hint", "")))
                print_captured("stdout", completed.stdout)
                print_captured("stderr", completed.stderr)
                if show_unified:
                    print("Complete unified diff:")
                    print(complete_diff(expected, actual))
                return False
    repeat_note = f" ({repetitions} repetitions)" if repetitions > 1 else ""
    print(f"[PASS] {name}{repeat_note}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--student-dir", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--tests", type=Path, default=Path("tests/public"))
    parser.add_argument("--case", metavar="CASE", help="run one case by directory name")
    parser.add_argument("--list-cases", action="store_true")
    parser.add_argument("--unified-diff", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()
    student_dir = args.student_dir.resolve()
    tests_dir = args.tests if args.tests.is_absolute() else student_dir / args.tests
    case_dirs = sorted(path.parent for path in tests_dir.resolve().rglob("case.json"))
    if not case_dirs:
        print(f"No case.json files found below {tests_dir}")
        return 2
    if args.list_cases:
        print("Available test cases:")
        for case_dir in case_dirs:
            manifest = json.loads((case_dir / "case.json").read_text(encoding="utf-8"))
            print(f"  {case_dir.name:<24} {manifest.get('name', case_dir.name)}")
        return 0
    if args.case:
        available = ", ".join(case.name for case in case_dirs)
        case_dirs = [case for case in case_dirs if case.name.casefold() == args.case.casefold()]
        if not case_dirs:
            print(f"Unknown test case '{args.case}'. Available cases: {available}")
            return 2
    if not args.no_compile and not compile_submission(student_dir):
        return 2
    executable = student_dir / ("sync_runner.exe" if os.name == "nt" else "sync_runner")
    if not executable.exists():
        print(f"Executable not found: {executable}")
        return 2
    passed = sum(run_case(executable, case, args.unified_diff) for case in case_dirs)
    print(f"\nResult: {passed}/{len(case_dirs)} test cases passed")
    return 0 if passed == len(case_dirs) else 1


if __name__ == "__main__":
    sys.exit(main())
