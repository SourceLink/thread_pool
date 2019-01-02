#include "sl_thread_pool.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

pthread_mutex_t work_mutex;


void *fun1(void * arg)
{
    static int count = 0;
    // while(1) 
    {
        pthread_mutex_lock(&work_mutex);
        count++;
        pthread_mutex_unlock(&work_mutex);
        printf("I will runing count = %d\n", count);
        sleep(1);
    }

    return NULL;
}


int main(int arg, char **argv)
{
    struct sl_thread_pool *creat_pool = NULL;

    pthread_mutex_init(&work_mutex, NULL);

    int time = -10000;

    printf("time: %f\n", (double)time / 10000);

    creat_pool = sl_thread_pool_create(5, 10, 1000);
    
    for (int i = 0; i < 20; i++) {
        sl_thread_pool_push_task(creat_pool, fun1, NULL);
    }

    sl_thread_pool_destory(creat_pool);

    for (int i = 0; i < 3; i++) {
        sl_thread_pool_push_task(creat_pool, fun1, NULL);
    }

    sleep(5);
    for (int i = 0; i < 3; i++) {
        sl_thread_pool_push_task(creat_pool, fun1, NULL);
    }

    printf("time: ==========================\n");
    // // for (int i = 0; i < 3; i++) {
    // //     sl_thread_pool_push_task(creat_pool, fun1, NULL);
    // // }

    // // sleep(5);
    // for (int i = 0; i < 50; i++) {
    //     sl_thread_pool_push_task(creat_pool, fun1, NULL);
    // }

    // sleep(50);
    // for (int i = 0; i < 3; i++) {
    //     sl_thread_pool_push_task(creat_pool, fun1, NULL);
    // }


    // sl_thread_pool_destory(creat_pool);
    sleep(50);

    return 0;
}