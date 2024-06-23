#include "fork.h"
#include "process.h"
#include "memory.h"
#include "interrupt.h"
#include "debug.h"
#include "thread.h"
#include "string.h"
#include "file.h"
#include "pipe.h"
extern void intr_exit(void);

/**
 * @brief 1. 复制父进程PCB，内核栈4KB到子进程PCB的地址
 *        2. 复制父进程的虚拟位图， 到子进程
 * @param child_thread 子进程PCB
 * @param parent_thread 父进程PCB
 * @return int32_t 成功返回0 失败返回-1
 */
static int32_t copy_pcb_vaddrbitmap_stack0(task_status_t *child_thread, task_status_t *parent_thread)
{
    // 复制PCB整页，包括内核线程的tcb和内核栈
    memcpy(child_thread, parent_thread, PG_SIZE);

    // 单独修改子进程信息
    child_thread->pid = fork_pid();
    child_thread->elapsed_ticks = 0;
    child_thread->status = TASK_READY;
    child_thread->ticks = child_thread->priority;
    child_thread->parent_pid = parent_thread->pid;
    child_thread->general_tag.prev = child_thread->general_tag.next = NULL;
    child_thread->all_list_tag.prev = child_thread->all_list_tag.next = NULL;
    // 初始化子进程的内存块描述符，（空闲块链表），管理的是进程的堆
    block_desc_init(child_thread->u_block_desc);

    // 复制父进程虚拟地址池的位图, 因为每个进程的虚拟内存都是独立的, 所以需要单独复制
    // 复制父进程的内存池的位图
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000 - USER_VADDR_START) / PG_SIZE / 8, PG_SIZE); // 计算虚拟地址位图需要的页框数
    void *vaddr_btmp = get_kernel_pages(bitmap_pg_cnt);
    if (vaddr_btmp == NULL)
        return -1;

    // 因为上面直接复制了整个父进程PCB, 所以child_thread->userprog_vaddr依旧指向父进程的bitmap
    // 这里逐字节的复制, 然后再转换下类型
    memcpy(vaddr_btmp, child_thread->userprog_vaddr.vaddr_bitmap.bits, bitmap_pg_cnt * PG_SIZE);

    child_thread->userprog_vaddr.vaddr_bitmap.bits = vaddr_btmp;

    // prepare for name
    ASSERT(strlen(child_thread->name) < 16);
    strcat(child_thread->name, "_fork");
    return 0;
}

/**
 * @brief 使用copy_pcb_vaddrbitmap_stack0完成了对虚拟内存池的复制（这是为了父进程和子进程对于虚拟地址的统一，相同），
 *        下面使用copy_body_stack3复制虚拟内存池对应的用户栈，代码，数据资源（给子进程真正的创建物理地址，填写页表）
 *
 *
 * @param child_thread 子进程pcb
 * @param parent_thread 父进程pcb
 * @param buf_page 内核用户区， 父进程的页面复制到内核 （切换子进程页表）由内核 复制到子进程（切换父进程页表），
 *                  因为进程之间是不能通信的，但所有进程都由内核空间
 */
static void copy_body_stack3(task_status_t *child_thread, task_status_t *parent_thread, void *buf_page)
{
    uint8_t *vaddr_btmp = parent_thread->userprog_vaddr.vaddr_bitmap.bits;               // 父进程位图地址
    uint32_t btmp_bytes_len = parent_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len; // 父进程位图长度

    uint32_t vaddr_start = parent_thread->userprog_vaddr.vaddr_start; // 父进程起始虚拟地址
    uint32_t idx_byte = 0;                                            // 字节
    uint32_t idx_bit = 0;                                             // 位
    uint32_t prog_vaddr = 0;

    // 逐字节遍历位图
    while (idx_byte < btmp_bytes_len)
    {
        if (vaddr_btmp[idx_byte])
        {
            // 逐位遍历位图
            idx_bit = 0;
            while (idx_bit < 8)
            {
                // 若父进程中的该虚拟页正在使用
                if ((BITMAP_MASK << idx_bit) & vaddr_btmp[idx_byte])
                {
                    // 计算当前页虚拟地址
                    prog_vaddr = (idx_byte * 8 + idx_bit) * PG_SIZE + vaddr_start;
                    // 将父进程该虚拟页复制到buf_page中
                    // buf_page必须是内核页, 因为不同进程的内核空间是共享的, 这样才能实现进程间的数据交换
                    memcpy(buf_page, (void *)prog_vaddr, PG_SIZE);

                    // 使用子进程的页目录, 此后操作的就是子进程的虚拟内存
                    page_dir_activate(child_thread);
                    // 从用户物理内存池中申请一个页，并映射页到子进程的页目录表中
                    get_a_page_without_opvaddrbitmap(PF_USER, prog_vaddr);
                    // 复制buf_page中的内容到子进程的页中
                    memcpy((void *)prog_vaddr, buf_page, PG_SIZE);
                    // 重新切换为父进程的虚拟页表
                    page_dir_activate(parent_thread);
                }
                idx_bit++;
            }
        }
        idx_byte++;
    }
}

/**
 * @brief 复制页表和页表项
 *
 * @param child_thread
 * @param parent_thread
 * @return int
 */
static void copy_page_tables(task_status_t *child_thread, task_status_t *parent_thread)
{
    uint8_t *vaddr_btmp = parent_thread->userprog_vaddr.vaddr_bitmap.bits;               // 父进程位图地址
    uint32_t btmp_bytes_len = parent_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len; // 父进程位图长度

    uint32_t vaddr_start = parent_thread->userprog_vaddr.vaddr_start; // 父进程起始虚拟地址
    uint32_t idx_byte = 0;                                            // 字节
    uint32_t idx_bit = 0;                                             // 位
    uint32_t prog_vaddr = 0;

    while (idx_byte < btmp_bytes_len) // 逐字节遍历位图
    {
        if (vaddr_btmp[idx_byte]) // 如果字节有内容
        {

            idx_bit = 0;
            while (idx_bit < 8) // 遍历位
            {
                if (vaddr_btmp[idx_byte] & (BITMAP_MASK << idx_bit)) // 确定该位有内容
                {
                    prog_vaddr = vaddr_start + (idx_byte * 8 + idx_bit) * PG_SIZE;
                    full_childProcess_pageTable(child_thread, parent_thread, prog_vaddr);
                }
                idx_bit++;
            }
        }
        idx_byte++;
    }
}

/**
 * @brief 为子进程构建thread_stack. 之所以要构建thread_stack是因为该函数作为fork系统调用一部分
 *        必然是用户调用系统调用, 则一定是发生了0x80软中断. 所以在返回的时候必然是经过intr_exit的
 *        用thread_stack来实现着一步, 设置子进程从中断退出所需要的资源
 *
 * @param child_thread
 * @return int32_t
 */
static int32_t build_child_stack(task_status_t *child_thread)
{
    // 获取用户进程的0级栈, 中断栈
    intr_stack_t *intr_0_stack = (intr_stack_t *)((uint32_t)child_thread + PG_SIZE - sizeof(intr_stack_t));
    // 子进程返回0
    intr_0_stack->eax = 0;

    // 为switch_to构建tread_stack
    uint32_t *ret_addr_in_thread_stack = (uint32_t *)intr_0_stack - 1; // 里面存储的eip, intr_exit中断退出函数

    uint32_t *esi_ptr_in_thread_stack = (uint32_t *)intr_0_stack - 2;
    uint32_t *edi_ptr_in_thread_stack = (uint32_t *)intr_0_stack - 3;
    uint32_t *ebx_ptr_in_thread_stack = (uint32_t *)intr_0_stack - 4;

    uint32_t *ebp_ptr_in_thread_stack = (uint32_t *)intr_0_stack - 5;

    // 填充thread_stack
    *ret_addr_in_thread_stack = (uint32_t)intr_exit;
    *ebp_ptr_in_thread_stack = *ebx_ptr_in_thread_stack = *edi_ptr_in_thread_stack = *esi_ptr_in_thread_stack = 0;

    // 把thread_stack的栈顶做为switch_to恢复数据时的栈顶
    child_thread->self_kstack = ebp_ptr_in_thread_stack;
    return 0;
}

/**
 * @brief 更新子进程pcb中fd_table所有打开文件的索引数
 *
 * @param pcb 需要更新的pcb
 */
static void update_inode_open_cnts(task_status_t *pcb)
{
    int32_t local_fd = 3, global_fd = 0;
    while (local_fd < MAX_FILES_OPEN_PER_PROC)
    {
        global_fd = pcb->fd_table[local_fd];
        ASSERT(global_fd < MAX_FILE_OPEN)
        if (global_fd != -1)
        {
            if (is_pipe(local_fd))
                file_table[global_fd].fd_pos++;
            else
                file_table[global_fd].fd_inode->i_open_cnts++;
        }
        local_fd++;
    }
}

/**
 * @brief copy_process用于将父进程中的信息复制给子进程
 *
 * @param child_thread 子进程
 * @param parent_thread 被复制的父进程
 * @return uint32_t 复制成功返回0; 复制失败返回-1
 */
static int32_t copy_process(task_status_t *child_thread, task_status_t *parent_thread)
{
    // a.分配内核缓存页
    // void *buf_page = get_kernel_pages(1);
    // if (buf_page == NULL)
    //     return -1;

    // b. 复制父进程的pcb（4kb大小）, 虚拟地址位图，内核栈给子进程
    if (copy_pcb_vaddrbitmap_stack0(child_thread, parent_thread) == 1)
        return -1;

    // c. 为子进程创建页表
    child_thread->pgdir = create_page_dir();
    if (child_thread->pgdir == NULL)
        return -1;

    // d.复制父进程的所有数据给子进程，实质是复制父进程的页表的每一张物理页（代码体， 用户栈， 堆等）？
    // 填充到子进程页表。
    // copy_body_stack3(child_thread, parent_thread, buf_page);
    copy_page_tables(child_thread, parent_thread);

    // e. 构建子进程的thread_stack 并且设置子进程返回值为0，这一步的目的是让子进程被调度后，从中断退出恢复(fork时的环境)
    // 因为fork()是内核提供的，在0x80号中断，是系统调用
    build_child_stack(child_thread);

    // f.更新文件计数
    update_inode_open_cnts(child_thread);

    //  mfree_page(PF_KERNEL, buf_page, 1);
    return 0;
}

/**
 * @brief sys_fork是fork系统调用的实现函数. 用于复制出来一个子进程. 子进程的数据和代码和父进程的数据代码一模一样
 *
 * @return pid_t 父进程返回子进程的pid; 子进程返回0
 */
pid_t sys_fork(void)
{
    task_status_t *parent_thread = running_thread();
    task_status_t *child_thread = get_kernel_pages(1);
    if (child_thread == NULL)
        return -1;

    // 必须关闭中断，而且父进程一定是进程
    ASSERT(INTR_OFF == intr_get_status() && parent_thread->pgdir != NULL);

    // 复制数据
    if (copy_process(child_thread, parent_thread) == -1)
        return -1;

    // 插入到就绪队列中
    ASSERT(!elem_find(&thread_ready_list, &child_thread->general_tag));
    list_append(&thread_ready_list, &child_thread->general_tag);
    ASSERT(!elem_find(&thread_all_list, &child_thread->all_list_tag))
    list_append(&thread_all_list, &child_thread->all_list_tag);

    return child_thread->pid;
}
