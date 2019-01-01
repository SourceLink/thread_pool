#ifndef  _SL_THREAD_POOL_H
#define  _SL_THREAD_POOL_H

#define _GNU_SOURCE
#include <pthread.h>
#include "sl_list.h"


struct sl_task_queue {
    struct list_head task_head;
    unsigned int num_tasks_alive;
    pthread_mutex_t task_mutex;             /* 用于线程队列互斥锁 */
    pthread_cond_t  task_ready_signal;      /* 任务队列有任务处理 */
};


struct sl_thread_task {
    struct list_head task_list;
    void *(*task_fun)(void *arg);
    void *arg;
};

typedef enum {
    THREAD_IDLE      = 0,
    THREAD_SUPPEND   = 1,
    THREAD_RUNING    = 2,
    THREAD_WORKING   = 3,
    THREAD_QUIT      = 4
} type_thread_status;

typedef enum {
    EVENT_IDLE      = 0,
    EVENT_THREAD    = 1,
    EVENT_TASK      = 2,
    EVENT_SHUTDOWN  = 3
} type_event;

struct sl_thread {
    struct list_head thread_list;
    pthread_t   thread_id;
    unsigned int thread_status;             /* 线程状态 */
};

/*  
    一个线程池维护一个线程表和一个任务队列 
*/
struct sl_thread_pool {
    struct list_head thread_head;           /* 线程链表 */
    struct sl_task_queue task_queue;        /* 任务链表 */
    unsigned int core_threads_num;          /* 初始化需要创建的线程数 */
    unsigned int max_threads_num;           /* 创建线程最大上限数 */
    unsigned int alive_threads_num;         /* 当前创建线程数量 */
    pthread_mutex_t thread_mutex; 
    pthread_cond_t  thread_run_signal;      /* 线程run信号 */
    int keep_alive_time;                    /* 空闲线程保持存活时间 unit: ms */
    struct sl_thread *thread_manager;       /* 领导 */
    unsigned int destory;
};

/* 
    desc: 初始化线程池
    min_td_num: 初始化线程数
    max_td_num: 最大线程数目,线程数量是动态分配
    queue_size: 任务对列的数目
    return: 返回线程池句柄
*/
struct sl_thread_pool *sl_thread_pool_create(unsigned int min_td_num, unsigned int max_td_num, int alive_time);
void sl_thread_pool_destory(struct sl_thread_pool *pool);
void sl_thread_pool_destory_now(struct sl_thread_pool *pool);
int sl_thread_pool_push_task(struct sl_thread_pool *pool, void *(*task_fun)(void *arg), void *arg);

#endif