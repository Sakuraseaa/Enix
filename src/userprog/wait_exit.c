#include "wait_exit.h"
#include "global.h"
#include "debug.h"
#include "thread.h"
#include "list.h"
#include "stdio-kernel.h"
#include "memory.h"
#include "bitmap.h"
#include "fs.h"
#include "file.h"
#include "pipe.h"
/**
 * @brief  释放用户进程资源
 *       1. 页表中对应的物理页
 *       2. 虚拟内存池占物理页
 *       3. 关闭打开的文件
 *
 * @param release_thread 要被释放的用户进程的pcb
 */
static void release_prog_resource(struct task_struct *release_thread)
{
    // 1. 页表中对应的物理页
    uint32_t *pgdir_vaddr = release_thread->pgdir;
    uint16_t user_pde_nr = 768, pde_idx = 0; // 用户页目录项总数以及索引
    uint32_t pde = 0;                        // 某个页目录项
    uint32_t *v_pde_ptr = NULL;              // 存储 某个页目录表项 的指针

    uint16_t user_pte_nr = 1024, pte_idx = 0; // 页表表项总数以及索引
    uint32_t pte = 0;                         // 某个页表项
    uint32_t *v_pte_ptr = NULL;               // 存储 某个页表项 的指针

    uint32_t *first_pte_vaddr_in_pde = NULL; // 表示页目录项中第0个页表项的地址，用来遍历页表中的pte
    uint32_t pg_phy_addr = 0;

    // 回收页表，物理页
    while (pde_idx < user_pde_nr)
    {
        v_pde_ptr = pgdir_vaddr + pde_idx;
        pde = *v_pde_ptr;
        if (pde & 0x00000001)
        { // 如果页目录项p位为1,表示该页表中可能有页表项存在，有物理页未被释放

            // pte_ptr(pde_idx * 0x400000)得到页表地址，一个页表的表示范围是4MB, (pde_idx * 0x400000)这个虚拟地址构造的很巧妙
            first_pte_vaddr_in_pde = pte_ptr(pde_idx * 0x400000);
            pte_idx = 0;
            while (pte_idx < user_pte_nr)
            {
                v_pte_ptr = first_pte_vaddr_in_pde + pte_idx;
                pte = *v_pte_ptr;
                if (pte & 0x00000001)
                {
                    // 当且页表项P位为1，则该页表项被分配了，物理页
                    // 那么释放该物理页
                    pg_phy_addr = pte & 0xFFFFF000;
                    free_a_phy_page(pg_phy_addr);
                }
                pte_idx++;
            }

            // 回收页表本身占用的物理页，只设置物理内存池
            pg_phy_addr = pde & 0xFFFFF000;
            free_a_phy_page(pg_phy_addr);
        }
        pde_idx++;
    }

    // 回收用户虚拟内存池池占用的物理内存
    uint32_t bitmap_pg_cnt = (release_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len) / PG_SIZE;
    uint8_t *user_vaddr_pool_bitmap = release_thread->userprog_vaddr.vaddr_bitmap.bits;
    mfree_page(PF_KERNEL, user_vaddr_pool_bitmap, bitmap_pg_cnt);

    // 关闭进程打开的文件
    uint8_t fd_idx = 3;
    while (fd_idx < MAX_FILES_OPEN_PER_PROC)
    {
        if (release_thread->fd_table[fd_idx] != -1)
        {
            if (is_pipe(fd_idx))
            {
                uint32_t global_fd = fd_local2global(fd_idx);
                if (--file_table[global_fd].fd_pos == 0)
                {
                    mfree_page(PF_KERNEL, file_table[global_fd].fd_inode, 1);
                    file_table[global_fd].fd_inode = NULL;
                }
            }
            else
                sys_close(fd_idx);
        }
        fd_idx++;
    }
}

/**
 * @brief find_child用于查找parent_pid所表示的线程的所有子线程
 *
 * @param elem 表示线程的tag
 * @param parent_pid 父进程pid
 * @return true elem表示的线程是parent_pid表示的父进程的子线程
 * @return false elem表示的线程不是parent_pid表示的父进程的子线程
 */
static bool find_child(list_elem_t *pelem, int32_t ppid)
{
    task_status_t *pcb = elem2entry(task_status_t, all_list_tag, pelem);
    if (pcb->parent_pid == ppid) // 若该任务的parent_pid为ppid,返回
        return true;             // list_traversal只有在回调函数返回true时才会停止继续遍历,所以在此返回true
    return false;                // 让list_traversal继续传递下一个元素
}

/**
 * @brief find_hanging_child用于查找parent_pid指定的父进程的所有状态为TASK_HANGING的子进程
 *
 * @param elem 表示线程的tag
 * @param parent_pid 父线程pid
 * @return true elem表示的线程是parent_pid表示的父进程的子线程, 并且是TASK_HANGING状态
 * @return false elem表示的线程不是parent_pid表示的父进程的子线程, 并且是TASK_HANGING状态
 */
static bool find_hanging_child(list_elem_t *pelem, int32_t ppid)
{
    struct task_struct *pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == ppid && pthread->status == TASK_HANGING)
        return true;
    return false;
}

/**
 * @brief init_adopt_a_child用于将pid表示的进程过继给init
 *
 * @param elem 表示线程的tag
 * @param pid 需要过继的线程的pid
 * @return true 当前进程是要过继的进程
 * @return false 当前进程不是要过继的进程
 */
static bool init_adopt_achild(struct list_elem *pelem, int32_t pid)
{
    task_status_t *pcb = elem2entry(task_status_t, all_list_tag, pelem);
    if (pcb->parent_pid == pid)
        pcb->parent_pid = 1;
    return false;
}

/**
 * @brief sys_wait是wait系统调用的实现函数. 用于让父进程等待子进程调用exit退出, 并将子进程的返回值保存到status中
 *
 * @param status 子进程的退出状态, 输出参数
 * @return pid_t 若等待成功, 则返回子进程的pid; 若等待失败, 则返回-1
 */
int16_t sys_wait(int32_t *status)
{
    task_status_t *parent_pcb = running_thread();

    while (1)
    {
        // 检测是否已经有运行完毕的子进程
        list_elem_t *child_elem = list_traversal(&thread_all_list, find_hanging_child, parent_pcb->pid);

        // 若有挂起(运行结束)的子进程
        if (child_elem != NULL)
        {
            task_status_t *child_pcb = elem2entry(task_status_t, all_list_tag, child_elem);
            *status = child_pcb->status;
            uint16_t child_pid = child_pcb->pid;
            // 释放子进程的PCB, 页目录表
            thread_exit(child_pcb, false);

            // 返回子进程pid
            return child_pid;
        }

        // 判断是否有子进程
        child_elem = list_traversal(&thread_all_list, find_child, parent_pcb->pid);
        if (child_elem == NULL)
            return -1;
        // 仍有子进程运行, 此时阻塞父进程
        thread_block(TASK_WAITING);
    }
}

/**
 * @brief sys_exit是exit系统调用的实现函数. 用于主动结束调用的进程
 */
void sys_exit(int32_t status)
{
    task_status_t *child_pcb = running_thread();
    child_pcb->exit_status = status;
    if (child_pcb->parent_pid == -1)
        PANIC("sys_exit: child_pcb->parent_pid is -1\n");

    // 把child_thread的所有子进程过继给init
    list_traversal(&thread_all_list, init_adopt_achild, child_pcb->pid);

    // 回收进程资源
    release_prog_resource(child_pcb);

    // 唤醒
    task_status_t *parent_pcb = pid2thread(child_pcb->parent_pid);

    if (parent_pcb->status == TASK_WAITING)
        thread_unblock(parent_pcb);

    // 线程把自己挂起，等待父进程回收pcb
    thread_block(TASK_HANGING);
}