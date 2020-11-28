#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#include "threadpool.h"

static void run(ThreadPoolTask *task, void *data) {
  if (rand() % 50 == 0)
    sleep(1);
  printf("Run %s\n", (char *) data);
}

static void clean(ThreadPoolTask *task, void *data) {
  ThreadPoolTaskInfo info;
  threadpool_task_info(task, &info);
  time_t runtime = ((info.time_finished.tv_sec * 1000000000)  + info.time_finished.tv_nsec)
    - ((info.time_started.tv_sec * 1000000000) + info.time_started.tv_nsec);
  printf("Clean: Runtime:%lu, Task:%s Cancelled:%s\n", runtime, (char *) data, info.cancelled ? "true" : "false");
  free(data);
}

int main() {
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
    sleep(1);
    threadpool_shutdown(pool, true);
    ThreadPoolInfo info2;
    threadpool_info(pool, &info2);
    printf("---before shutdown---\nmin:%lu\nmax:%lu\ncount:%lu\nactive:%lu\nidle:%lu\n",
       info.thread_min, info.thread_max, info.thread_count, info.active_count, info.idle_count);
    printf("---after shutdown---\nmin:%lu\nmax:%lu\ncount:%lu\nactive:%lu\nidle:%lu\n",
       info2.thread_min, info2.thread_max, info2.thread_count, info2.active_count, info2.idle_count);
    threadpool_destroy(pool);
  }
}
