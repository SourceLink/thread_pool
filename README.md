---
layout: post
title:  "线程池的C实现"
date:   2019-01-02
catalog:  true
tags:
    - thread pool
---


# 一. 概述

相信大家一直有听过线程池, 但是不一定都知道这到底是个什么东西,是如何实现的; 

## 1.1 为什么要使用线程池?

- 因为线程的创建和销毁开销较大, 在单位时间内处理大量任务时再创建线程进行处理时间来不及.
- 可以控制线程数目, 保证不会过度消耗内存.

## 1.2 线程池适合应用的场合

当一个服务器接受到大量短小线程的请求时, 使用线程池技术是非常合适的, 它可以大大减少线程的创建和销毁次数, 提高服务器的工作效率. 但是线程要求的运行时间比较长, 则不适用.



# 二. 功能说明

## 2.1 线程池比喻

- Setup1: 一个医院，每天面对成千上万的病人，处理方式是：来一个病人找来一个医生处理，处理完了医生也走了。当看病时间较短的时候，医生来去的时间，显得尤为费时了

- Setup2: 医院引进了线程池的概念。设置门诊，把医生全派出去坐诊，病人来看病先挂号排队，医生根据病人队列顺序依次处理各个病人，这样就省去医生来来去去的时间了。但是，很多时候病人不多，医生却很多导致很多医生空闲浪费水电资源撒

- Setup3: 医院引进了可伸缩性线程池的概念，如阶段二，但是门诊一开始只派出了部分医生，但是增加了一个领导，病人依旧是排队看病，领导负责协调整个医院的医生。当病人很多医生忙不过来的时候，领导就去多叫几个医生来帮忙；当病人不多医生太多的时候，领导就叫一些医生回家休息去免得浪费医院资源


## 2.2 线程池功能

线程池一般有以下三个功能:  

- 创建线程池
- 销毁线程池
- 添加新任务

以上是对外的三个接口方法;

本次实现的线程池对外有四个接口:

```
struct sl_thread_pool *sl_thread_pool_create(unsigned int core_td_num, unsigned int max_td_num, int alive_time);
void sl_thread_pool_destory(struct sl_thread_pool *pool);
void sl_thread_pool_destory_now(struct sl_thread_pool *pool);
int sl_thread_pool_push_task(struct sl_thread_pool *pool, void *(*task_fun)(void *arg), void *arg);
```

在销毁线程的时候我做了个功能细化,分为两种: 一种是立即销毁线程池, 一种是执行完任务队列中的任务再销毁线程池,两种方式都是为阻塞方式;


## 2.3 API 介绍

### 2.3.1 创建线程池
    
```
struct sl_thread_pool *sl_thread_pool_create(unsigned int core_td_num, unsigned int max_td_num, int alive_time);
```

> core_td_num:  初始化线程数  
max_td_num: 最大线程数目(线程数量是动态分配)  
alive_time:  线程空闲时存活的时间,单位:毫秒  
return: 返回线程池句柄  

该接口主要是用于创建一个线程池, 笔者写的该线程池可以动态的伸缩所以加入了最大线程数限制和存活时间.


### 2.3.2 销毁线程池

```
void sl_thread_pool_destory(struct sl_thread_pool *pool);
```

调用该接口时,线程池不会立马被注销而是处理完任务队列中的所有任务才注销;

```
void sl_thread_pool_destory_now(struct sl_thread_pool *pool);
```

调用该接口时,立即注销线程池;


### 2.3.3 添加新任务

```
int sl_thread_pool_push_task(struct sl_thread_pool *pool, void *(*task_fun)(void *arg), void *arg);
```


向线程池中添加一个新任务, 形参`task_fun`为任务的函数指针, `arg`为函数指针的参数;


# 三. 实现原理

笔者写的该线程池有两个重要的链表:一个是线程链表,一个是任务链表,还有一个重要的线程:manager线程,用于管理线程的销毁和创建;

## 3.1 线程池创建

```
struct sl_thread_pool *sl_thread_pool_create(unsigned int core_td_num, unsigned int max_td_num, int alive_time)
{
    struct sl_thread_pool *pstp = NULL;
    struct sl_thread    *thread = NULL;
    int create_ret = -1;
    pstp = (struct sl_thread_pool*)malloc(sizeof(struct sl_thread_pool));     ①
    if (pstp == NULL) {
        ERR("%s: malloc error for creat pool", __FUNCTION__);
        goto malloc_pool_err;
    }
    
    
    create_ret = sl_create_thread_key(destructor_fun);                        ②
    if (create_ret != 0) {
        ERR("%s: create thread key error", __FUNCTION__);
        goto create_key_err;
    }

    /* 创建manager*/
    create_manager_looper(pstp);
    thread = sl_thread_create(sl_thread_manager_do, pstp);                    ③
    if (thread == NULL) {
        ERR("%s: malloc error for create pool", __FUNCTION__);
        goto create_manager_err;
    } else {
        pstp->thread_manager = thread;
        pthread_setname_np(thread->thread_id, "manager_thread"); 
    }

    /* 初始化线程池链表 */
    list_head_init(&pstp->thread_head);
    list_head_init(&pstp->task_queue.task_head);
    /* 初始化线程池计数 */
    pstp->core_threads_num = core_td_num;
    pstp->max_threads_num = max_td_num;
    pstp->keep_alive_time = alive_time;
    pstp->alive_threads_num = 0;
    pstp->destory = 0;

    pthread_mutex_init(&pstp->thread_mutex, NULL);
    // pthread_cond_init(&pstp->thread_run_signal, NULL);

    /* 初始化工作锁 */
    pthread_mutex_init(&pstp->task_queue.task_mutex, NULL);
    /* 初始化工作队列同步条件 */
    pthread_cond_init(&pstp->task_queue.task_ready_signal, NULL);

    /* 创建核心线程 */
    for (int i = 0; i < pstp->core_threads_num; i++) {                       ④
       thread = sl_thread_create(sl_thread_do, pstp);
       if (thread != NULL) {
            list_add(&pstp->thread_head, &thread->thread_list);   
            pthread_setname_np(thread->thread_id, "core_thread");      
       } else {
           i--;
       }
    }

    /* 等待核心线程创建完成 */
    while (pstp->alive_threads_num != pstp->core_threads_num);

    return pstp;


create_manager_err:
    pthread_key_delete(g_key);

create_key_err:
    free(pstp);

malloc_pool_err:

    return NULL;
}
```

> ①: 为线程池分配空间.  
> ②: 创建线程私有数据.  
> ③: 创建manager线程.  
> ④: 创建核心线程,这一定数量的线程是不会被释放的.  


线程池的数据结构如下:  

```
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
```

## 3.2 线程管理


```
static void *sl_thread_manager_do(void *arg)
{
    struct sl_thread_pool *pstp = (struct sl_thread_pool *)arg;
    int next_poll_time = -1; 
    int keep_alive_time = -1;

    if (pstp == NULL) {
        ERR("%s: pool is NULL", __FUNCTION__);
        return NULL;
    }

    do {
        usleep(100);
    } while(pstp->thread_manager == NULL);

    while (pstp->thread_manager->thread_status != THREAD_QUIT) {
        keep_alive_time = poll_event(pstp, next_poll_time);
        next_poll_time = get_next_poll_time(keep_alive_time);
    }
    INFO("sl_thread_manager_do quit");

    return NULL;
}
```

manager线程主要是`epoll`来轮询事件,然后做出相应的处理;主要的事件有三个:  

- 线程挂起事件(空闲)  
- 新增任务事件  
- 注销事件  

```
static int poll_event(struct sl_thread_pool *pool, int time_out)
{
    ...

    struct epoll_event eventItems[EPOLL_MAX_EVENTS];
    int event_count = epoll_wait(g_epoll_fd, eventItems, EPOLL_MAX_EVENTS, time_out);
   
    ...
    
    // Check for poll timeout.
    if (event_count == 0) {                                                ①
        list_for_each(plh, &pstp->thread_head) { 
            pst = sl_list_entry(plh, struct sl_thread, thread_list);
            DEBUG("%s: pstp->alive_threads_num = %d, %ld thread status %s", __FUNCTION__, pstp->alive_threads_num, pst->thread_id, get_status(pst->thread_status));
            if (pstp->alive_threads_num > pstp->core_threads_num) {
                if (pst->thread_status == THREAD_SUPPEND) {
                    pst->thread_status = THREAD_QUIT;
                    sl_notify_all(&pstp->task_queue);
                    delete_when_each(plh);     
                    pthread_join(pst->thread_id, NULL);
                    free(pst);
                    keep_time = 50;  // 50ms再检测一次
                    break;
                }
            } else {
                keep_time = -1;
                break;
            }
        }
        return keep_time;
    }

    // despatch for poll event
    for (int i = 0; i < event_count; i++) {
        fd = eventItems[i].data.fd;
        epoll_events = eventItems[i].events;
        if ((fd == g_wake_read_pip_fd) && (epoll_events & EPOLLIN)) {
            /* thread和task同时来临只处理thread */
            ret_event = sl_get_event();
            switch(ret_event) {
                case EVENT_THREAD:                                        ②
                    DEBUG("EVENT_THREAD");
                    if (pstp->alive_threads_num > pstp->core_threads_num)  {
                        keep_time = pstp->keep_alive_time;             
                    } else {
                        keep_time = -1;
                    }
                    break;

                case EVENT_TASK:                                           ③
                    DEBUG("EVENT_TASK");
                    /* 判断当前线程的消息和当前运行线程比例 */
                    pstq = &pstp->task_queue;
                    if(pstq->num_tasks_alive >= (pstp->alive_threads_num * 2) && (pstp->alive_threads_num <= pstp->max_threads_num)) {
                        /* 创建线程 */
                        pst = sl_thread_create(sl_thread_do, pstp);
                        if (pst != NULL) {
                            list_add(&pstp->thread_head, &pst->thread_list); 
                            pthread_setname_np(pst->thread_id, "other_thread"); 
                        }
                    }
                    break;
                case EVENT_SHUTDOWN:                                       ④
                    DEBUG("EVENT_SHUTDOWN");
                    /* 执行完任务对列中的任务才shutdown */
                    pstp->core_threads_num = 0;
                    pool->destory = 1;
                    break;
                default: break;
            }
        } 
    }

    return keep_time;
}
```

> ①:  wait超时的处理,一般进入超时状态都是准备注销线程, 线程空闲时则注销.  
②: 线程状态变化处理,判断当前线程是否多余核心线程,如果是则设置存活时间为下一轮的wait超时时间.  
③: 发送任务事件后,主要是判断当前任务数量,线程池是否处理的过来,否则创建新线程.  
④: 注销事件,核心线程数设置为0,等待任务链表中的任务处理完再注销;  


事件的轮询主要是借助`epoll`监控管道的变化实现,想了解的可以详细看下代码;


## 3.3 任务的执行

```
static void *sl_thread_do(void *arg)
{
    struct sl_thread_pool *pstp = (struct sl_thread_pool *)arg;
    struct sl_thread_task *pstt = NULL;
    struct sl_task_queue  *pstq = NULL;
    
    if (pstp == NULL) {
        ERR("%s: pool is NULL", __FUNCTION__);
        return NULL;
    }

    pstq = &pstp->task_queue;

    pthread_mutex_lock(&pstp->thread_mutex);
    pstp->alive_threads_num++;
    pthread_mutex_unlock(&pstp->thread_mutex);

    sl_save_thread_self(pstp);

    while (sl_get_thread_self()->thread_status != THREAD_QUIT) {  

        pstt = sl_task_pull(pstq);                                         ①
        if (pstt != NULL) {
            sl_update_thread_status(THREAD_WORKING);
            pstt->task_fun(&pstt->arg);                                    ②
            free(pstt);
        }
    }

    pthread_mutex_lock(&pstp->thread_mutex);
    pstp->alive_threads_num--;
    pthread_mutex_unlock(&pstp->thread_mutex);

    sl_update_thread_status(THREAD_IDLE);                                  

    sl_clear_thread_self();                                                 ③

    INFO("thread_run_task %ld quit, currten threads count %d, currten tasks count %d\n", 
                    pthread_self(), pstp->alive_threads_num, pstq->num_tasks_alive);
    
    return NULL;
}
```

> ①: 从任务对列中取出一个任务, 没有则休眠;  
②: 执行任务  
③: 清除私有数据中存放的值  

这在说明一点,用线程的私有数据进行存储, 主要是为了更新线程的状态方便; 


## 3.4 任务添加


```
int sl_thread_pool_push_task(struct sl_thread_pool *pool, void *(*task_fun)(void *arg), void *arg)
{
    struct sl_task_queue *pstq = NULL;
    struct sl_thread_task *pstt = NULL;

    if (pool == NULL || task_fun == NULL || pool->destory == 1) {
        ERR("%s: pool or task_fun is NULL or is destory status", __FUNCTION__);
        return -1;
    }  

    pstq = &pool->task_queue;

    pstt = (struct sl_thread_task*)malloc(sizeof(struct sl_thread_task));    
    if (pstt == NULL) {
        ERR("%s: malloc error for creat a task", __FUNCTION__);
        return -1;
    }

    pstt->task_fun = task_fun;
    pstt->arg      = arg;

    return sl_task_push(pstq, pstt);
}
```

该接口主要分配了一个空间初始化传进来的任务,往下看:   


```
static int sl_task_push(struct sl_task_queue *_stq, struct sl_thread_task *new_task)
{
    struct sl_task_queue *pstq = _stq;
    struct sl_thread_task *pstt = new_task;

    if (pstq == NULL || pstt == NULL) {
        ERR("%s: pstq or pstt is NULL", __FUNCTION__);
        return -1;
    }  

    pthread_mutex_lock(&pstq->task_mutex);
    list_add(&pstq->task_head, &pstt->task_list);
    pstq->num_tasks_alive++;
    pthread_mutex_unlock(&pstq->task_mutex);
    sl_notify_one(pstq);
    sl_update_task_queue_info();
    return pstq->num_tasks_alive;
}
```

将刚才保存的任务添加进任务对列并发送通知;


# 四. 总结

笔者写这个线程池,主要涉及到这个点有: 同步变量, 锁, 线程私有数据, 管道, epoll和双向队列;

代码已经放到我的github上了: [thread pool](https://github.com/SourceLink/thread_pool)
