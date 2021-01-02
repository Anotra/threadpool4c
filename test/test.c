#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "threadpool.h"

static void
run(ThreadPoolTask *task, void *data) {
  if (rand() % 50 == 0)
    sleep(1);
  printf("Run %s\n", (char *) data);
}

static void
clean(ThreadPoolTask *task, void *data) {
  ThreadPoolTaskInfo info;
  threadpool_task_info(task, &info);
  time_t runtime = (info.time_finished.tv_sec - info.time_started.tv_sec) * 1000000000 + info.time_finished.tv_nsec - info.time_started.tv_nsec;
  printf("Clean: Runtime:%lu, Task:%s Cancelled:%s\n", runtime, (char *) data, info.cancelled ? "true" : "false");
  free(data);
}

static void *
thread2_run(void *threadpool) {
  sleep(2);
  threadpool_shutdown(threadpool, true);
  printf("THREAD 2 DONE!\n");
  return NULL;
}

static struct {
  int count;
  pthread_mutex_t mtx;
} test_incr_data;

static void
incr_test(ThreadPoolTask *task, void *incr) {
  pthread_mutex_lock(&test_incr_data.mtx);
  test_incr_data.count += *(int *)incr;
  pthread_mutex_unlock(&test_incr_data.mtx);
  free(incr);
}

int
main() {
  ThreadPool *pool = threadpool_create(1, 50);
  if (pool) {
    for (int i=0; i<10000; i++) {
      char *buf = calloc(64, sizeof *buf);
      if (buf) {
        snprintf(buf, 64, "%i!", i);
        ThreadPoolTask *task;
        if (!threadpool_execute(pool, run, clean, buf, &task)) {
          free(buf);
        } else {
          printf("task: %p\n", task);
          threadpool_destroy_task_on_completion(task, false);
        }
      }
    }
    ThreadPoolInfo info;
    threadpool_info(pool, &info);
    pthread_t thread2;
    pthread_create(&thread2, NULL, thread2_run, pool);
    sleep(1);
    threadpool_shutdown(pool, false);
    ThreadPoolInfo info2;
    threadpool_info(pool, &info2);
    printf("---before shutdown---\nmin:%lu\nmax:%lu\ncount:%lu\nactive:%lu\nidle:%lu\n",
       info.thread_min, info.thread_max, info.thread_count, info.active_count, info.idle_count);
    printf("---after shutdown---\nmin:%lu\nmax:%lu\ncount:%lu\nactive:%lu\nidle:%lu\n",
       info2.thread_min, info2.thread_max, info2.thread_count, info2.active_count, info2.idle_count);
    threadpool_destroy(pool);
    pthread_join(thread2, NULL);


    printf("Running Increment Test\n");
    pool = threadpool_create(10, 50);
    pthread_mutex_init(&test_incr_data.mtx, NULL);
    const int incr_test_count = 100000;
    int incr_test_result = 0;
    for (int i=0; i<incr_test_count; i++) {
      int *val = malloc(sizeof *val);
      *val = i;
      incr_test_result += i;
      threadpool_execute(pool, incr_test, NULL, val, NULL);
    }
    threadpool_shutdown(pool, false);
    pthread_mutex_destroy(&test_incr_data.mtx);
    printf("Increment test %i: %s\n", incr_test_count, test_incr_data.count == incr_test_result ? "Succeeded" : "Failed");
    threadpool_destroy(pool);

  }
}
