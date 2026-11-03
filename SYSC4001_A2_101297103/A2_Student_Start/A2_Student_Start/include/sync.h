#ifndef SYSC4001_SYNC_H
#define SYSC4001_SYNC_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * A non-recursive spinlock implemented with a C11 atomic flag.
 * The lock does not guarantee FIFO ordering or fairness.
 */
typedef struct {
    atomic_flag flag;
    bool initialized;
} StudentSpinlock;

/* Initialize lock in the unlocked state. Returns 0 or an errno-style code. */
int student_spin_init(StudentSpinlock *lock);

/* Wait until lock is acquired. lock must have been initialized. */
int student_spin_lock(StudentSpinlock *lock);

/* Release lock. The calling thread must hold it. */
int student_spin_unlock(StudentSpinlock *lock);

/*
 * A blocking counting semaphore. Its count is protected by mutex, and
 * condition is used to sleep and wake waiters. Do not replace this type with
 * sem_t or use POSIX semaphore functions in your implementation.
 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned int count;
    bool initialized;
} StudentSemaphore;

/* Initialize semaphore with initial_count available permits. */
int student_sem_init(StudentSemaphore *semaphore, unsigned int initial_count);

/* Wait for a permit, blocking without busy waiting when count is zero. */
int student_sem_wait(StudentSemaphore *semaphore);

/* Add one permit and wake an eligible waiter. */
int student_sem_post(StudentSemaphore *semaphore);

/* Destroy an unused semaphore. No thread may be waiting or using it. */
int student_sem_destroy(StudentSemaphore *semaphore);

/*
 * A bounded FIFO queue of integer job identifiers. slots tracks free entries,
 * items_available tracks queued entries, and guard protects queue metadata.
 */
typedef struct {
    int *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    StudentSemaphore slots;
    StudentSemaphore items_available;
    StudentSpinlock guard;
    bool initialized;
} WorkQueue;

/* Allocate an empty queue. capacity must be positive and no greater than UINT_MAX. */
int work_queue_init(WorkQueue *queue, size_t capacity);

/* Add job to the FIFO queue, blocking without busy waiting while it is full. */
int work_queue_submit(WorkQueue *queue, int job);

/* Remove the oldest job into *job, blocking without busy waiting while empty. */
int work_queue_take(WorkQueue *queue, int *job);

/* Destroy an unused queue and release its storage. The queue need not be empty. */
int work_queue_destroy(WorkQueue *queue);

#endif
