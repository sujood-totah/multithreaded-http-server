#include "threadpool.h"
#include <stdlib.h>
#include <string.h>

/**
 * do_work - thread routine: wait for work, dequeue, run routine.
 * PDF: 1) if destruction begun exit 2) if queue empty wait 3) check destruction again
 *      4) take first from queue 5) if queue empty and destroy waiting, signal
 *      6) if destruction not begun, signal dispatch (free space) 7) call routine
 */
void *do_work(void *p)
{
    threadpool *pool = (threadpool *)p;

    for (;;) {
        pthread_mutex_lock(&pool->qlock);

        /* 1. If destruction has begun, exit */
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->qlock);
            return NULL;
        }

        /* 2. If queue is empty, wait */
        while (pool->qsize == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->q_not_empty, &pool->qlock);
        }

        /* 3. Check destruction again */
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->qlock);
            return NULL;
        }

        /* 4. Take first element from queue */
        work_t *work = pool->qhead;
        if (!work) {
            pthread_mutex_unlock(&pool->qlock);
            continue;
        }

        pool->qhead = work->next;
        if (pool->qhead == NULL)
            pool->qtail = NULL;
        pool->qsize--;

        /* 5. If queue became empty and destruction waits to begin, signal */
        if (pool->qsize == 0 && pool->dont_accept)
            pthread_cond_signal(&pool->q_empty);

        /* 6. If destruction hasn't begun, signal dispatch (free space) */
        if (!pool->shutdown)
            pthread_cond_signal(&pool->q_not_full);

        pthread_mutex_unlock(&pool->qlock);

        /* 7. Call the thread routine (outside lock) */
        if (work->routine)
            (void)work->routine(work->arg);
        free(work);
    }
    return NULL;
}

/**
 * create_threadpool - create pool and worker threads.
 * PDF: sanity check, init structure, init mutex/cond, create threads with do_work.
 */
threadpool *create_threadpool(int num_threads_in_pool, int max_queue_size)
{
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL)
        return NULL;
    if (max_queue_size <= 0 || max_queue_size > MAXW_IN_QUEUE)
        return NULL;

    threadpool *tp = (threadpool *)malloc(sizeof(threadpool));
    if (!tp)
        return NULL;

    tp->num_threads = num_threads_in_pool;
    tp->max_qsize = max_queue_size;
    tp->qsize = 0;
    tp->qhead = tp->qtail = NULL;
    tp->shutdown = 0;
    tp->dont_accept = 0;

    if (pthread_mutex_init(&tp->qlock, NULL) != 0) {
        free(tp);
        return NULL;
    }
    if (pthread_cond_init(&tp->q_not_empty, NULL) != 0) {
        pthread_mutex_destroy(&tp->qlock);
        free(tp);
        return NULL;
    }
    if (pthread_cond_init(&tp->q_empty, NULL) != 0) {
        pthread_cond_destroy(&tp->q_not_empty);
        pthread_mutex_destroy(&tp->qlock);
        free(tp);
        return NULL;
    }
    if (pthread_cond_init(&tp->q_not_full, NULL) != 0) {
        pthread_cond_destroy(&tp->q_empty);
        pthread_cond_destroy(&tp->q_not_empty);
        pthread_mutex_destroy(&tp->qlock);
        free(tp);
        return NULL;
    }

    tp->threads = (pthread_t *)malloc((size_t)num_threads_in_pool * sizeof(pthread_t));
    if (!tp->threads) {
        pthread_cond_destroy(&tp->q_not_full);
        pthread_cond_destroy(&tp->q_empty);
        pthread_cond_destroy(&tp->q_not_empty);
        pthread_mutex_destroy(&tp->qlock);
        free(tp);
        return NULL;
    }

    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&tp->threads[i], NULL, do_work, tp) != 0) {
            tp->shutdown = 1;
            for (int j = 0; j < i; j++)
                pthread_join(tp->threads[j], NULL);
            free(tp->threads);
            pthread_cond_destroy(&tp->q_not_full);
            pthread_cond_destroy(&tp->q_empty);
            pthread_cond_destroy(&tp->q_not_empty);
            pthread_mutex_destroy(&tp->qlock);
            free(tp);
            return NULL;
        }
    }

    return tp;
}

/**
 * dispatch - enqueue a job.
 * PDF: 1) if destroy begun don't accept 2) create work_t 3) if full wait 4) add to queue 5) signal not empty
 */
void dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg)
{
    if (!from_me || !dispatch_to_here)
        return;

    pthread_mutex_lock(&from_me->qlock);

    /* 1. If destroy has begun or shutdown, run job in caller thread to avoid arg/fd leak */
    if (from_me->dont_accept || from_me->shutdown) {
        pthread_mutex_unlock(&from_me->qlock);
        (void)dispatch_to_here(arg);
        return;
    }

    work_t *work = (work_t *)malloc(sizeof(work_t));
    if (!work) {
        pthread_mutex_unlock(&from_me->qlock);
        (void)dispatch_to_here(arg);   /* run so arg/fd freed, no leak */
        return;
    }
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    /* 3. If queue is full, wait (and wake on destroy) */
    while (from_me->qsize >= from_me->max_qsize && !from_me->dont_accept && !from_me->shutdown)
        pthread_cond_wait(&from_me->q_not_full, &from_me->qlock);

    if (from_me->dont_accept || from_me->shutdown) {
        pthread_mutex_unlock(&from_me->qlock);
        free(work);
        (void)dispatch_to_here(arg);
        return;
    }

    /* 4. Add to queue */
    if (from_me->qtail)
        from_me->qtail->next = work;
    else
        from_me->qhead = work;
    from_me->qtail = work;
    from_me->qsize++;

    /* 5. Signal queue not empty */
    pthread_cond_signal(&from_me->q_not_empty);
    pthread_mutex_unlock(&from_me->qlock);
}

/**
 * destroy_threadpool - PDF: 1) dont_accept=1 2) wait queue empty 3) shutdown=1 4) signal waiters 5) join 6) free
 */
void destroy_threadpool(threadpool *destroyme)
{
    if (!destroyme)
        return;

    pthread_mutex_lock(&destroyme->qlock);

    /* 1. Don't accept new work */
    destroyme->dont_accept = 1;

    /* 2. Wait for queue to become empty */
    while (destroyme->qsize != 0)
        pthread_cond_wait(&destroyme->q_empty, &destroyme->qlock);

    /* 3. Set shutdown so threads exit */
    destroyme->shutdown = 1;

    /* 4. Wake threads waiting on empty queue and any dispatch waiting on full */
    pthread_cond_broadcast(&destroyme->q_not_empty);
    pthread_cond_broadcast(&destroyme->q_not_full);

    pthread_mutex_unlock(&destroyme->qlock);

    /* 5. Join all threads */
    for (int i = 0; i < destroyme->num_threads; i++)
        pthread_join(destroyme->threads[i], NULL);

    /* 6. Free resources */
    pthread_mutex_destroy(&destroyme->qlock);
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_empty);
    pthread_cond_destroy(&destroyme->q_not_full);
    free(destroyme->threads);
    free(destroyme);
}
