#define _XOPEN_SOURCE 700

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
  ThreadPoolInfo public;
};

struct threadpool_task {
  ThreadPoolRunnable *runnable;
  ThreadPoolCleanup *cleanup;
  void *data;
  void *next;
  ThreadPoolTaskInfo public;
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
  if (!pool->public.is_shutdown) {
    while (true) {
      if (pool->tasks) {
        pool->public.active_count++;
        ThreadPoolTask *tail = pool->tasks;
        ThreadPoolTask *task = tail->next;
        if (tail == task) {
          pool->tasks = NULL;
        } else {
          tail->next = task->next;
        }
        clock_gettime(CLOCK_REALTIME, &task->public.time_started);
        if (!task->public.cancelled && !pool->cancel_remaining) {
          pthread_mutex_unlock(&pool->lock);
          task->runnable(task, task->data);
          pthread_mutex_lock(&pool->lock);
        } else {
          task->public.cancelled = true;
        }
        clock_gettime(CLOCK_REALTIME, &task->public.time_finished);
        if (task->cleanup) {
          pthread_mutex_unlock(&pool->lock);
          task->cleanup(task, task->data);
          pthread_mutex_lock(&pool->lock);
        }
        task->public.finished = true;
        if (task->public.destroy_on_completion) {
          task_free(task);
        }
        pool->public.active_count--;
      } else if (pool->public.is_shutdown) {
          break;
      } else {
        pool->public.idle_count++;
        if (pool->waiting_count + pool->public.active_count >= pool->public.thread_min) {
          struct timespec ts;
          clock_gettime(CLOCK_REALTIME, &ts);
          ts.tv_sec += pool->public.idle_timeout_seconds;
          pool->waiting_to_die_count++;
          pthread_cond_timedwait(&pool->cond_inactive, &pool->lock, &ts);
          pool->waiting_to_die_count--;
          if (!pool->tasks) {
            pool->public.idle_count--;
            break;
          }
        } else {
          pool->waiting_count++;
          pthread_cond_wait(&pool->cond_active, &pool->lock);
          pool->waiting_count--;
        }
        pool->public.idle_count--;
      }
      
    }
  }
  pool->public.thread_count--;
  if (pool->public.is_shutdown) {
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
    pool->public.thread_count++;
    pthread_mutex_unlock(&pool->lock);
    return true;
  }
  return false;
}

ThreadPool *
threadpool_create(size_t min, size_t max) {
  ThreadPool *pool = calloc(1, sizeof *pool);
  if (pool) {
    pool->public.thread_min = min;
    pool->public.thread_max = max;
    pool->public.idle_timeout_seconds = 60;
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
                    pool->public.is_shutdown = true;
                    while (pool->public.thread_count) {
                      pthread_cond_wait(&pool->cond_thread_new_or_die, &pool->lock);
                    }
                    break;
                  }
                }
                if (!pool->public.is_shutdown) {
                  pthread_mutexattr_destroy(&mtxattr);
                  while (pool->public.idle_count != min) {
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
  const bool is_already_shutdown = pool->public.is_shutdown;
  pool->cancel_remaining |= cancel_remaining;
  pool->public.is_shutdown = true;
  pthread_cond_broadcast(&pool->cond_active);
  pthread_cond_broadcast(&pool->cond_inactive);
  while (pool->public.thread_count) {
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

  pthread_cond_destroy(&pool->cond_active);
  pthread_cond_destroy(&pool->cond_inactive);
  pthread_cond_destroy(&pool->cond_thread_new_or_die);
  pthread_attr_destroy(&pool->thread_attributes);
  pthread_mutex_destroy(&pool->lock);
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
    task->public.pool = pool;
    task->runnable = runnable;
    task->cleanup = cleanup;
    task->data = data;
    task->next = task;
    task->public.user_has_pointer = !!taskret;
    task->public.destroy_on_completion = !taskret;
    clock_gettime(CLOCK_REALTIME, &task->public.time_created);
    pthread_mutex_lock(&pool->lock);
    if (pool->public.is_shutdown) {
      pthread_mutex_unlock(&pool->lock);
      if (taskret)
        *taskret = NULL;
      free(task);
      return false;
    }
    task->public.id = pool->id_counter++;
    if (pool->tasks) {
      task->next = pool->tasks->next;
      pool->tasks->next = task;
    }
    pool->tasks = task;
    if (pool->waiting_count) {
      pthread_cond_signal(&pool->cond_active);
    } else if (pool->waiting_to_die_count) {
      pthread_cond_signal(&pool->cond_inactive);
    } else if (pool->public.thread_count < pool->public.thread_max) {
      thread_new(pool);
    }
    pthread_mutex_unlock(&pool->lock);
    return true;
  }
  return false;
}

void
threadpool_destroy_task_on_completion(ThreadPoolTask *task, bool cancel) {
  pthread_mutex_t *mutex = &task->public.pool->lock;
  pthread_mutex_lock(mutex);
  if (task->public.finished) {
    task_free(task);
  } else {
    task->public.cancelled = cancel;
    task->public.destroy_on_completion = true;
  }
  pthread_mutex_unlock(mutex);
}

void
threadpool_info(ThreadPool *pool, ThreadPoolInfo *info) {
  pthread_mutex_lock(&pool->lock);
  memcpy(info, &pool->public, sizeof *info);
  pthread_mutex_unlock(&pool->lock);
}

void
threadpool_task_info(ThreadPoolTask *task, ThreadPoolTaskInfo *info) {
  pthread_mutex_lock(&task->public.pool->lock);
  memcpy(info, &task->public, sizeof *info);
  pthread_mutex_unlock(&task->public.pool->lock);
}
