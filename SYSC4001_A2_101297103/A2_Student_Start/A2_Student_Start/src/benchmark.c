#include "sync.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { USE_SPIN, USE_SEMAPHORE } Primitive;

typedef struct {
    Primitive primitive;
    StudentSpinlock *spinlock;
    StudentSemaphore *semaphore;
    unsigned int iterations;
    unsigned long long *counter;
    int error;
} Worker;

static void *run_worker(void *argument)
{
    Worker *worker = argument;
    for (unsigned int i = 0; i < worker->iterations; ++i) {
        int status = worker->primitive == USE_SPIN
            ? student_spin_lock(worker->spinlock)
            : student_sem_wait(worker->semaphore);
        if (status != 0) {
            worker->error = status;
            return NULL;
        }
        (*worker->counter)++;
        status = worker->primitive == USE_SPIN
            ? student_spin_unlock(worker->spinlock)
            : student_sem_post(worker->semaphore);
        if (status != 0) {
            worker->error = status;
            return NULL;
        }
    }
    return NULL;
}

static int parse_uint(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 || parsed > 100000000UL) return -1;
    *value = (unsigned int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    const char *primitive_name = NULL;
    unsigned int thread_count = 1, iterations = 100000;
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) return 2;
        if (strcmp(argv[i], "--primitive") == 0) primitive_name = argv[++i];
        else if (strcmp(argv[i], "--threads") == 0 && parse_uint(argv[++i], &thread_count) == 0) {}
        else if (strcmp(argv[i], "--iterations") == 0 && parse_uint(argv[++i], &iterations) == 0) {}
        else return 2;
    }
    Primitive primitive;
    if (primitive_name != NULL && strcmp(primitive_name, "spin") == 0) primitive = USE_SPIN;
    else if (primitive_name != NULL && strcmp(primitive_name, "semaphore") == 0) primitive = USE_SEMAPHORE;
    else {
        fprintf(stderr, "Usage: %s --primitive spin|semaphore --threads N --iterations N\n", argv[0]);
        return 2;
    }

    StudentSpinlock spinlock;
    StudentSemaphore semaphore;
    int status = primitive == USE_SPIN
        ? student_spin_init(&spinlock)
        : student_sem_init(&semaphore, 1);
    if (status != 0) {
        fprintf(stderr, "Primitive initialization failed with status %d\n", status);
        return 1;
    }

    pthread_t *threads = calloc(thread_count, sizeof(*threads));
    Worker *workers = calloc(thread_count, sizeof(*workers));
    if (threads == NULL || workers == NULL) return 1;
    unsigned long long counter = 0;
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (unsigned int i = 0; i < thread_count; ++i) {
        workers[i] = (Worker){primitive, &spinlock, &semaphore, iterations, &counter, 0};
        if (pthread_create(&threads[i], NULL, run_worker, &workers[i]) != 0) return 1;
    }
    for (unsigned int i = 0; i < thread_count; ++i) pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    long long elapsed_ns = (finish.tv_sec - start.tv_sec) * 1000000000LL
                         + (finish.tv_nsec - start.tv_nsec);
    int errors = 0;
    for (unsigned int i = 0; i < thread_count; ++i) errors += workers[i].error != 0;
    if (primitive == USE_SEMAPHORE) errors += student_sem_destroy(&semaphore) != 0;
    printf("primitive,threads,iterations_per_thread,elapsed_ms,expected,observed,errors\n");
    printf("%s,%u,%u,%.3f,%llu,%llu,%d\n", primitive_name, thread_count, iterations,
           elapsed_ns / 1000000.0,
           (unsigned long long)thread_count * iterations, counter, errors);
    free(threads);
    free(workers);
    return errors == 0 ? 0 : 1;
}
