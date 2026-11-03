#!/usr/bin/env python3
"""Compile the scheduler and compare its CSV files with expected outputs."""

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

OUTPUT_FILES = ("transitions.csv", "timeline.csv", "metrics.csv", "summary.csv")
MAX_CAPTURED_LINES = 100

FILE_HINTS = {
    "transitions.csv": (
        "Start with this first transition mismatch; later output differences are often consequences of it."
    ),
    "timeline.csv": "Each timeline row describes the half-open interval [start, end).",
    "metrics.csv": "Metric differences usually follow from earlier transition or timeline timing.",
    "summary.csv": "Summary differences usually follow from earlier timeline differences.",
}


def canonical_text(path: Path) -> str:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
    return text.rstrip("\n") + "\n"


def compile_submission(student_dir: Path) -> bool:
    clean = subprocess.run(
        ["make", "clean"], cwd=student_dir, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False,
    )
    if clean.returncode != 0:
        print("[BUILD FAILED] make clean failed; refusing to use stale objects.")
        print(clean.stdout)
        return False
    build = subprocess.run(
        ["make"], cwd=student_dir, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False,
    )
    if build.returncode != 0:
        print("[BUILD FAILED]")
        print(clean.stdout)
        print(build.stdout)
        return False
    print("[BUILD PASS]")
    return True


def csv_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        return [], []
    return rows[0], rows[1:]


def describe_row(header: list[str], row: list[str] | None) -> str:
    if row is None:
        return "<no row>"
    fields = []
    for index, value in enumerate(row):
        name = header[index] if index < len(header) else f"column_{index + 1}"
        fields.append(f"{name}={value if value else '<blank>'}")
    for index in range(len(row), len(header)):
        fields.append(f"{header[index]}=<missing>")
    return ", ".join(fields) if fields else "<empty row>"


def friendly_csv_diff(expected: Path, actual: Path) -> str:
    expected_header, expected_rows = csv_rows(expected)
    actual_header, actual_rows = csv_rows(actual)
    lines: list[str] = []

    if expected_header != actual_header:
        lines.append("CSV header differs.")
        lines.append(f"  Expected: {', '.join(expected_header) or '<empty>'}")
        lines.append(f"  Actual:   {', '.join(actual_header) or '<empty>'}")
        return "\n".join(lines) + "\n"

    shared_rows = min(len(expected_rows), len(actual_rows))
    if expected_rows == actual_rows:
        return ("CSV values match, but text formatting differs (for example quoting). "
                "Use the supplied CSV writers.\n")
    mismatch = next(
        (index for index in range(shared_rows)
         if expected_rows[index] != actual_rows[index]),
        shared_rows,
    )
    expected_row = expected_rows[mismatch] if mismatch < len(expected_rows) else None
    actual_row = actual_rows[mismatch] if mismatch < len(actual_rows) else None

    lines.append(
        f"First difference at data row {mismatch + 1} "
        f"(CSV line {mismatch + 2})."
    )
    lines.append(f"  Expected: {describe_row(expected_header, expected_row)}")
    lines.append(f"  Actual:   {describe_row(actual_header, actual_row)}")

    if expected_row is not None and actual_row is not None:
        changed_fields = []
        width = max(len(expected_header), len(expected_row), len(actual_row))
        for index in range(width):
            expected_value = expected_row[index] if index < len(expected_row) else "<missing>"
            actual_value = actual_row[index] if index < len(actual_row) else "<missing>"
            if expected_value != actual_value:
                name = expected_header[index] if index < len(expected_header) else f"column_{index + 1}"
                changed_fields.append(
                    f"    {name}: expected {expected_value or '<blank>'}; "
                    f"actual {actual_value or '<blank>'}"
                )
        if changed_fields:
            lines.append("  Changed fields:")
            lines.extend(changed_fields)

    if len(expected_rows) != len(actual_rows):
        lines.append(
            f"  Row count: expected {len(expected_rows)} data rows; "
            f"actual {len(actual_rows)}."
        )

    hint = FILE_HINTS.get(expected.name)
    if hint:
        lines.append(f"  Hint: {hint}")
    lines.append("  Use --unified-diff to display the complete line-by-line diff.")
    return "\n".join(lines) + "\n"


def unified_diff(expected_text: str, actual_text: str, filename: str) -> str:
    diff = difflib.unified_diff(
        expected_text.splitlines(keepends=True), actual_text.splitlines(keepends=True),
        fromfile=f"expected/{filename}", tofile=f"actual/{filename}",
    )
    lines = list(diff)
    if len(lines) > 200:
        lines = lines[:200] + ["... diff truncated after 200 lines ...\n"]
    return "".join(lines)


def compare_file(expected: Path, actual: Path, show_unified_diff: bool) -> tuple[bool, str]:
    if not actual.exists():
        return False, f"missing output file: {actual.name}"
    if not expected.exists():
        return False, f"test package is missing expected file: {expected}"
    try:
        expected_text = canonical_text(expected)
        actual_text = canonical_text(actual)
    except (OSError, UnicodeError) as error:
        return False, f"Cannot read {actual.name}: {error}"
    if expected_text == actual_text:
        return True, ""
    message = friendly_csv_diff(expected, actual)
    if show_unified_diff:
        message += "\nComplete unified diff:\n"
        message += unified_diff(expected_text, actual_text, expected.name)
    return False, message


def print_captured_output(label: str, output: str) -> None:
    lines = output.rstrip().splitlines()
    if not lines:
        return
    print(f"{label}:")
    if len(lines) <= MAX_CAPTURED_LINES:
        print("\n".join(lines))
        return
    leading_count = 75
    trailing_count = 25
    omitted = len(lines) - leading_count - trailing_count
    print("\n".join(lines[:leading_count]))
    print(f"... {omitted} lines omitted ...")
    print("\n".join(lines[-trailing_count:]))


def return_code_message(return_code: int) -> str:
    if return_code >= 0:
        return f"scheduler exited with status {return_code}"
    signal_number = -return_code
    try:
        signal_name = signal.Signals(signal_number).name
    except ValueError:
        signal_name = "unknown signal"
    explanations = {
        "SIGSEGV": "invalid memory access (segmentation fault)",
        "SIGABRT": "the program aborted, often because an assertion failed",
        "SIGFPE": "invalid arithmetic operation, such as division by zero",
    }
    explanation = explanations.get(signal_name)
    suffix = f": {explanation}" if explanation else ""
    return f"scheduler terminated by signal {signal_number} ({signal_name}{suffix})"


def run_case(executable: Path, case_dir: Path, show_unified_diff: bool) -> bool:
    manifest = json.loads((case_dir / "case.json").read_text(encoding="utf-8"))
    case_name = manifest.get("name", case_dir.name)
    input_path = case_dir / manifest.get("input", "input.csv")
    expected_dir = case_dir / "expected"
    with tempfile.TemporaryDirectory(prefix="sysc4001_a1_") as temporary:
        output_dir = Path(temporary) / "output"
        command = [
            str(executable), "-i", str(input_path), "-a", manifest["algorithm"],
            "-q", str(manifest.get("quantum", 2)),
            "-c", str(manifest.get("context_switch", 0)), "-o", str(output_dir),
        ]
        try:
            completed = subprocess.run(
                command, cwd=executable.parent, text=True, errors="replace",
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=10, check=False,
            )
        except subprocess.TimeoutExpired:
            print(f"[FAIL] {case_name}: execution exceeded 10 seconds")
            return False
        except OSError as error:
            print(f"[FAIL] {case_name}: cannot launch scheduler: {error}")
            return False
        if completed.returncode != 0:
            print(f"[FAIL] {case_name}: {return_code_message(completed.returncode)}")
            print_captured_output("stdout", completed.stdout)
            print_captured_output("stderr", completed.stderr)
            return False

        passed = True
        messages: list[str] = []
        for filename in OUTPUT_FILES:
            file_passed, message = compare_file(
                expected_dir / filename,
                output_dir / filename,
                show_unified_diff,
            )
            if not file_passed:
                passed = False
                messages.append(f"\n--- {filename} ---\n{message}")
        if passed:
            print(f"[PASS] {case_name}")
        else:
            print(f"[FAIL] {case_name}")
            print("".join(messages))
        return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--student-dir", type=Path, default=Path(__file__).resolve().parent,
        help="directory containing the student's Makefile (default: this package)",
    )
    parser.add_argument(
        "--tests", type=Path, default=Path("tests/public"),
        help="test-set directory; relative paths are resolved under --student-dir",
    )
    parser.add_argument(
        "--case", metavar="CASE",
        help="run one case by directory name, for example: --case fcfs_basic",
    )
    parser.add_argument(
        "--list-cases", action="store_true",
        help="list the cases found under --tests and exit without compiling",
    )
    parser.add_argument(
        "--unified-diff", action="store_true",
        help="also display the complete line-by-line diff for mismatched CSV files",
    )
    parser.add_argument("--no-compile", action="store_true", help="skip make clean && make")
    args = parser.parse_args()
    student_dir = args.student_dir.resolve()
    tests_dir = args.tests if args.tests.is_absolute() else student_dir / args.tests
    tests_dir = tests_dir.resolve()
    case_dirs = sorted(path.parent for path in tests_dir.rglob("case.json"))
    if not case_dirs:
        print(f"No case.json files found below {tests_dir}")
        return 2
    if args.list_cases:
        print("Available test cases:")
        for case_dir in case_dirs:
            manifest = json.loads((case_dir / "case.json").read_text(encoding="utf-8"))
            print(
                f"  {case_dir.name:<22} {manifest.get('name', case_dir.name)} "
                f"[{manifest.get('algorithm', 'unknown')}]"
            )
        return 0
    if args.case:
        matching_cases = [
            case_dir for case_dir in case_dirs
            if case_dir.name.casefold() == args.case.casefold()
        ]
        if not matching_cases:
            available = ", ".join(case_dir.name for case_dir in case_dirs)
            print(f"Unknown test case '{args.case}'. Available cases: {available}")
            return 2
        case_dirs = matching_cases
    if not args.no_compile and not compile_submission(student_dir):
        return 2
    executable = student_dir / ("scheduler.exe" if os.name == "nt" else "scheduler")
    if not executable.exists():
        print(f"Executable not found: {executable}")
        return 2
    passed = sum(
        run_case(executable, case_dir, args.unified_diff)
        for case_dir in case_dirs
    )
    total = len(case_dirs)
    print(f"\nResult: {passed}/{total} test cases passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"[HARNESS ERROR] {error}", file=sys.stderr)
        sys.exit(2)
