#ifndef  _SL_MESSAGE_QUEUE_H
#define  _SL_MESSAGE_QUEUE_H

#include <pthread.h>
#include "sl_list.h"

struct sl_message_queue {
    struct list_head msg_head;
    pthread_mutex_t msg_mutex;             /* 用于线程队列互斥锁 */
};


struct sl_messgae {
    struct list_head msg_list;
    int type;                   /* 消息类型 */
    int code;                   /* 消息代码 */
    int value;                  /* 消息值 */
};


void sl_queue_create(void);
void sl_queue_destory(void);
int sl_push_msg(struct sl_messgae *new_msg);
struct sl_messgae * sl_pull_msg(void);
int  sl_is_empty_queue(void);


#endif