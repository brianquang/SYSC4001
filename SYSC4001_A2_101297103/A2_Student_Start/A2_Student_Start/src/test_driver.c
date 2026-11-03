#include "sync.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    const char *mode;
    unsigned int threads;
    unsigned int operations;
    unsigned int waiters;
    unsigned int initial;
    unsigned int producers;
    unsigned int consumers;
    unsigned int jobs;
    unsigned int capacity;
    unsigned int delay_ms;
    const char *output_dir;
} Options;

typedef struct {
    const char *mode;
    unsigned long long expected;
    unsigned long long observed;
    unsigned long long violations;
    unsigned long long errors;
    unsigned long long blocked_before_release;
    unsigned long long expected_checksum;
    unsigned long long observed_checksum;
} Result;

/* pthread-style APIs return the error code directly; do not read errno. */
static int report_status(int status, const char *operation, int line)
{
    if (status != 0) {
        char message[256];
        if (strerror_r(status, message, sizeof(message)) != 0) {
            snprintf(message, sizeof(message), "Unknown error code");
        }
        fprintf(stderr, "test_driver.c:%d: %s returned %d (%s)\n",
                line, operation, status, message);
        fflush(stderr);
    }
    return status;
}

#define CHECK_CALL(operation) report_status((operation), #operation, __LINE__)

static int parse_uint(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT_MAX) {
        return EINVAL;
    }
    *value = (unsigned int)parsed;
    return 0;
}

static int parse_options(int argc, char **argv, Options *options)
{
    memset(options, 0, sizeof(*options));
    options->threads = 2;
    options->operations = 1000;
    options->waiters = 2;
    options->initial = 2;
    options->producers = 2;
    options->consumers = 2;
    options->jobs = 20;
    options->capacity = 4;
    options->delay_ms = 30;

    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) {
            return EINVAL;
        }
        const char *name = argv[i++];
        const char *value = argv[i];
        if (strcmp(name, "--mode") == 0) {
            options->mode = value;
        } else if (strcmp(name, "--threads") == 0) {
            if (parse_uint(value, &options->threads) != 0) return EINVAL;
        } else if (strcmp(name, "--operations") == 0) {
            if (parse_uint(value, &options->operations) != 0) return EINVAL;
        } else if (strcmp(name, "--waiters") == 0) {
            if (parse_uint(value, &options->waiters) != 0) return EINVAL;
        } else if (strcmp(name, "--initial") == 0) {
            if (parse_uint(value, &options->initial) != 0) return EINVAL;
        } else if (strcmp(name, "--producers") == 0) {
            if (parse_uint(value, &options->producers) != 0) return EINVAL;
        } else if (strcmp(name, "--consumers") == 0) {
            if (parse_uint(value, &options->consumers) != 0) return EINVAL;
        } else if (strcmp(name, "--jobs") == 0) {
            if (parse_uint(value, &options->jobs) != 0) return EINVAL;
        } else if (strcmp(name, "--capacity") == 0) {
            if (parse_uint(value, &options->capacity) != 0) return EINVAL;
        } else if (strcmp(name, "--delay-ms") == 0) {
            if (parse_uint(value, &options->delay_ms) != 0) return EINVAL;
        } else if (strcmp(name, "--output") == 0) {
            options->output_dir = value;
        } else {
            return EINVAL;
        }
    }
    if (options->mode == NULL || options->output_dir == NULL) return EINVAL;
    return 0;
}

static void delay_ms(unsigned int milliseconds)
{
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int write_result(const char *directory, const Result *result)
{
    char path[4096];
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) return errno;
    if (snprintf(path, sizeof(path), "%s/results.csv", directory) >= (int)sizeof(path)) {
        return ENAMETOOLONG;
    }
    FILE *file = fopen(path, "w");
    if (file == NULL) return errno;
    fprintf(file, "mode,expected_operations,observed_operations,violations,errors,blocked_before_release,expected_checksum,observed_checksum\n");
    fprintf(file, "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            result->mode, result->expected, result->observed,
            result->violations, result->errors,
            result->blocked_before_release, result->expected_checksum,
            result->observed_checksum);
    return fclose(file) == 0 ? 0 : EIO;
}

typedef struct {
    StudentSpinlock *lock;
    unsigned int operations;
    volatile unsigned long long *counter;
    atomic_uint *inside;
    atomic_ullong *violations;
    atomic_ullong *errors;
} SpinWorker;

static void *spin_worker(void *argument)
{
    SpinWorker *worker = argument;
    for (unsigned int i = 0; i < worker->operations; ++i) {
        if (student_spin_lock(worker->lock) != 0) {
            atomic_fetch_add(worker->errors, 1);
            return NULL;
        }
        if (atomic_fetch_add(worker->inside, 1) != 0) {
            atomic_fetch_add(worker->violations, 1);
        }
        unsigned long long next = *worker->counter + 1;
        if ((i % 97U) == 0U) sched_yield();
        *worker->counter = next;
        if (atomic_fetch_sub(worker->inside, 1) != 1) {
            atomic_fetch_add(worker->violations, 1);
        }
        if (student_spin_unlock(worker->lock) != 0) {
            atomic_fetch_add(worker->errors, 1);
            return NULL;
        }
    }
    return NULL;
}

static Result run_spin(const Options *options)
{
    Result result = {.mode = "spin"};
    result.expected = (unsigned long long)options->threads * options->operations;
    if (options->threads == 0 || options->operations == 0) {
        result.errors = 1;
        return result;
    }
    StudentSpinlock lock;
    int status = student_spin_init(&lock);
    if (status != 0) {
        result.errors = 1;
        return result;
    }
    pthread_t *threads = calloc(options->threads, sizeof(*threads));
    SpinWorker *workers = calloc(options->threads, sizeof(*workers));
    volatile unsigned long long counter = 0;
    atomic_uint inside = 0;
    atomic_ullong violations = 0;
    atomic_ullong errors = 0;
    if (threads == NULL || workers == NULL) {
        result.errors = 1;
        free(threads);
        free(workers);
        return result;
    }
    unsigned int created = 0;
    for (; created < options->threads; ++created) {
        workers[created] = (SpinWorker){&lock, options->operations, &counter,
                                       &inside, &violations, &errors};
        if (pthread_create(&threads[created], NULL, spin_worker, &workers[created]) != 0) {
            atomic_fetch_add(&errors, 1);
            break;
        }
    }
    for (unsigned int i = 0; i < created; ++i) {
        if (pthread_join(threads[i], NULL) != 0) atomic_fetch_add(&errors, 1);
    }
    result.observed = counter;
    result.violations = atomic_load(&violations);
    result.errors = atomic_load(&errors);
    free(threads);
    free(workers);
    return result;
}

typedef struct {
    StudentSemaphore *semaphore;
    atomic_uint *started;
    atomic_ullong *completed;
    atomic_ullong *errors;
} SemWorker;

static void *sem_worker(void *argument)
{
    SemWorker *worker = argument;
    atomic_fetch_add(worker->started, 1);
    int status = CHECK_CALL(student_sem_wait(worker->semaphore));
    if (status != 0) atomic_fetch_add(worker->errors, 1);
    else atomic_fetch_add(worker->completed, 1);
    return NULL;
}

static Result run_semaphore_block(const Options *options)
{
    Result result = {.mode = "semaphore_block", .expected = options->waiters,
                     .blocked_before_release = options->waiters};
    if (options->waiters == 0) {
        result.errors = 1;
        return result;
    }
    StudentSemaphore semaphore;
    if (CHECK_CALL(student_sem_init(&semaphore, 0)) != 0) {
        result.errors = 1;
        return result;
    }
    pthread_t *threads = calloc(options->waiters, sizeof(*threads));
    SemWorker *workers = calloc(options->waiters, sizeof(*workers));
    atomic_uint started = 0;
    atomic_ullong completed = 0;
    atomic_ullong errors = 0;
    if (threads == NULL || workers == NULL) {
        result.errors = 1;
        free(threads);
        free(workers);
        CHECK_CALL(student_sem_destroy(&semaphore));
        return result;
    }
    unsigned int created = 0;
    for (; created < options->waiters; ++created) {
        workers[created] = (SemWorker){&semaphore, &started, &completed, &errors};
        if (CHECK_CALL(pthread_create(&threads[created], NULL, sem_worker, &workers[created])) != 0) {
            atomic_fetch_add(&errors, 1);
            break;
        }
    }
    while (atomic_load(&started) < created) sched_yield();
    delay_ms(options->delay_ms);
    unsigned long long completed_before = atomic_load(&completed);
    result.blocked_before_release = created - completed_before;
    for (unsigned int i = 0; i < created; ++i) {
        if (CHECK_CALL(student_sem_post(&semaphore)) != 0) atomic_fetch_add(&errors, 1);
    }
    for (unsigned int i = 0; i < created; ++i) {
        if (CHECK_CALL(pthread_join(threads[i], NULL)) != 0) atomic_fetch_add(&errors, 1);
    }
    result.observed = atomic_load(&completed);
    result.violations = completed_before;
    if (CHECK_CALL(student_sem_destroy(&semaphore)) != 0) atomic_fetch_add(&errors, 1);
    result.errors = atomic_load(&errors);
    free(threads);
    free(workers);
    return result;
}

static Result run_semaphore_count(const Options *options)
{
    Result result = {.mode = "semaphore_count", .blocked_before_release = 1};
    result.expected = (unsigned long long)options->initial * 2ULL + 1ULL;
    if (options->initial == 0) {
        result.errors = 1;
        return result;
    }
    StudentSemaphore semaphore;
    if (CHECK_CALL(student_sem_init(&semaphore, options->initial)) != 0) {
        result.errors = 1;
        return result;
    }
    atomic_uint started = 0;
    atomic_ullong completed = 0;
    atomic_ullong errors = 0;
    for (unsigned int i = 0; i < options->initial; ++i) {
        if (CHECK_CALL(student_sem_wait(&semaphore)) == 0) result.observed++;
        else result.errors++;
    }
    pthread_t thread;
    SemWorker worker = {&semaphore, &started, &completed, &errors};
    bool created = CHECK_CALL(pthread_create(&thread, NULL, sem_worker, &worker)) == 0;
    if (!created) result.errors++;
    if (created) {
        while (atomic_load(&started) == 0) sched_yield();
        delay_ms(options->delay_ms);
        unsigned long long before = atomic_load(&completed);
        result.violations += before;
        result.blocked_before_release = 1ULL - before;
        if (CHECK_CALL(student_sem_post(&semaphore)) != 0) result.errors++;
        if (CHECK_CALL(pthread_join(thread, NULL)) != 0) result.errors++;
        result.observed += atomic_load(&completed);
    }
    for (unsigned int i = 0; i < options->initial; ++i) {
        if (CHECK_CALL(student_sem_post(&semaphore)) != 0) result.errors++;
    }
    for (unsigned int i = 0; i < options->initial; ++i) {
        if (CHECK_CALL(student_sem_wait(&semaphore)) == 0) result.observed++;
        else result.errors++;
    }
    result.errors += atomic_load(&errors);
    if (CHECK_CALL(student_sem_destroy(&semaphore)) != 0) result.errors++;
    return result;
}

static Result run_semaphore_overflow(void)
{
    Result result = {.mode = "semaphore_overflow", .expected = 1};
    StudentSemaphore semaphore;
    if (CHECK_CALL(student_sem_init(&semaphore, UINT_MAX)) != 0) {
        result.errors = 1;
        return result;
    }
    int status = student_sem_post(&semaphore);
    if (status == EOVERFLOW) result.observed = 1;
    else result.violations = 1;
    if (CHECK_CALL(student_sem_destroy(&semaphore)) != 0) result.errors++;
    return result;
}

typedef struct {
    WorkQueue *queue;
    unsigned int first_job;
    unsigned int job_count;
    atomic_ullong *errors;
} Producer;

static void *producer_worker(void *argument)
{
    Producer *producer = argument;
    for (unsigned int i = 0; i < producer->job_count; ++i) {
        if (work_queue_submit(producer->queue, (int)(producer->first_job + i)) != 0) {
            atomic_fetch_add(producer->errors, 1);
            return NULL;
        }
    }
    return NULL;
}

typedef struct {
    WorkQueue *queue;
    unsigned int job_count;
    unsigned int total_jobs;
    atomic_uint *seen;
    atomic_ullong *consumed;
    atomic_ullong *out_of_range;
    atomic_ullong *errors;
    atomic_ullong *checksum;
} Consumer;

static void *consumer_worker(void *argument)
{
    Consumer *consumer = argument;
    for (unsigned int i = 0; i < consumer->job_count; ++i) {
        int job = 0;
        if (work_queue_take(consumer->queue, &job) != 0) {
            atomic_fetch_add(consumer->errors, 1);
            return NULL;
        }
        atomic_fetch_add(consumer->consumed, 1);
        if (job < 1 || (unsigned int)job > consumer->total_jobs) {
            atomic_fetch_add(consumer->out_of_range, 1);
        } else {
            atomic_fetch_add(&consumer->seen[job - 1], 1);
            atomic_fetch_add(consumer->checksum, (unsigned int)job);
        }
    }
    return NULL;
}

static Result run_queue(const Options *options)
{
    Result result = {.mode = "queue", .expected = options->jobs};
    result.expected_checksum = (unsigned long long)options->jobs * (options->jobs + 1ULL) / 2ULL;
    if (options->producers == 0 || options->consumers == 0 ||
        options->jobs == 0 || options->capacity == 0) {
        result.errors = 1;
        return result;
    }
    WorkQueue queue;
    if (work_queue_init(&queue, options->capacity) != 0) {
        result.errors = 1;
        return result;
    }
    pthread_t *producer_threads = calloc(options->producers, sizeof(*producer_threads));
    pthread_t *consumer_threads = calloc(options->consumers, sizeof(*consumer_threads));
    Producer *producers = calloc(options->producers, sizeof(*producers));
    Consumer *consumers = calloc(options->consumers, sizeof(*consumers));
    atomic_uint *seen = calloc(options->jobs, sizeof(*seen));
    atomic_ullong consumed = 0, out_of_range = 0, errors = 0, checksum = 0;
    if (!producer_threads || !consumer_threads || !producers || !consumers || !seen) {
        result.errors = 1;
        goto cleanup;
    }

    unsigned int next_job = 1;
    unsigned int producers_created = 0, consumers_created = 0;
    for (; consumers_created < options->consumers; ++consumers_created) {
        unsigned int count = options->jobs / options->consumers;
        if (consumers_created < options->jobs % options->consumers) count++;
        consumers[consumers_created] = (Consumer){&queue, count, options->jobs, seen,
            &consumed, &out_of_range, &errors, &checksum};
        if (pthread_create(&consumer_threads[consumers_created], NULL,
                           consumer_worker, &consumers[consumers_created]) != 0) {
            atomic_fetch_add(&errors, 1);
            break;
        }
    }
    for (; producers_created < options->producers; ++producers_created) {
        unsigned int count = options->jobs / options->producers;
        if (producers_created < options->jobs % options->producers) count++;
        producers[producers_created] = (Producer){&queue, next_job, count, &errors};
        next_job += count;
        if (pthread_create(&producer_threads[producers_created], NULL,
                           producer_worker, &producers[producers_created]) != 0) {
            atomic_fetch_add(&errors, 1);
            break;
        }
    }
    for (unsigned int i = 0; i < producers_created; ++i) {
        if (pthread_join(producer_threads[i], NULL) != 0) atomic_fetch_add(&errors, 1);
    }
    for (unsigned int i = 0; i < consumers_created; ++i) {
        if (pthread_join(consumer_threads[i], NULL) != 0) atomic_fetch_add(&errors, 1);
    }

    unsigned long long duplicates = 0, missing = 0;
    for (unsigned int i = 0; i < options->jobs; ++i) {
        unsigned int count = atomic_load(&seen[i]);
        if (count == 0) missing++;
        else if (count > 1) duplicates += count - 1U;
    }
    result.observed = atomic_load(&consumed);
    result.observed_checksum = atomic_load(&checksum);
    result.violations = duplicates + missing + atomic_load(&out_of_range);
    if (queue.count != 0) result.violations++;
    result.errors = atomic_load(&errors);

cleanup:
    if (work_queue_destroy(&queue) != 0) result.errors++;
    free(producer_threads);
    free(consumer_threads);
    free(producers);
    free(consumers);
    free(seen);
    return result;
}

static Result run_queue_fifo(const Options *options)
{
    Result result = {.mode = "queue_fifo", .expected = options->jobs};
    result.expected_checksum = (unsigned long long)options->jobs * (options->jobs + 1ULL) / 2ULL;
    if (options->jobs == 0 || options->capacity == 0) {
        result.errors = 1;
        return result;
    }
    WorkQueue queue;
    if (work_queue_init(&queue, options->capacity) != 0) {
        result.errors = 1;
        return result;
    }
    unsigned int next = 1;
    while (next <= options->jobs) {
        unsigned int remaining = options->jobs - next + 1U;
        unsigned int batch = remaining < options->capacity ? remaining : options->capacity;
        for (unsigned int offset = 0; offset < batch; ++offset) {
            if (work_queue_submit(&queue, (int)(next + offset)) != 0) result.errors++;
            else if (queue.count != offset + 1U) result.violations++;
        }
        for (unsigned int offset = 0; offset < batch; ++offset) {
            int job = 0;
            if (work_queue_take(&queue, &job) != 0) {
                result.errors++;
                continue;
            }
            result.observed++;
            result.observed_checksum += (unsigned int)job;
            if (job != (int)(next + offset)) result.violations++;
            if (queue.count != batch - offset - 1U) result.violations++;
        }
        next += batch;
    }
    if (work_queue_destroy(&queue) != 0) result.errors++;
    return result;
}

static Result run_invalid_arguments(void)
{
    Result result = {.mode = "invalid_arguments", .expected = 7};
    WorkQueue queue;
    result.observed += student_spin_init(NULL) == EINVAL;
    result.observed += student_spin_lock(NULL) == EINVAL;
    result.observed += student_sem_init(NULL, 0) == EINVAL;
    result.observed += student_sem_wait(NULL) == EINVAL;
    result.observed += work_queue_init(NULL, 1) == EINVAL;
    result.observed += work_queue_init(&queue, 0) == EINVAL;
    result.observed += work_queue_take(NULL, NULL) == EINVAL;
    result.violations = result.expected - result.observed;
    return result;
}

int main(int argc, char **argv)
{
    Options options;
    if (parse_options(argc, argv, &options) != 0) {
        fprintf(stderr, "Usage: %s --mode MODE --output DIR [mode options]\n", argv[0]);
        return 2;
    }
    Result result;
    if (strcmp(options.mode, "spin") == 0) result = run_spin(&options);
    else if (strcmp(options.mode, "semaphore_block") == 0) result = run_semaphore_block(&options);
    else if (strcmp(options.mode, "semaphore_count") == 0) result = run_semaphore_count(&options);
    else if (strcmp(options.mode, "semaphore_overflow") == 0) result = run_semaphore_overflow();
    else if (strcmp(options.mode, "queue") == 0) result = run_queue(&options);
    else if (strcmp(options.mode, "queue_fifo") == 0) result = run_queue_fifo(&options);
    else if (strcmp(options.mode, "invalid_arguments") == 0) result = run_invalid_arguments();
    else {
        fprintf(stderr, "Unknown mode: %s\n", options.mode);
        return 2;
    }
    int status = write_result(options.output_dir, &result);
    if (status != 0) {
        fprintf(stderr, "Could not write results.csv: %s\n", strerror(status));
        return 2;
    }
    return 0;
}
