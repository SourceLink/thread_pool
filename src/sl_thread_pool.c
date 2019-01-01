/*
************************************ (C) COPYRIGHT 2018 Sourcelink **************************************
 * File Name	: sl_thread_pool.c
 * Author		: Sourcelink 
 * Version		: V1.0
 * Date 		: 2018/12/16
 * Description	: 2018年12月16日: 数据结构双向链表栈
 *				  
 ********************************************************************************************************
*/

#include "sl_thread_pool.h"
#include "sl_debug.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <string.h>

DEBUG_SET_LEVEL(DEBUG_LEVEL_DEBUG);




#define NAME(STA)   case STA: return #STA
const char *get_status(int sta)
{
    switch(sta) {
       NAME(THREAD_IDLE);
       NAME(THREAD_SUPPEND);
       NAME(THREAD_RUNING);
       NAME(THREAD_WORKING);
       
       default: return "???";
    }
}






#define EPOLL_SIZE_HINT  8
#define EPOLL_MAX_EVENTS 16

#define THREAD_STATUS       "S"
#define TASK_QUEUE_INFO     "I"
#define POOL_DESTORY        "D"

static pthread_key_t g_key;
static int g_epoll_fd = -1;
static int g_wake_read_pip_fd = -1;
static int g_wake_write_pip_fd = -1;

static struct sl_thread_task *sl_task_pull(struct sl_task_queue *_stq);
static int sl_task_push(struct sl_task_queue *_stq, struct sl_thread_task *new_task);
static int sl_task_queue_clear(struct sl_task_queue *_stq);


static struct sl_thread *sl_thread_create(void *(*task_fun)(void *arg), void  *arg);
static int sl_threads_destory(struct sl_thread_pool *pool);
static void sl_manager_destory(struct sl_thread *thread);
static void *sl_thread_do(void *arg);
static void *sl_thread_manager_do(void *pool);
static void create_manager_looper(struct sl_thread_pool *pool);
static void sl_update_thread_status(type_thread_status status);
static void sl_update_task_queue_info(void);
static type_event sl_get_event(void);
static int get_next_poll_time(int _keep_time);
static void sl_update_pool_destory_info(void);

static void wake(const char *info);

static void destructor_fun(void *arg)
{
    INFO("key will delete\n");
}


/*
*********************************************************************************************************
*    函 数 名: sl_creat_thread_key
*    功能说明: 创建线程私有数据key
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static int sl_create_thread_key(void (*__destr_function) (void *))
{
    int ret = -1;
    if(pthread_key_create(&g_key, __destr_function) == 0) {
       ret = 0;
    }
    return ret;
}


/*
*********************************************************************************************************
*    函 数 名: sl_save_thread_self
*    功能说明: 将自己存入私有数据中
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static struct sl_thread *sl_save_thread_self(struct sl_thread_pool *_stp)
{

    struct sl_thread_pool *pstp = _stp;
    struct list_head *plh = NULL;
    struct sl_thread *pst = NULL;

    do {
         list_for_each(plh, &pstp->thread_head) {
            pst = sl_list_entry(plh, struct sl_thread, thread_list);
            if (pthread_self() == pst->thread_id) {
                pthread_setspecific(g_key, pst);
                break; 
            } else {
                pst = NULL; 
            }
        }
    } while(pst == NULL);
    
    return pst;
}


/*
*********************************************************************************************************
*    函 数 名: sl_clear_thread_self
*    功能说明: 清除私有数据指针
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static void sl_clear_thread_self(void)
{
    pthread_setspecific(g_key, NULL);
}


/*
*********************************************************************************************************
*    函 数 名: sl_get_thread_self
*    功能说明: 获取线程本身
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static struct sl_thread *sl_get_thread_self() 
{
    struct sl_thread *ret = NULL;
    ret = (struct sl_thread *)pthread_getspecific(g_key);
    return ret;
}


/*
*********************************************************************************************************
*    函 数 名: wait_run_task_signal
*    功能说明: 等待任务就绪信号
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static void wait_task_signal(struct sl_task_queue *_stq)
{
    struct sl_task_queue *pstq = _stq;
    if (pstq != NULL) {
        pthread_mutex_lock(&pstq->task_mutex);
        while (list_empty(&pstq->task_head) && sl_get_thread_self()->thread_status != THREAD_QUIT) {
            sl_update_thread_status(THREAD_SUPPEND);
            pthread_cond_wait(&pstq->task_ready_signal, &pstq->task_mutex);
        }
        if (sl_get_thread_self()->thread_status != THREAD_QUIT) {
            sl_update_thread_status(THREAD_RUNING);
        }
        pthread_mutex_unlock(&pstq->task_mutex);
        
    }
}


/*
*********************************************************************************************************
*    函 数 名: sl_notify_one
*    功能说明: 发送一个任务信号
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static void sl_notify_one(struct sl_task_queue *_stq)
{
    struct sl_task_queue *pstq = _stq;
    if (pstq != NULL) {
        pthread_cond_signal(&pstq->task_ready_signal);
    }
}


/*
*********************************************************************************************************
*    函 数 名: sl_notify_all
*    功能说明: 唤醒所有被阻塞的线程
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static void sl_notify_all(struct sl_task_queue *_stq)
{
    struct sl_task_queue *pstq = _stq;
    if (pstq != NULL) {
        pthread_cond_broadcast(&pstq->task_ready_signal);
    }
}


/*
*********************************************************************************************************
*    函 数 名: sl_thread_pool_create
*    功能说明: 创建线程池
*    形    参: core_td_num:初始化线程数    max_td_num:最大线程数目,线程数量是动态分配    queue_size:任务对列的数目 
*    返 回 值: 返回创建好的线程池对象
*********************************************************************************************************
*/
struct sl_thread_pool *sl_thread_pool_create(unsigned int core_td_num, unsigned int max_td_num, int alive_time)
{
    struct sl_thread_pool *pstp = NULL;
    struct sl_thread    *thread = NULL;
    int create_ret = -1;
    pstp = (struct sl_thread_pool*)malloc(sizeof(struct sl_thread_pool));
    if (pstp == NULL) {
        ERR("%s: malloc error for creat pool", __FUNCTION__);
        goto malloc_pool_err;
    }
    
    
    create_ret = sl_create_thread_key(destructor_fun);
    if (create_ret != 0) {
        ERR("%s: create thread key error", __FUNCTION__);
        goto create_key_err;
    }

    /* 创建manager*/
    create_manager_looper(pstp);
    thread = sl_thread_create(sl_thread_manager_do, pstp);
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
    for (int i = 0; i < pstp->core_threads_num; i++) {
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


/*
*********************************************************************************************************
*    函 数 名: sl_thread_pool_destory
*    功能说明:  
*    形    参: 无
*    返 回 值: 无
*********************************************************************************************************
*/
void sl_thread_pool_destory_now(struct sl_thread_pool *pool)
{
    struct list_head *plh = NULL;
    struct sl_thread *pst = NULL;
    struct sl_thread_task *pstt = NULL;
    
    if (pool != NULL) {

        pool->destory = 1;
        /* 释放工作队列 */
        sl_task_queue_clear(&pool->task_queue);
        
        /* 释放线程链表 */
        sl_threads_destory(pool);

        /* manager注销 */
        sl_manager_destory(pool->thread_manager);

        pthread_mutex_destroy(&pool->thread_mutex);
        /* 注销工作锁 */
        pthread_mutex_destroy(&pool->task_queue.task_mutex);
        /* 注销工作队列同步条件 */
        pthread_cond_destroy(&pool->task_queue.task_ready_signal);
    }
}


/*
*********************************************************************************************************
*    函 数 名: sl_thread_pool_destory
*    功能说明:  注销线程池,等待工作对列中的队列都执行完再注销.该函数会柱塞;
*    形    参: 无
*    返 回 值: 无
*********************************************************************************************************
*/
void sl_thread_pool_destory(struct sl_thread_pool *pool)
{
    struct list_head *plh = NULL;
    struct sl_thread *pst = NULL;
    struct sl_thread_task *pstt = NULL;
    
    if (pool != NULL) {

        sl_update_pool_destory_info();

        do {
            usleep(10000);
        }
        while (pool->task_queue.num_tasks_alive != 0 || pool->alive_threads_num != 0);
        /* 释放工作队列 */
        sl_task_queue_clear(&pool->task_queue);

        /* manager注销 */
        sl_manager_destory(pool->thread_manager);

        pthread_mutex_destroy(&pool->thread_mutex);
        /* 注销工作锁 */
        pthread_mutex_destroy(&pool->task_queue.task_mutex);
        /* 注销工作队列同步条件 */
        pthread_cond_destroy(&pool->task_queue.task_ready_signal);
    }
}

/*
*********************************************************************************************************
*    函 数 名: sl_thread_pool_push_task
*    功能说明:  向线程池添加一个任务 
*    形    参: 无
*    返 回 值: 返回当前任务链表中的任务数量
*********************************************************************************************************
*/
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


/*
*********************************************************************************************************
*    函 数 名: sl_task_push
*    功能说明:  向任务链表中压入一个任务 
*    形    参: 无
*    返 回 值: 返回当前任务链表中的任务数量
*********************************************************************************************************
*/
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


/*
*********************************************************************************************************
*    函 数 名:  sl_task_pull
*    功能说明:  当任务链表中无任务时,调用该函数会休眠
*    形    参: 无
*    返 回 值: 返回从任务链表取出的任务块
*********************************************************************************************************
*/
static struct sl_thread_task *sl_task_pull(struct sl_task_queue *_stq)
{
    struct sl_task_queue *pstq = _stq;
    struct list_head     *plh  = NULL;
    struct sl_thread_task *pstt = NULL;

    if (pstq == NULL) {
        ERR("%s: pstq is NULL", __FUNCTION__);
        return pstt;
    }

    wait_task_signal(pstq);

    pthread_mutex_lock(&pstq->task_mutex);
    list_for_each(plh, &pstq->task_head) {
        pstt = sl_list_entry(plh, struct sl_thread_task, task_list);
        list_delete(plh);
        pstq->num_tasks_alive--;
        break;
    }
    pthread_mutex_unlock(&pstq->task_mutex);

    return pstt;
}


/*
*********************************************************************************************************
*    函 数 名:  sl_task_queue_clear
*    功能说明:  清除任务链表
*    形    参: 无
*    返 回 值: 返回剩余任务计数
*********************************************************************************************************
*/
static int sl_task_queue_clear(struct sl_task_queue *_stq)
{
    struct sl_task_queue *pstq = _stq;
    struct list_head     *plh  = NULL;
    struct sl_thread_task *pstt = NULL;

    if (pstq == NULL) {
        ERR("%s: pstq is NULL", __FUNCTION__);
        return -1;
    }
    
    /* 释放工作队列 */
    pthread_mutex_lock(&pstq->task_mutex);
    list_for_each(plh, &pstq->task_head) { 
        pstt = sl_list_entry(plh, struct sl_thread_task, task_list);
        delete_when_each(plh);
        free(pstt);
        pstq->num_tasks_alive--;
    }
    pthread_mutex_unlock(&pstq->task_mutex);

    return pstq->num_tasks_alive;
}


/*
*********************************************************************************************************
*    函 数 名: sl_thread_create
*    功能说明: 创建一个线程
*    形    参: task_fun: 线程运行函数   arg: 形参
*    返 回 值: 返回创建好的线程对象
*********************************************************************************************************
*/
static struct sl_thread *sl_thread_create(void *(*task_fun)(void *arg), void  *arg)
{
    struct sl_thread *pst = NULL;
    pst = (struct sl_thread *)malloc(sizeof(struct sl_thread));
    if(pst == NULL) {
        ERR("%s: malloc error for creat a thread", __FUNCTION__);
        goto malloc_err;
    }

    pst->thread_status = THREAD_IDLE;

    if(pthread_create(&pst->thread_id, NULL, task_fun, arg) != 0) {
        ERR("%s: pthread_create error", __FUNCTION__);
        goto create_err;
    }

    return pst;

create_err:
    free(pst);

malloc_err:

    return NULL;
}


/*
*********************************************************************************************************
*    函 数 名: sl_threads_destory
*    功能说明:  注销线程池内所有线程
*    形    参: 
*    返 回 值: 返回剩余正在运行的线程数
*********************************************************************************************************
*/
static int sl_threads_destory(struct sl_thread_pool *pool)
{
    struct list_head *plh = NULL;
    struct sl_thread *pst = NULL;

    if (pool == NULL) {
        ERR("%s: pool is NULL", __FUNCTION__);
        return -1;
    }

    list_for_each(plh, &pool->thread_head) { 
        pst = sl_list_entry(plh, struct sl_thread, thread_list);
        delete_when_each(plh);
        pst->thread_status = THREAD_QUIT;
        sl_notify_all(&pool->task_queue);
        pthread_join(pst->thread_id, NULL);
        free(pst);
    }
    return pool->alive_threads_num;
}


/*
*********************************************************************************************************
*    函 数 名: sl_manager_destory
*    功能说明:  注销manager线程
*    形    参: 
*    返 回 值: 返回剩余正在运行的线程数
*********************************************************************************************************
*/
static void sl_manager_destory(struct sl_thread *thread)
{
    struct sl_thread *pst = NULL;

    if (thread == NULL) {
        ERR("%s: thread is NULL", __FUNCTION__);
        return ;
    }

    thread->thread_status = THREAD_QUIT;
    wake("W");
    pthread_join(thread->thread_id, NULL);
    free(thread);
}

/*
*********************************************************************************************************
*    函 数 名: create_manager_looper
*    功能说明: 创建一个epoll对象用于轮询时间
*    形    参: 
*    返 回 值: none
*********************************************************************************************************
*/
static void create_manager_looper(struct sl_thread_pool *pool)
{
    int wake_fds[2];
    int result = -1; 

    if (pool == NULL) {
        ERR("%s: pool is NULL", __FUNCTION__);
        return;
    }

    result = pipe(wake_fds);
    if (result != 0) {
        ERR("%s: pipe init error", __FUNCTION__);
        return ;
    }

    g_wake_read_pip_fd = wake_fds[0];
    g_wake_write_pip_fd = wake_fds[1];

    result = fcntl(g_wake_read_pip_fd, F_SETFL, O_NONBLOCK);
    if (result != 0) {
        ERR("Could not make wake read pipe non-blocking.");
        return ;
    }
        
    result = fcntl(g_wake_write_pip_fd, F_SETFL, O_NONBLOCK);
    if (result != 0) {
        ERR("Could not make wake read pipe non-blocking.");
        return ;
    }
    
    g_epoll_fd = epoll_create(EPOLL_SIZE_HINT);
    if (g_epoll_fd < 0) {
        ERR("%s: Could not create epoll instance.", __FUNCTION__);
        return ;
    }

    struct epoll_event eventItem;
    memset(&eventItem, 0, sizeof(struct epoll_event)); 
    eventItem.events = EPOLLIN;
    eventItem.data.fd = g_wake_read_pip_fd;
    result = epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_wake_read_pip_fd, &eventItem);
    if (result != 0) {
        ERR("%s: Could not add wake read pipe to epoll instance.", __FUNCTION__);
        return ;
    }
}


/*
*********************************************************************************************************
*    函 数 名: poll_event
*    功能说明: 轮循事件,主要是线程状态变化和任务队列变化时间
*    形    参: time_out: 等待超时时间
*    返 回 值: 根据事件计算出线程的活动时间
*********************************************************************************************************
*/
static int poll_event(struct sl_thread_pool *pool, int time_out)
{
    struct sl_thread_pool *pstp = pool;
    struct list_head       *plh = NULL;
    struct sl_task_queue  *pstq = NULL;
    struct sl_thread       *pst = NULL;
    int fd = -1;
    uint32_t epoll_events = 0;
    type_event ret_event;
    int keep_time = -1;
    int ret = -1;

    if (pstp == NULL) {
        ERR("%s: pool is NULL", __FUNCTION__);
        return keep_time;
    }

    struct epoll_event eventItems[EPOLL_MAX_EVENTS];
    int event_count = epoll_wait(g_epoll_fd, eventItems, EPOLL_MAX_EVENTS, time_out);
   
    // Check for poll error.
    if (event_count < 0) {
        ERR("%s: epoll_wait is error", __FUNCTION__);
        return keep_time;
    }

    // Check for poll timeout.
    if (event_count == 0) {
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
                case EVENT_THREAD:
                    DEBUG("EVENT_THREAD");
                    if (pstp->alive_threads_num > pstp->core_threads_num)  {
                        keep_time = pstp->keep_alive_time;             
                    } else {
                        keep_time = -1;
                    }
                    break;

                case EVENT_TASK:
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
                case EVENT_SHUTDOWN:
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


/*
*********************************************************************************************************
*    函 数 名: get_abs_keep_alive_time
*    功能说明: 根据传进来的seconds计算出alivetime的绝对时间
*    形    参: seconds:keep alive time的相对时间
*    返 回 值: none
*********************************************************************************************************
*/
static struct timespec get_abs_keep_alive_time(double seconds)
{
    struct timespec abs_time;
    const long nanopersecond = 1000000000;
    long nanoseconds = 0;

    clock_gettime(CLOCK_REALTIME, &abs_time);

    nanoseconds = seconds * nanopersecond;

    abs_time.tv_sec += (time_t)((abs_time.tv_nsec + nanoseconds) / nanopersecond);
    abs_time.tv_nsec = (long)((abs_time.tv_nsec + nanoseconds) % nanopersecond);

    return abs_time;
}


/*
*********************************************************************************************************
*    函 数 名: get_next_poll_time
*    功能说明: 根据keep_alive_time计算出下一轮wait时间
*    形    参: _keep_time: 线程活动时间
*    返 回 值: 下一轮wait时间, -1: 无限等待 0:不等待 other: 等待的具体时间 
*********************************************************************************************************
*/
static int get_next_poll_time(int _keep_time)
{
    struct timespec currten_time;
    static struct timespec s_abs_keep_time;
    /* unit: ms */
    static int s_poll_wait_time = -1;

    double sec = 0;
    double nsec = 0;
    /* unit: second */
    double next_keep_time = 0;

    if (_keep_time == -1) {
       s_poll_wait_time = -1; 
       memset(&s_abs_keep_time, 0, sizeof(struct timespec));
    } else {
        if (s_poll_wait_time != -1) {
            /* 说明此时在等待超时,但被其他信号唤醒 */
            clock_gettime(CLOCK_REALTIME, &currten_time);
            if ((currten_time.tv_sec >= s_abs_keep_time.tv_sec) && (currten_time.tv_nsec >= s_abs_keep_time.tv_nsec)) {
                /* 从头开始更新 */
                next_keep_time = (double)_keep_time / 1000;
                s_abs_keep_time = get_abs_keep_alive_time(next_keep_time);
                s_poll_wait_time = _keep_time;
            } else {
                /* 计算还有多长时间才达到超时 */
                sec = (double)(s_abs_keep_time.tv_sec - currten_time.tv_sec);
                nsec = (double)(s_abs_keep_time.tv_nsec - currten_time.tv_nsec);
                next_keep_time = sec + nsec / 1000000000;
                DEBUG("poll sec = %f, nsec = %f, next_keep_time = %f", sec, nsec, next_keep_time);
                s_abs_keep_time = get_abs_keep_alive_time(next_keep_time);
                s_poll_wait_time = (int)(sec * 1000 + nsec / 1000000);
            }
        } else {
            /* 记录下当前绝对时间 */
            next_keep_time = (double)_keep_time / 1000;
            s_abs_keep_time = get_abs_keep_alive_time(next_keep_time);
            s_poll_wait_time = _keep_time;
        }
    }

    return s_poll_wait_time;
}


/*
*********************************************************************************************************
*    函 数 名: wake
*    功能说明: 唤醒manager线程
*    形    参: info:唤醒信息
*    返 回 值: none
*********************************************************************************************************
*/
static void wake(const char *info)
{
    ssize_t num_write;
    do {
        num_write = write(g_wake_write_pip_fd, info, strlen(info));
    } while (num_write == -1);
}


/*
*********************************************************************************************************
*    函 数 名: awoken
*    功能说明: 读取收到的消息
*    形    参: _event: 将读取到的消息写进行该buf
*    返 回 值: none
*********************************************************************************************************
*/
static void awoken(char *_event)
{
    char buffer[16];
    ssize_t num_read;
    do {
        num_read = read(g_wake_read_pip_fd, buffer, sizeof(buffer));
    } while (num_read == -1  || num_read == sizeof(buffer));
    buffer[num_read] = '\0';
    memcpy(_event, buffer, num_read);
}


/*
*********************************************************************************************************
*    函 数 名: sl_update_thread_status
*    功能说明: 更新线程状态,当status为THREAD_SUPPEND时通知manager线程
*    形    参: status:线程状态值
*    返 回 值: none
*********************************************************************************************************
*/
static void sl_update_thread_status(type_thread_status status)
{
    int temp_status = sl_get_thread_self()->thread_status;
    if (temp_status != status) {
        sl_get_thread_self()->thread_status = status;
    }
    
    if (status == THREAD_SUPPEND && temp_status != status) {
        wake(THREAD_STATUS);
    }
}


/*
*********************************************************************************************************
*    函 数 名: sl_update_task_queue_info
*    功能说明: 通知manager线程已经更新的工作队列
*    形    参: none
*    返 回 值: none
*********************************************************************************************************
*/
static void sl_update_task_queue_info(void)
{
    wake(TASK_QUEUE_INFO);
}


/*
*********************************************************************************************************
*    函 数 名: sl_update_pool_destory_info
*    功能说明: 
*    形    参: none
*    返 回 值: none
*********************************************************************************************************
*/
static void sl_update_pool_destory_info(void)
{
    wake(POOL_DESTORY);
}

/*
*********************************************************************************************************
*    函 数 名: sl_get_event
*    功能说明: 从管道中获取的buf中解析事件, 一次只能返回一个事件,需要修改
*    形    参: none
*    返 回 值: 获取到的事件
*********************************************************************************************************
*/
static type_event sl_get_event(void)
{
    char buffer[16] = {0};
    type_event ret_event = EVENT_IDLE;
    awoken(buffer);
    DEBUG("%s: buffer is %s", __FUNCTION__, buffer);
    if (strstr(buffer, POOL_DESTORY)) {
        ret_event = EVENT_SHUTDOWN;
    } else if (strstr(buffer, THREAD_STATUS)) {
        ret_event = EVENT_THREAD;
    } else if (strstr(buffer, TASK_QUEUE_INFO)) {
        ret_event = EVENT_TASK;
    } 
   
   return  ret_event;
}


/*
*********************************************************************************************************
*    函 数 名: sl_thread_manager_do
*    功能说明: 负责线程数目的动态控制
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
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
}


/*
*********************************************************************************************************
*    函 数 名: sl_thread_do
*    功能说明: 该函数功能是从任务队列中取出任务并执行
*    形    参: 
*    返 回 值: 
*********************************************************************************************************
*/
static void *sl_thread_do(void *arg)
{
    struct sl_thread_pool *pstp = (struct sl_thread_pool *)arg;
    struct sl_thread_task *pstt = NULL;
    struct sl_task_queue  *pstq = NULL;
    struct sl_thread      *pst  = NULL;
    
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

        pstt = sl_task_pull(pstq); 
        if (pstt != NULL) {
            sl_update_thread_status(THREAD_WORKING);
            pstt->task_fun(&pstt->arg);
            free(pstt);
        }
    }

    pthread_mutex_lock(&pstp->thread_mutex);
    pstp->alive_threads_num--;
    pthread_mutex_unlock(&pstp->thread_mutex);

    sl_update_thread_status(THREAD_IDLE);

    sl_clear_thread_self();

    INFO("thread_run_task %ld quit, currten threads count %d, currten tasks count %d\n", 
                    pthread_self(), pstp->alive_threads_num, pstq->num_tasks_alive);
    
    return NULL;
}