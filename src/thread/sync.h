/*
 * @date:
 */
#ifndef __THREAD_SYNC_H
#define __THREAD_SYNC_H
#include "list.h"
#include "stdint.h"
#include "thread.h"

/* 信号量结构 */
// 信号量是一种同步机制，信号量就是一个计数器 p = --, v = ++
struct semaphore
{
   uint8_t value;
   struct list waiters; // 记录再此信号量上等待的所有线程（这些线程都被阻塞了）
};

/* 锁结构 */
struct lock
{
   struct task_struct *holder; // 锁的持有者
   struct semaphore semaphore; // 用二元信号量实现锁
   uint32_t holder_repeat_nr;  // 锁的持有者重复申请锁的次数， 此变量存在的意义在于当线程进入临界区后仍然有可能获得本锁
};

typedef struct lock mutex_t;
typedef struct lock lock_t;
typedef struct semaphore semaphore_t;

void sema_init(struct semaphore *psema, uint8_t value);
void sema_down(struct semaphore *psema);
void sema_up(struct semaphore *psema);
void lock_init(struct lock *plock);
void lock_acquire(struct lock *plock);
void lock_release(struct lock *plock);

#endif
