#include "pipe.h"
#include "memory.h"
#include "fs.h"
#include "file.h"
#include "ioqueue.h"
#include "thread.h"

void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd)
{
    task_status_t *pcb = running_thread();

    if (new_local_fd < 3)
        pcb->fd_table[old_local_fd] = new_local_fd;
    else
    {
        uint32_t new_global_fd = pcb->fd_table[new_local_fd];
        pcb->fd_table[old_local_fd] = new_global_fd;
    }
}

// 判断文件描述符fd是否指向管道
bool is_pipe(uint32_t fd)
{
    uint32_t file_table_idx = fd_local2global(fd);
    return file_table[file_table_idx].fd_flag == PIPE_FLAG;
}

/**
 * @brief  创建管道。 我们在内核空间申请4kb作为管道，我们使用环形队列来控制这片区域
 *         该区域有被一个文件结构指出，生成的两个文件描述符指向这个文件结构
 *          在队列头写入数据，队列尾部读出数据
 * @param pipe_fd 输出参数，存储文件描述符fd[0]用于读管道， fd[1]用于写管道
 * @return int32_t 成功返回 0 失败返回 -1
 */
int32_t sys_pipe(int32_t pipe_fd[2])
{
    // 得到文件表的idx
    int32_t global_fd = get_free_slot_in_global();

    // 申请一页内核内存做环形缓冲区
    file_table[global_fd].fd_inode = get_kernel_pages(1);
    if (file_table[global_fd].fd_inode == NULL)
        return -1;

    // 初始化环形缓存区
    ioqueue_init((struct ioqueue *)file_table[global_fd].fd_inode);

    // 将fd_flag复用管道标志，标记该内存属于管道
    file_table[global_fd].fd_flag = PIPE_FLAG;

    // 把fd_pos复用为管道打开数
    file_table[global_fd].fd_pos = 2;
    pipe_fd[0] = pcb_fd_install(global_fd);
    pipe_fd[1] = pcb_fd_install(global_fd);
    return 0;
}

// 从管道中读数据
uint32_t pipe_read(int32_t fd, void *buf, uint32_t count)
{
    char *buffer = buf;
    uint32_t bytes_read = 0;
    // 把文件描述符转化成文件表下标
    uint32_t global_fd = fd_local2global(fd);

    // 获取管道的环形缓存区
    struct ioqueue *ioq = (struct ioqueue *)file_table[global_fd].fd_inode;

    // 选择较小的数据读取量，避免阻塞
    uint32_t ioq_len = ioq_length(ioq);
    uint32_t size = ioq_len > count ? count : ioq_len;
    while (bytes_read < size)
    {
        *buffer = ioq_getchar(ioq);
        bytes_read++;
        buffer++;
    }

    return bytes_read;
}

// 给管道写入数据
uint32_t pipe_write(int32_t fd, const void *buf, uint32_t count)
{
    uint32_t bytes_write = 0;
    // 得到文件表下标
    uint32_t global_fd = fd_local2global(fd);
    struct ioqueue *ioq = (struct ioqueue *)file_table[global_fd].fd_inode;

    // 选择较小的数据写入量，避免阻塞
    uint32_t ioq_left = bufsize - ioq_length(ioq);
    uint32_t size = ioq_left > count ? count : ioq_left;

    const char *buffer = buf;
    while (bytes_write < size)
    {
        ioq_putchar(ioq, *buffer);
        buffer++;
        bytes_write++;
    }
    return bytes_write;
}
