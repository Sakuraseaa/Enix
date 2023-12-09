/*
 * @Author: SuKun 1214200610@qq.com
 * @Date: 2023-01-26 05:13:54
 * @LastEditors: SuKun 1214200610@qq.com
 * @LastEditTime: 2023-03-09 22:26:33
 * @FilePath: /SKOS/src/userprog/process.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef __USERPROG_PROCESS_H

#define __USERPROG_PROCESS_H
#include "thread.h"
#include "stdint.h"
#define default_prio 31
// 内核栈顶的虚拟起始地址是0xc0000000
#define USER_STACK3_VADDR (0xc0000000 - 0x1000)
// 用户进程的起始虚拟地址
#define USER_VADDR_START 0x8048000
void process_execute(void *filename, char *name);
void start_process(void *filename_);
void process_activate(struct task_struct *p_thread);
void page_dir_activate(struct task_struct *p_thread);
uint32_t *create_page_dir(void);
void create_user_vaddr_bitmap(struct task_struct *user_prog);
#endif
