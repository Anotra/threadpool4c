#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>

typedef struct threadpool ThreadPool;
typedef struct threadpool_task ThreadPoolTask;
typedef void ThreadPoolRunnable(ThreadPoolTask *task, void *data);
typedef void ThreadPoolCleanup(ThreadPoolTask *task, void *data);

typedef struct threadpool_info {
  size_t thread_count;
  size_t thread_min;
  size_t thread_max;
  size_t idle_count;
  size_t active_count;
  size_t idle_timeout_seconds;
  bool is_shutdown;
} ThreadPoolInfo;

typedef struct threadpool_task_info {
  ThreadPool *pool;
  int64_t id;
  struct timespec time_created;
  struct timespec time_started;
  struct timespec time_finished;
  bool finished:1;
  bool cancelled:1;
  bool user_has_pointer:1;
  bool destroy_when_done:1;
} ThreadPoolTaskInfo;

/** Create Thread Pool
 * @param min minimum number of threads
 * @param max maximum number of threads
 * This will block until the entire pool is initialized
 * @return ThreadPool or NULL
 */
extern ThreadPool *
threadpool_create(size_t min, size_t max);

/** Shut Down Thread Pool 
 * Finishes or cancels all remaining task and blocks until completion
 * @param pool ThreadPool to shut down
 * @param cancelRemaining cancel all remaining tasks on thread pool
 */
extern void
threadpool_shutdown(ThreadPool *pool, bool cancel_remaining);

/** Destroy Thread Pool
 * Ensure all tasks are destroyed before calling this
 * @param pool ThreadPool to destroy
 */
extern void
threadpool_destroy(ThreadPool *pool);

/** Execute On Thread Pool
 * @param pool ThreadPool to execute on
 * @param runnable void run(ThreadPoolTask *task, void *data)
 * @param cleanup pass NULL or void cleanup(ThreadPoolTask *task, void *data)
 * @param data data passed to run and cleanup function
 * @param task pass NULL or &task and you're required to call threadpool_destroy_task_when_done later
 * @see threadpool_destroy_task_when_done()
 * @return task id or (0 if failed to add task to queue)
 */
extern uint64_t
threadpool_execute(
  ThreadPool *pool,
  ThreadPoolRunnable *runnable,
  ThreadPoolCleanup *cleanup,
  void *data,
  ThreadPoolTask **task
);

/** Destroy Task
 * Call this when you're done using a ThreadPoolTask 
 * This will be destroyed automatically after cleanup unless 
 * you assign it to a pointer with threadpool_execute
 * @param task Task to destroy
 * @param cancel cancel the task if it's not been executed
 */
extern void
threadpool_destroy_task_when_done(ThreadPoolTask *task, bool cancel);

/**
 * calls threadpool_destroy_task_when_done
 */
static inline void
threadpool_destroy_task(ThreadPoolTask *task, bool cancel) {
  threadpool_destroy_task_when_done(task, cancel);
}

/** Get info about a Thread Pool
 * @param pool ThreadPool to get info about
 * @param info Where to copy info into
 */
extern void
threadpool_info(ThreadPool *pool, ThreadPoolInfo *info);

/** Get info about a Thread Pool Task
 * @param task ThreadPoolTask to get info about
 * @param info Where to copy info into
 */
extern void
threadpool_task_info(ThreadPoolTask *task, ThreadPoolTaskInfo *info);

#endif // !THREADPOOL_H