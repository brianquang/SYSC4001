# SYSC 4001 Assignment 1

The complete specification is in the assignment handout. This package contains the supplied CSV input/output code, test harness, and source files.

## Running the public tests

Build and run all public tests on Linux or WSL:

```text
make
python3 test_harness.py --no-compile
```

It is usually easier to develop one scheduling policy at a time. Begin with the basic FCFS case:

```text
python3 test_harness.py --case fcfs_basic
```

List all available case names with:

```text
python3 test_harness.py --list-cases
```

After changing your C source, either run `make` again before using `--no-compile`, or omit `--no-compile` and let the harness rebuild the program. A useful progression is `fcfs_basic`, `fcfs_idle`, `fcfs_io`, `rr_quantum`, and finally the three SRTF cases.

`fcfs_idle` has an idle interval from time 3 to 7: the first process has finished,
and the second has not arrived. Run it with `python3 test_harness.py --case fcfs_idle`.

When a CSV file differs, the harness reports the first differing data row using named fields. Start with the first mismatch in `transitions.csv`; later timeline and metric differences are often consequences of that earlier scheduling decision. To also show the complete line-by-line diff, add `--unified-diff`; for example:

```text
make
python3 test_harness.py --no-compile --case fcfs_basic --unified-diff
```

If a program crashes on Linux, the harness translates common signal numbers. For example, signal 11 (`SIGSEGV`) means the scheduler made an invalid memory access.

## Running the scheduler directly

The executable interface is:

```text
./scheduler -i INPUT.csv -a fcfs|rr|srtf -q QUANTUM -c CONTEXT_SWITCH -o OUTPUT_DIR
```

All five options are required. `QUANTUM` must be positive and affects Round Robin only; `CONTEXT_SWITCH` must be nonnegative.

Input files contain 1–256 processes with distinct positive PIDs. Arrival times and
I/O durations are nonnegative; CPU durations and burst counts are positive. A process
with more than one CPU burst must have a positive I/O duration. All fields are integers
that fit in a signed 32-bit integer. Keep experimental workloads small so they run quickly.
The simulator's integer timestamps must also remain within that range.

Use a different output directory for each experiment: rerunning with the same `-o`
directory replaces its four CSV files. The parent of an output directory must exist.

Implement `run_simulation()` in `src/student_scheduler.c`. You may replace that file and add other `.c` or `.h` files. Do not change the supplied logging function signatures, CSV headers, command-line interface, or output filenames.

## Where to start

Open `src/student_scheduler.c` first. It contains:

- suggested `PCB` and whole-simulation structures;
- a commented list of optional private helpers and the required event order;
- examples of calls to the supplied transition and timeline writers; and
- an example of the one final `write_metrics()` call, which calculates and writes all metrics.

You may rename, combine, replace, or remove its private structures and helpers. The public types and supplied function signatures are documented in `include/scheduler.h`.
