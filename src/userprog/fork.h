#ifndef __USERPROG_FORK_H
#define __USERPROG_FORK_H

#include "thread.h"
/* fork子进程,只能由用户进程通过系统调用fork调用,
   内核线程不可直接调用,原因是要从0级栈中获得esp3等 */

/**
 * @brief sys_fork是fork系统调用的实现函数. 用于复制出来一个子进程. 子进程的数据和代码和父进程的数据代码一模一样
 *
 * @return pid_t 父进程返回子进程的pid; 子进程返回0
 */
pid_t sys_fork(void);

#endif