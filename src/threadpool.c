#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif

#include <pthread.h>
#include <string.h>

#include "threadpool.h"

struct joinable_thread {
  ThreadPool *pool;
  struct joinable_thread *next;
  pthread_t thread;
};

struct threadpool {
  pthread_attr_t thread_attributes;
  pthread_mutex_t lock;
  pthread_cond_t cond_active;
  pthread_cond_t cond_inactive;
  pthread_cond_t cond_thread_new_or_die;
  size_t waiting_count;
  size_t waiting_to_die_count;
  ThreadPoolTask *tasks;
  struct joinable_thread *joinable_threads;
  int64_t id_counter;
  bool cancel_remaining;
  ThreadPoolInfo info;
};

struct threadpool_task {
  ThreadPoolRunnable *runnable;
  ThreadPoolCleanup *cleanup;
  void *data;
  void *next;
  ThreadPoolTaskInfo info;
};

static inline void
task_free(ThreadPoolTask *task) {
  free(task);
}

static void *
thread_run(void *arg) {
  struct joinable_thread *joinable_thread = arg;
  ThreadPool *pool = joinable_thread->pool;
  pthread_mutex_lock(&pool->lock);
  pthread_cond_broadcast(&pool->cond_thread_new_or_die);
  if (!pool->info.is_shutdown) {
    while (true) {
      if (pool->tasks) {
        pool->info.active_count++;
        ThreadPoolTask *tail = pool->tasks;
        ThreadPoolTask *task = tail->next;
        if (tail == task) {
          pool->tasks = NULL;
        } else {
          tail->next = task->next;
        }
        clock_gettime(CLOCK_REALTIME, &task->info.time_started);
        if (!task->info.cancelled && !pool->cancel_remaining) {
          pthread_mutex_unlock(&pool->lock);
          task->runnable(task, task->data);
          pthread_mutex_lock(&pool->lock);
        } else {
          task->info.cancelled = true;
        }
        clock_gettime(CLOCK_REALTIME, &task->info.time_finished);
        task->info.finished = true;
        if (task->cleanup) {
          task->cleanup(task, task->data);
        }
        if (task->info.destroy_on_completion) {
          task_free(task);
        }
        pool->info.active_count--;
      } else {
        if (pool->info.is_shutdown) {
          break;
        } else {
          pool->info.idle_count++;
          if (pool->waiting_count + pool->info.active_count >= pool->info.thread_min) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += pool->info.idle_timeout_seconds;
            pool->waiting_to_die_count++;
            pthread_cond_timedwait(&pool->cond_inactive, &pool->lock, &ts);
            pool->waiting_to_die_count--;
            if (!pool->tasks) {
              pool->info.idle_count--;
              break;
            }
          } else {
            pool->waiting_count++;
            pthread_cond_wait(&pool->cond_active, &pool->lock);
            pool->waiting_count--;
          }
          pool->info.idle_count--;
        }
      }
    }
  }
  pool->info.thread_count--;
  if (pool->info.is_shutdown) {
    joinable_thread->next = pool->joinable_threads;
    pool->joinable_threads = joinable_thread;
  } else {
    pthread_detach(joinable_thread->thread);
    free(joinable_thread);
  }
  pthread_cond_broadcast(&pool->cond_thread_new_or_die);
  pthread_mutex_unlock(&pool->lock);
  return NULL;
}

static bool
thread_new(ThreadPool *pool) {
  struct joinable_thread *joinable_thread = calloc(1, sizeof *joinable_thread);
  if (joinable_thread) {
    joinable_thread->pool = pool;
    pthread_mutex_lock(&pool->lock);
    int result = pthread_create(&joinable_thread->thread, &pool->thread_attributes, thread_run, joinable_thread);
    if (result != 0) {
      pthread_mutex_unlock(&pool->lock);
      free(joinable_thread);
      return false;
    }
    pool->info.thread_count++;
    pthread_mutex_unlock(&pool->lock);
    return true;
  }
  return false;
}

ThreadPool *
threadpool_create(size_t min, size_t max) {
  ThreadPool *pool = calloc(1, sizeof *pool);
  if (pool) {
    pool->info.thread_min = min;
    pool->info.thread_max = max;
    pool->info.idle_timeout_seconds = 60;
    pthread_mutexattr_t mtxattr;
    if (pthread_mutexattr_init(&mtxattr) == 0) {
      pthread_mutexattr_settype(&mtxattr, PTHREAD_MUTEX_RECURSIVE);
      if (pthread_mutex_init(&pool->lock, &mtxattr) == 0) {
        pthread_mutex_lock(&pool->lock);
        if (pthread_cond_init(&pool->cond_active, NULL) == 0) {
          if (pthread_cond_init(&pool->cond_inactive, NULL) == 0) {
            if (pthread_cond_init(&pool->cond_thread_new_or_die, NULL) == 0) {
              if (pthread_attr_init(&pool->thread_attributes) == 0) {
		pthread_attr_setstacksize(&pool->thread_attributes, 1024 * 1024 * 2);
                for (int i = 0; i < min; i++) {
                  if(!thread_new(pool)) {
                    pool->info.is_shutdown = true;
                    while (pool->info.thread_count) {
                      pthread_cond_wait(&pool->cond_thread_new_or_die, &pool->lock);
                    }
                    break;
                  }
                }
                if (!pool->info.is_shutdown) {
                  pthread_mutexattr_destroy(&mtxattr);
                  while (pool->info.idle_count != min) {
                    pthread_cond_wait(&pool->cond_thread_new_or_die, &pool->lock);
                  }
                  pthread_mutex_unlock(&pool->lock);
                  return pool;
                }
                pthread_attr_destroy(&pool->thread_attributes);
              }
              pthread_cond_destroy(&pool->cond_thread_new_or_die);
            }
            pthread_cond_destroy(&pool->cond_inactive);
          }
          pthread_cond_destroy(&pool->cond_active);
        }
        pthread_mutex_unlock(&pool->lock);
        pthread_mutex_destroy(&pool->lock);
      }
      pthread_mutexattr_destroy(&mtxattr);
    }
    free(pool);
  }
  return NULL;
}

void
threadpool_shutdown(ThreadPool *pool, bool cancel_remaining) {
  pthread_mutex_lock(&pool->lock);
  const bool is_already_shutdown = pool->info.is_shutdown;
  if (!is_already_shutdown) {
    pool->cancel_remaining = cancel_remaining;
    pool->info.is_shutdown = true;
  }
  pthread_cond_broadcast(&pool->cond_active);
  pthread_cond_broadcast(&pool->cond_inactive);
  while (pool->info.thread_count) {
    pthread_cond_wait(&pool->cond_thread_new_or_die, &pool->lock);
  }
  if (!is_already_shutdown) {
    for (struct joinable_thread *p = pool->joinable_threads; p;) {
      struct joinable_thread *next = p->next;
      pthread_join(p->thread, NULL);
      free(p);
      p = next;
    }
  }
  pthread_mutex_unlock(&pool->lock);
}

void
threadpool_destroy(ThreadPool *pool) {
  threadpool_shutdown(pool, false);
  pthread_mutex_lock(&pool->lock);
  pthread_cond_destroy(&pool->cond_active);
  pthread_cond_destroy(&pool->cond_inactive);
  pthread_cond_destroy(&pool->cond_thread_new_or_die);
  pthread_attr_destroy(&pool->thread_attributes);
  pthread_mutex_unlock(&pool->lock);
  free(pool);
}

bool
threadpool_execute(
  ThreadPool *pool,
  ThreadPoolRunnable *runnable,
  ThreadPoolCleanup *cleanup,
  void *data,
  ThreadPoolTask **taskret
) {
  ThreadPoolTask *task = calloc(1, sizeof *task);
  if (taskret)
    *taskret = task;
  if (task) {
    task->info.pool = pool;
    task->runnable = runnable;
    task->cleanup = cleanup;
    task->data = data;
    task->next = task;
    task->info.user_has_pointer = !!taskret;
    task->info.destroy_on_completion = !taskret;
    clock_gettime(CLOCK_REALTIME, &task->info.time_created);
    pthread_mutex_lock(&pool->lock);
    if (pool->info.is_shutdown) {
      pthread_mutex_unlock(&pool->lock);
      if (taskret)
        *taskret = NULL;
      free(task);
      return false;
    }
    task->info.id = pool->id_counter++;
    if (pool->tasks) {
      task->next = pool->tasks->next;
      pool->tasks->next = task;
    }
    pool->tasks = task;
    if (pool->waiting_count) {
      pthread_cond_signal(&pool->cond_active);
    } else if (pool->waiting_to_die_count) {
      pthread_cond_signal(&pool->cond_inactive);
    } else if (pool->info.thread_count < pool->info.thread_max) {
      thread_new(pool);
    }
    pthread_mutex_unlock(&pool->lock);
    return true;
  }
  return false;
}

void
threadpool_destroy_task_on_completion(ThreadPoolTask *task, bool cancel) {
  pthread_mutex_t *mutex = &task->info.pool->lock;
  pthread_mutex_lock(mutex);
  if (task->info.finished) {
    task_free(task);
  } else {
    task->info.cancelled = cancel;
    task->info.destroy_on_completion = true;
  }
  pthread_mutex_unlock(mutex);
}

void
threadpool_info(ThreadPool *pool, ThreadPoolInfo *info) {
  pthread_mutex_lock(&pool->lock);
  memcpy(info, &pool->info, sizeof *info);
  pthread_mutex_unlock(&pool->lock);
}

void
threadpool_task_info(ThreadPoolTask *task, ThreadPoolTaskInfo *info) {
  pthread_mutex_lock(&task->info.pool->lock);
  memcpy(info, &task->info, sizeof *info);
  pthread_mutex_unlock(&task->info.pool->lock);
}
