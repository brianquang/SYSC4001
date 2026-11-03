#include "sync.h"
#include <stdlib.h>
#include <errno.h>
#include <sched.h>
#include <limits.h> // ~~ Note this include... Why might we need this? 

/*
 * Complete one section at a time. It is fine to leave later functions
 * returning ENOSYS while you work on the spinlock or semaphore.
 *
 * Suggested public-test order:
 *   spin_basic
 *   semaphore_blocking, then semaphore_counting
 *   queue_fifo, then queue_basic and queue_capacity_one
 *
 * You may add private helpers or source files. Do not change sync.h.
 */

/*
 * Spinlock
 *
 * Decide which atomic operation claims the flag and which memory ordering is
 * needed on lock and unlock. Check the pointer and initialized flag before
 * touching the atomic flag.
 */

int student_spin_init(StudentSpinlock *lock)
{
    if (lock == NULL) {
        return EINVAL;
    }

    atomic_flag_clear(&lock->flag);
    lock->initialized = true;

    return 0;
}

int student_spin_lock(StudentSpinlock *lock)
{
    if (lock == NULL || !lock->initialized) {
        return EINVAL;
    }

    while (atomic_flag_test_and_set_explicit(
               &lock->flag, memory_order_acquire)) {
        sched_yield();
    }

    return 0;
}

int student_spin_unlock(StudentSpinlock *lock)
{
    if (lock == NULL || !lock->initialized) {
        return EINVAL;
    }

    atomic_flag_clear_explicit(&lock->flag, memory_order_relaxed);

    return 0;
}

/*
 * Semaphore
 *
 * Keep every access to count protected by the pthread_mutex semaphore->mutex. 
 */

int student_sem_init(StudentSemaphore *semaphore, unsigned int initial_count)
{
    if (semaphore == NULL) {
        return EINVAL;
    }

    int status = pthread_mutex_init(&semaphore->mutex, NULL);
    if (status != 0) {
        return status;
    }

    status = pthread_cond_init(&semaphore->condition, NULL);
    if (status != 0) {
        pthread_mutex_destroy(&semaphore->mutex);
        return status;
    }

    semaphore->count = initial_count;
    semaphore->initialized = true;
    return 0;
}

int student_sem_wait(StudentSemaphore *semaphore)
{
    if (semaphore == NULL || !semaphore->initialized) {
        return EINVAL;
    }

    int status = pthread_mutex_lock(&semaphore->mutex);
    if (status != 0) {
        return status;
    }

    while (semaphore->count == 0) {
        status = pthread_cond_wait(&semaphore->condition, &semaphore->mutex);
        if (status != 0) {
            pthread_mutex_unlock(&semaphore->mutex);
            return status;
        }
    }

    semaphore->count--;
    status = pthread_mutex_unlock(&semaphore->mutex);
    return status;
}

int student_sem_post(StudentSemaphore *semaphore)
{
    if (semaphore == NULL || !semaphore->initialized) {
        return EINVAL;
    }

    int status = pthread_mutex_lock(&semaphore->mutex);
    if (status != 0) {
        return status;
    }

    if (semaphore->count == UINT_MAX) {
        pthread_mutex_unlock(&semaphore->mutex);
        return EOVERFLOW;
    }

    semaphore->count++;
    pthread_cond_signal(&semaphore->condition);

    status = pthread_mutex_unlock(&semaphore->mutex);
    return status;
}

int student_sem_destroy(StudentSemaphore *semaphore)
{
    if (semaphore == NULL || !semaphore->initialized) {
        return EINVAL;
    }

    int status = pthread_cond_destroy(&semaphore->condition);
    if (status != 0) {
        return status;
    }

    status = pthread_mutex_destroy(&semaphore->mutex);
    if (status != 0) {
        return status;
    }

    semaphore->initialized = false;
    return 0;
}

/*
 * Work queue
 *
 * Check the WorkQueue definition in the header file for details on its structure and 
 * synchronization requirements.
 */
int work_queue_init(WorkQueue *queue, size_t capacity)
{
    if (queue == NULL || capacity == 0 || capacity > UINT_MAX) {
        return EINVAL;
    }

    queue->items = calloc(capacity, sizeof(*queue->items));
    if (queue->items == NULL) {
        return ENOMEM;
    }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->initialized = false;

    int status = student_spin_init(&queue->guard);
    if (status != 0) {
        free(queue->items);
        queue->items = NULL;
        return status;
    }

    status = student_sem_init(&queue->slots, (unsigned int)capacity);
    if (status != 0) {
        free(queue->items);
        queue->items = NULL;
        return status;
    }

    status = student_sem_init(&queue->items_available, 0);
    if (status != 0) {
        student_sem_destroy(&queue->slots);
        free(queue->items);
        queue->items = NULL;
        return status;
    }

    queue->initialized = true;
    return 0;
}

int work_queue_submit(WorkQueue *queue, int job)
{
    if (queue == NULL || !queue->initialized) {
        return EINVAL;
    }

    int status = student_sem_wait(&queue->slots);
    if (status != 0) {
        return status;
    }

    status = student_spin_lock(&queue->guard);
    if (status != 0) {
        /* restore the slot because the item was not enqueued */
        student_sem_post(&queue->slots);
        return status;
    }

    queue->items[queue->tail] = job;
    queue->tail = (queue->tail + 1U) % queue->capacity;
    queue->count++;

    student_spin_unlock(&queue->guard);

    status = student_sem_post(&queue->items_available);
    return status;
}

int work_queue_take(WorkQueue *queue, int *job)
{
    if (queue == NULL || job == NULL || !queue->initialized) {
        return EINVAL;
    }

    int status = student_sem_wait(&queue->items_available);
    if (status != 0) {
        return status;
    }

    status = student_spin_lock(&queue->guard);
    if (status != 0) {
        /* restore the item availability because we did not take one */
        student_sem_post(&queue->items_available);
        return status;
    }

    *job = queue->items[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count--;

    student_spin_unlock(&queue->guard);

    status = student_sem_post(&queue->slots);
    return status;
}

int work_queue_destroy(WorkQueue *queue)
{
    if (queue == NULL || !queue->initialized) {
        return EINVAL;
    }

    int status = student_sem_destroy(&queue->items_available);
    if (status != 0) {
        return status;
    }

    status = student_sem_destroy(&queue->slots);
    if (status != 0) {
        return status;
    }

    free(queue->items);
    queue->items = NULL;
    queue->capacity = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->initialized = false;
    return 0;
}
