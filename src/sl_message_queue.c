#include "sl_message_queue.h"
#include "sl_debug.h"
#include <stdlib.h>

DEBUG_SET_LEVEL(DEBUG_LEVEL_ERR);

static struct sl_message_queue g_msg_queue;

void sl_queue_create(void)
{
    list_head_init(&g_msg_queue.msg_head);
    pthread_mutex_init(&g_msg_queue.msg_mutex, NULL);
}


int sl_push_msg(struct sl_messgae *new_msg)
{
   struct sl_messgae *psm = new_msg;
   struct list_head *phead = &g_msg_queue.msg_head;

    if (psm == NULL) {
        ERR("%s: psm is NULL", __FUNCTION__);  
        return -1;
    }

    pthread_mutex_lock(&g_msg_queue.msg_mutex);
    list_add_tail(phead, &psm->msg_list);
    pthread_mutex_unlock(&g_msg_queue.msg_mutex);
    return 0;
}

struct sl_messgae *sl_pull_msg(void)
{
    struct list_head *plh = NULL;
    struct list_head *phead = &g_msg_queue.msg_head;
    struct sl_messgae *psm = NULL;

    if (sl_is_empty_queue()) {
       DEBUG("%s: queue is NULL", __FUNCTION__);   
    }

    pthread_mutex_lock(&g_msg_queue.msg_mutex);
    list_for_each(plh, phead) {
        psm = sl_list_entry(plh, struct sl_messgae, msg_list);
        delete_when_each(plh);
        break;
    }
    pthread_mutex_unlock(&g_msg_queue.msg_mutex);
    return psm;
}


int sl_is_empty_queue(void) 
{
    return list_empty(&g_msg_queue.msg_head);
}


void sl_queue_destory(void)
{
    struct list_head *plh = NULL;
    struct list_head *phead = &g_msg_queue.msg_head;
    struct sl_messgae *psm = NULL;

    pthread_mutex_lock(&g_msg_queue.msg_mutex);
    list_for_each(plh, phead) {
        psm = sl_list_entry(plh, struct sl_messgae, msg_list);
        delete_when_each(plh);
        free(psm);
    }
    pthread_mutex_unlock(&g_msg_queue.msg_mutex);

    pthread_mutex_destroy(&g_msg_queue.msg_mutex);
}