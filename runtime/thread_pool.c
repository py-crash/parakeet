#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include "thread_pool.h"

static inline unsigned long long get_cpu_time();

typedef struct {
  int               id;
  worker_data_t    *worker_data;
} worker_args_t;

static void *worker(void *args) {
  worker_args_t *my_args = (worker_args_t*)args;

  int id = my_args->id;
  worker_data_t *worker_data = my_args->worker_data;
  free(args);

  // Do work forever
  for (;;) {
    thread_status_t status = worker_data->status;
    if (status == THREAD_RUN) {
      task_list_t *task_list = worker_data->task_list;

      // Check whether we're done.
      if (task_list->cur_task == task_list->num_tasks ||
          (worker_data->fixed_num_iters > 0 &&
           worker_data->fixed_num_iters == worker_data->iters_done)) {
        pthread_mutex_lock(&worker_data->mutex);
        worker_data->status = THREAD_FINISHED;
        if (worker_data->notify_when_done) {
          pthread_cond_signal(worker_data->master_cond);
        }
        pthread_mutex_unlock(&worker_data->mutex);
      } else {
        // We know now that we have an iteration to perform for this task, so
        // do it.
        task_t *task = &task_list->tasks[task_list->cur_task];
        (*worker_data->work_function)(task->next_iteration,
                                      task->next_iteration,
                                      worker_data->args,
                                      worker_data->tile_sizes);
        task->next_iteration++;
        worker_data->iters_done++;
        worker_data->timestamp = get_cpu_time();

        // If this was the last iteration of this task, move to the next one.
        if (task->next_iteration > task->last_iteration) {
          task_list->cur_task++;
        }
      }
    } else if (status == THREAD_STOP) {
      break;
    } else {
      pthread_mutex_lock(&worker_data->mutex);
      if (status == THREAD_PAUSE) {
        pthread_barrier_wait(worker_data->task_list->barrier);
      }
      pthread_cond_wait(&worker_data->cond, &worker_data->mutex);
      pthread_mutex_unlock(&worker_data->mutex);
    }
  }

  pthread_exit(NULL);
  return NULL;
}

thread_pool_t *create_thread_pool(int max_threads) {
  int i, rc;

  thread_pool_t *thread_pool = (thread_pool_t*)malloc(sizeof(thread_pool_t));

  thread_pool->workers = (pthread_t*)malloc(max_threads*sizeof(pthread_t));
  thread_pool->num_workers = max_threads;
  thread_pool->num_active = 0;
  pthread_cond_init(&thread_pool->master_cond, NULL);
  thread_pool->worker_data =
    (worker_data_t*)malloc(sizeof(worker_data_t)*max_threads);
  thread_pool->iters_done = (int*)malloc(sizeof(int) * max_threads);
  thread_pool->timestamps =
    (unsigned long long*)malloc(sizeof(unsigned long long) * max_threads);
  thread_pool->job = NULL;

  cpu_set_t cpu_set;
  pthread_attr_t attr;
  pthread_attr_init(&attr);

  for (i = 0; i < max_threads; ++i) {
    thread_pool->worker_data[i].task_list = NULL;
    pthread_mutex_init(&thread_pool->worker_data[i].mutex, NULL);
    pthread_cond_init(&thread_pool->worker_data[i].cond, NULL);
    thread_pool->worker_data[i].status = THREAD_IDLE;
    thread_pool->worker_data[i].master_cond =
      &thread_pool->master_cond;
    thread_pool->iters_done[i] = 0;
    thread_pool->timestamps[i] = 0;
    worker_args_t *args = (worker_args_t*)malloc(sizeof(worker_args_t));
    args->id = i;
    args->worker_data = &thread_pool->worker_data[i];
    CPU_ZERO(&cpu_set);
    CPU_SET(i, &cpu_set);
    pthread_attr_setaffinity_np(&attr, 8, &cpu_set);
    rc = pthread_create(&thread_pool->workers[i], &attr, worker, (void*)args);
    if (rc) {
      printf("Couldn't create worker %d (Error code %d). Exiting.\n", i, rc);
      exit(-1);
    }
  }

  return thread_pool;
}

// This function should only ever be called when all of the threads are paused.
void launch_job(thread_pool_t *thread_pool,
                work_function_t work_function, void *args, job_t *job,
                int *tile_sizes, int fixed_num_iters) {
  assert(job->num_lists <= thread_pool->num_workers);

  thread_pool->job = job;
  thread_pool->num_active = job->num_lists;

  // Update the threads' data with the current batch of work and parallelism
  // configuration.
  int i;
  for (i = 0; i < thread_pool->num_active; ++i) {
    pthread_mutex_lock(&thread_pool->worker_data[i].mutex);
    if (job->task_lists[i].num_tasks > 0) {
      thread_pool->worker_data[i].status = THREAD_RUN;
      thread_pool->worker_data[i].task_list = &job->task_lists[i];
    } else {
      thread_pool->worker_data[i].status = THREAD_FINISHED;
      thread_pool->worker_data[i].task_list = NULL;
    }
    thread_pool->worker_data[i].notify_when_done = 0;
    thread_pool->worker_data[i].work_function = work_function;
    thread_pool->worker_data[i].args = args;
    thread_pool->worker_data[i].tile_sizes = tile_sizes;
    thread_pool->worker_data[i].fixed_num_iters = fixed_num_iters;
    thread_pool->worker_data[i].iters_done = 0;
    thread_pool->worker_data[i].timestamp =
      thread_pool->timestamps[i] = get_cpu_time();
    pthread_cond_signal(&thread_pool->worker_data[i].cond);
    pthread_mutex_unlock(&thread_pool->worker_data[i].mutex);
    thread_pool->iters_done[i] = 0;
  }
  for (i = thread_pool->num_active; i < thread_pool->num_workers; ++i) {
    pthread_mutex_lock(&thread_pool->worker_data[i].mutex);
    thread_pool->worker_data[i].status = THREAD_IDLE;
    thread_pool->worker_data[i].task_list = NULL;
    thread_pool->worker_data[i].notify_when_done = 0;
    thread_pool->worker_data[i].work_function = NULL;
    thread_pool->worker_data[i].args = NULL;
    thread_pool->worker_data[i].tile_sizes = NULL;
    thread_pool->worker_data[i].fixed_num_iters = 0;
    thread_pool->worker_data[i].iters_done = 0;
    thread_pool->worker_data[i].timestamp = 0;
    pthread_mutex_unlock(&thread_pool->worker_data[i].mutex);
  }
}

void pause_job(thread_pool_t *thread_pool) {
  int i;
  for (i = 0; i < thread_pool->num_active; ++i) {
    pthread_mutex_lock(&thread_pool->worker_data[i].mutex);
    thread_pool->worker_data[i].status = THREAD_PAUSE;
    pthread_cond_signal(&thread_pool->worker_data[i].cond);
    pthread_mutex_unlock(&thread_pool->worker_data[i].mutex);
  }
  pthread_barrier_wait(&thread_pool->job->barrier);

  thread_pool->num_active = 0;
}

int job_finished(thread_pool_t *thread_pool) {
  int all_done = 1;
  int i;
  for (i = 0; i < thread_pool->num_active && all_done; ++i) {
    pthread_mutex_lock(&thread_pool->worker_data[i].mutex);
    all_done =
      all_done && THREAD_FINISHED == thread_pool->worker_data[i].status;
    pthread_mutex_unlock(&thread_pool->worker_data[i].mutex);
  }
  return all_done;
}

double get_throughput(thread_pool_t *thread_pool) {
  double tp = 0.0;
  unsigned long long timestamp;
  int i, iters;
  for (i = 0; i < thread_pool->num_active; ++i) {
    iters = thread_pool->worker_data[i].iters_done;
    timestamp = thread_pool->worker_data[i].timestamp;
    tp += ((double)(iters - thread_pool->iters_done[i]) /
           (double)(timestamp - thread_pool->timestamps[i]));
    thread_pool->iters_done[i] = iters;
    thread_pool->timestamps[i] = timestamp;
  }

  return tp;
}

void wait_for_job(thread_pool_t *thread_pool) {
  int i;
  for (i = 0; i < thread_pool->num_active; ++i) {
    pthread_mutex_lock(&thread_pool->worker_data[i].mutex);
    if (THREAD_FINISHED != thread_pool->worker_data[i].status) {
      thread_pool->worker_data[i].notify_when_done = 1;
      pthread_cond_wait(&thread_pool->master_cond,
                        &thread_pool->worker_data[i].mutex);
    }
    pthread_mutex_unlock(&thread_pool->worker_data[i].mutex);
  }
}

job_t *get_job(thread_pool_t *thread_pool) {
  return thread_pool->job;
}

void destroy_thread_pool(thread_pool_t *thread_pool) {
  int i;
  for (i = 0; i < thread_pool->num_workers; ++i) {
    pthread_mutex_lock(&thread_pool->worker_data[i].mutex);
    thread_pool->worker_data[i].status = THREAD_STOP;
    pthread_cond_signal(&thread_pool->worker_data[i].cond);
    pthread_mutex_unlock(&thread_pool->worker_data[i].mutex);
    pthread_join(thread_pool->workers[i], NULL);
    pthread_mutex_destroy(&thread_pool->worker_data[i].mutex);
    pthread_cond_destroy(&thread_pool->worker_data[i].cond);
  }

  pthread_cond_destroy(&thread_pool->master_cond);
  free(thread_pool->worker_data);
  free(thread_pool->iters_done);
  free(thread_pool->workers);
  free(thread_pool);
}

static inline unsigned long long get_cpu_time() {
  unsigned hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo)|(((unsigned long long)hi)<<32);
}