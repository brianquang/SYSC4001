# SYSC 4001 Assignment 2

The handout is the specification. This folder contains the public interface, starter file, test
programs, benchmark, public cases, and test harness.

## What to edit

Implement the functions in `src/student_sync.c`. You may add private `.c` or `.h` files if helpful;
the Makefile automatically includes additional `.c` files in `src/`. Do not change `include/sync.h`,
`src/test_driver.c`, `src/benchmark.c`, the public test files, or `test_harness.py`.

## Where to start

As in Assignment 1, begin by exploring the supplied code:

1. Read `include/sync.h` for the structures, function signatures, and preconditions.
2. Open `src/student_sync.c` for the implementation sections and design prompts.
3. Read `tests/public/spin_basic/case.json`, then follow its arguments into
   `src/test_driver.c` to see how one workload calls your functions and checks results.

The supplied driver handles thread creation, workloads, and CSV output. Your task is to implement
the synchronization library. You do not need to understand every workload before starting.
The untouched starter builds, but its functions return `ENOSYS` (not implemented), so the public
tests initially fail. Keep later sections as stubs while you work on the first primitive.

Work on the components in this order:

1. `StudentSpinlock`, using the supplied C11 `atomic_flag` and no pthread mutex.
2. `StudentSemaphore`, using its pthread mutex and condition variable and no `sem_t`.
3. `WorkQueue`, using your semaphore to block on full/empty conditions and your spinlock to protect
   the short FIFO metadata update.

## Running public tests

Build and run all public tests on Linux or WSL:

```text
make
python3 test_harness.py --no-compile
```

Start with one primitive and one small case:

```text
python3 test_harness.py --no-compile --case spin_basic
```

Then work through `semaphore_blocking`, `semaphore_counting`, `queue_fifo`, `queue_basic`, and
`queue_capacity_one`. List case names with:

```text
python3 test_harness.py --list-cases
```

After changing C source, run `make` again before using `--no-compile`, or omit `--no-compile` and let
the harness rebuild. A case that exceeds its time limit is usually deadlocked; inspect whether every
wait has a matching post and whether every acquired lock is released on every path.

When `results.csv` differs, the harness reports the first changed field by name. Add
`--unified-diff` to see the complete line-by-line comparison:

```text
python3 test_harness.py --no-compile --case queue_basic --unified-diff
```

## Running the supplied programs directly

The correctness runner interface is:

```text
./sync_runner --mode MODE --output OUTPUT_DIR [mode options]
```

The public `case.json` files show complete commands. The runner writes `results.csv`. It checks
counts and safety conditions because the threads do not have to run in the same order each time.

Use the benchmark only after the corresponding primitive works:

```text
./sync_benchmark --primitive spin --threads 1 --iterations 10000
./sync_benchmark --primitive semaphore --threads 1 --iterations 10000
./sync_benchmark --primitive spin --threads 8 --iterations 10000
./sync_benchmark --primitive semaphore --threads 8 --iterations 10000
```

Timing varies across machines and runs. Compare broad trends, confirm `expected == observed`, and do
not treat a single timing as a correctness test. Also check that `errors` is zero. A contended
semaphore run may take tens of seconds on some machines.

For the report, run each of the four configurations at least five times. Report a four-row table
with primitive, thread count, iterations per thread, number of runs, and median `elapsed_ms`.
Record your CPU model, available logical CPUs, Linux/WSL environment, and the four commands.
The report has two parts: timing and interpretation (15 marks), and fairness/observation
(10 marks), each with a 200-word explanation limit. The handout gives the full requirements; keep the report less than two pages.

## Checking your submission

Extract your submission ZIP into a clean directory and run `make clean` and `make` there.
Then, from the original `A2_Student_Start/` directory, run:

```text
python3 test_harness.py --student-dir /path/to/extracted/submission \
    --tests "$PWD/tests/public" --no-compile
```

Replace the submission path with the directory containing your submitted `Makefile`, `include/`,
and `src/`. The absolute `--tests` path uses the original public cases, which are not required in
your submission. Include `report.pdf` and, optionally, `README.txt`; omit build products.

## Checks worth adding while you work

- Exactly one thread can be inside a protected critical section.
- A semaphore wait rechecks its predicate after every condition-variable wakeup.
- `0 <= queue.count <= queue.capacity`.
- `queue.items_available.count + queue.slots.count` represents the queue capacity when no operation
  is between its two semaphore updates.
- Queue jobs are removed in FIFO order, and each successful submit enables one successful take.
- Never print while holding a spinlock unless temporarily diagnosing a tiny case; logging changes
  timing and can hide or expose races.
