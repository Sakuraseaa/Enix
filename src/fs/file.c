#include "file.h"
#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "stdio-kernel.h"
#include "memory.h"
#include "debug.h"
#include "interrupt.h"
#include "string.h"
#include "thread.h"
#include "global.h"

// 文件表
struct file file_table[MAX_FILE_OPEN];

/**
 * @brief get_free_slot_in_global用于从全局文件表中找到一个空位
 *
 * @return int32_t 若文件表中有空闲的, 则返回空闲的文件表的索引; （要存在文件描述符数组）
 *          若无空闲的位置, 则返回-1
 */
int32_t get_free_slot_in_global(void)
{
    uint32_t fd_idx = 3;
    while (fd_idx < MAX_FILE_OPEN)
    {
        if (file_table[fd_idx].fd_inode == NULL) // 找到了空位置
            break;

        fd_idx++;
    }
    if (fd_idx == MAX_FILE_OPEN)
    {
        printk("exceed max open files\n");
        return -1;
    }
    return fd_idx;
}

/**
 * @brief pcb_fd_install用于将全局文件表索引安装到用户进程的文件描述符数组中
 *
 * @param globa_fd_idx 要安装的全局文件描述符表
 * @return int32_t 若成功安装到用户文件描述符数组中, 则返回用户文件描述符数组的索引(文件描述符), 若失败则返回-1
 */
int32_t pcb_fd_install(int32_t global_fd_idx)
{
    struct task_struct *cur = running_thread();
    uint8_t local_fd_idx = 3; // 跨过stdin stdout stderr

    while (local_fd_idx < MAX_FILES_OPEN_PER_PROC)
    {
        if (cur->fd_table[local_fd_idx] == -1)
        {
            cur->fd_table[local_fd_idx] = global_fd_idx;
            break;
        }
        local_fd_idx++;
    }

    if (local_fd_idx == MAX_FILES_OPEN_PER_PROC)
    {
        printk("exceed max open files_per_proc\n");
        return -1;
    }

    return local_fd_idx;
}

/**
 * @brief inode_bitmap_alloc 在inode位图中设置一位为1，分配一个inode, 修改的是内存的inode位图，没有同步
 *      到硬盘
 * @param part 需要分配inode的分区
 * @return int32_t 若分配成功,得到inode表的index; 若分配失败, 则返回-1
 */
int32_t inode_bitmap_alloc(struct partition *part)
{
    int32_t bit_idx = bitmap_scan(&part->inode_bitmap, 1);
    if (bit_idx == -1)
        return -1;
    bitmap_set(&part->inode_bitmap, bit_idx, 1);
    return bit_idx;
}

/**
 * @brief block_bitmap_alloc 用于从partition指向的分区中分配一个block. 注意, 该函数只会修改内存中的block_bitmap,
 *        而不会修改物理磁盘中partition中的block bitmap
 *
 * @param part 需要分配 空闲块 的分区
 * @return int32_t 若分配成功, 得到的是 被分配的扇区 的扇区号(lba扇区地址); 若分配失败, 则返回-1
 */
int32_t block_bitmap_alloc(struct partition *part)
{
    int32_t bit_idx = bitmap_scan(&part->block_bitmap, 1);
    if (bit_idx == -1)
    {
        return -1;
    }
    bitmap_set(&part->block_bitmap, bit_idx, 1);

    return (part->sb->data_start_lba + bit_idx);
}

/**
 * @brief file_create用于在parent_dir执行的目录中创建一个名为filename的一般文件. 注意, 创建好的文件默认处于打开状态
 *        因此创建的文件对应的inode会被插入到current_partition.open_inode_list,
 *        而且还会把刚打开的文件添加到系统的文件描述符表（file_table）中
 *
 * @details 创建一个普通文件, 需要如下的几步:
 *              1. 在当前分区中申请一个inode, 此时首先需要向inode_bitmap中申请获得一个inode号, 用来操作inode_table
 *
 *              2. 申请得到inode之后, 需要填充inode基础信息
 *
 *              3. 创建文件之后, 需要向文件所在的父目录中, 插入文件的目录项
 *
 *              4. 因为前面操作的inode_bitmap, block_bitmap，目录等都是在内存中的, 因此需要将内存中数据持久化到硬盘中
 *
 *              5. 如果中间哪步出错, 那么申请的资源一定释放, 即inode_bitmap或者block_bitmap中修改的位需要修改回来
 *
 *
 * @param parent_dir 需要创建文件所在的目录
 * @param filename 需要创建的文件的名字
 * @param flag 需要创建的文件的读写属性
 * @return int32_t 若成功, 则返回文件描述符; 若失败, 则返回-1
 *
 */
int32_t file_create(struct dir *parent_dir, char *filename, uint8_t flag)
{
    // 创建一个缓冲区， 用于稍后与磁盘的io
    void *io_buf;
    if ((io_buf = sys_malloc(1024)) == NULL)
    {
        printk("in file_creat: sys_malloc for io_buf failed\n");
        return -1;
    }

    // 用于操作失败时回滚各资源状态
    uint8_t rollback_step = 0;
    // 为新文件分配 inode, 此步得到了inode表里面索引 inode_no
    int32_t inode_no = inode_bitmap_alloc(cur_part);
    if (inode_no == -1)
    {
        printk("in file_creat: allocate inode failed\n");
        return -1;
    }

    /* 此inode要从堆中申请内存,不可生成局部变量(函数退出时会释放)
     * 因为file_table数组中的文件描述符的inode指针要指向它.*/
    // 此处修改PCB为了使得inode申请在内核堆空间，这样inode队列就可以共享给每个进程使用了，况且inode这样的内核数据理应由内核管理
    struct task_struct *cur = running_thread();
    uint32_t user_pgdir_bk = (uint32_t)cur->pgdir;
    cur->pgdir = NULL;
    struct inode *new_file_inode = (struct inode *)sys_malloc(sizeof(struct inode));
    cur->pgdir = (uint32_t *)user_pgdir_bk;
    if (new_file_inode == NULL)
    {
        printk("file_create: sys_malloc for inode failded\n");
        rollback_step = 1;
        goto rollback;
    }
    inode_init(inode_no, new_file_inode); // 初始化i结点

    /* 返回的是filew文件的下标，这一步是新建立inode关联到file_table文件表 */
    // 为什么这样做？ 我其实不知道，但是我猜因为刚创建文件默认打开
    int fd_idx = get_free_slot_in_global();
    if (fd_idx == -1)
    {
        printk("exceed max open files\n");
        rollback_step = 2;
        goto rollback;
    }
    // 新创建的文件默认打开，则为新文件注册文件描述符
    file_table[fd_idx].fd_inode = new_file_inode;
    file_table[fd_idx].fd_pos = 0;
    file_table[fd_idx].fd_flag = flag;
    file_table[fd_idx].fd_inode->write_deny = false;

    // 创造 新建文件的目录项
    struct dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct dir_entry));
    // 对目录项进行初始化
    create_dir_entry(filename, inode_no, FT_REGULAR, &new_dir_entry);

    // 上面的操作都发送在内存里面，下面把inode_bitmap, inode, 目录的数据块 ，目录的inode 持久化到硬盘
    /* a 在目录parent_dir下安装目录项new_dir_entry, 写入硬盘后返回true,否则false */
    if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf))
    {
        printk("sync dir_entry to disk failed\n");
        rollback_step = 3;
        goto rollback;
    }

    // b 把父目录 i 结点的内容同步到硬盘
    memset(io_buf, 0, 1024);
    inode_sync(cur_part, parent_dir->inode, io_buf);

    // c 把新创建文件的 i 结点内容同步到硬盘
    memset(io_buf, 0, 1024);
    inode_sync(cur_part, new_file_inode, io_buf);

    // d 将inode_bitmap位图同步到硬盘
    bitmap_sync(cur_part, inode_no, INODE_BITMAP);

    // e 将创建的文件i结点添加到 open_inodes链表中
    list_push(&cur_part->open_inodes, &new_file_inode->inode_tag);
    new_file_inode->i_open_cnts = 1;

    sys_free(io_buf);
    return pcb_fd_install(fd_idx);

rollback:
    // 释放资源是依次释放的, 申请的顺序是:
    //      1. inode_bitmap
    //      2. 内核线程堆中的inode内存
    //      3. 系统全局的open_file_table中的一个free slot
    // 所以释放资源的时候, 倒序释放
    switch (rollback_step)
    {
    case 3:
        /* 失败时,将file_table中的相应位清空 */
        memset(&file_table[fd_idx], 0, sizeof(struct file));
    case 2:
        cur->pgdir = NULL;
        sys_free(new_file_inode);
        cur->pgdir = (uint32_t *)user_pgdir_bk;
    case 1:
        /* 如果新文件的i结点创建失败,之前位图中分配的inode_no也要恢复 */
        bitmap_set(&cur_part->inode_bitmap, inode_no, 0);
        break;
    }
    sys_free(io_buf);
    return -1;
}

/**
 * @brief file_open打开 inode_no号的文件
 *         a. 把文件注册在全局文件表里面
 *         b. 把文件inode加载入内存（第一次打开）
 *         b.1 检测重复写问题
 *         c. 把文件表下标 记录 在进程自己的文件描述符表中
 *
 * @param inode_no 需要打开文件的inode号
 * @param flag  打开方式
 *
 * @return 文件描述符
 */
int32_t file_open(uint32_t inode_no, uint8_t flag)
{
    int fd_idx = get_free_slot_in_global();
    if (fd_idx == -1)
    {
        printk("exceed max open files\n");
        return -1;
    }
    file_table[fd_idx].fd_inode = inode_open(cur_part, inode_no);
    // 每次打开文件， 把 fd_pos置为0,让文件内的指针指向开头
    file_table[fd_idx].fd_pos = 0;
    file_table[fd_idx].fd_flag = flag;

    // 检测文件是否要重复写
    bool *write_deny = &file_table[fd_idx].fd_inode->write_deny;
    if ((flag & O_WRONLY) || (flag & O_RDWR))
    {
        // 只要关于写文件, 判断是否有其他进程写此文件.若是读文件，不考虑 write_deny
        // 以下进入临界区前先关中断
        enum intr_status old_status = intr_disable();
        if (!(*write_deny))
        {
            // 没有人写
            *write_deny = true;          // 标记我这个进程要写了
            intr_set_status(old_status); // 恢复中断
        }
        else
        { // 已经有人写了
            intr_set_status(old_status);
            printk("file cant's be write now, try again later\n");
            return -1;
        }
    }

    // 在进程空间注册自己的 fd_idx
    return pcb_fd_install(fd_idx);
}

/**
 * @brief file_close关闭文件
 *         a. 是否inode内存
 *         b. 标记自己空闲
 *         c. write_deny = false
 *
 * @param file 全局文件表元素
 * @return 成功返回0， 失败返回 -1
 */
int32_t file_close(struct file *file)
{
    if (file == NULL)
        return -1;
    file->fd_inode->write_deny = false;
    inode_close(file->fd_inode);
    file->fd_inode = NULL; // 使文件结构可用
    return 0;
}

/**
 * @brief bitmap_sync用于将内存中的bitmap的bit_idx位的值同步到（硬盘中）partition指向的分区中
 *      硬盘中的位图需要修改的位置，我们可以用 bit_idx / 4096 来确定对于硬盘中块位图lba的偏移(单位:扇区) A
 *      内存中的偏移需要用字节来衡量，把上面 (bit_idx / 4096) * 512就是对于内存中块位图的字节偏移 B
 *      把内存中的B位置直接写入硬盘中的A位置，就完成了块位图的同步
 * @param partition 要写入的bitmap所在的分区
 * @param bit_idx 要写入的位
 * @param btmp 要写入的位图的标志（inode_bitmap 或 block_bitmap）
 */
void bitmap_sync(struct partition *partition, uint32_t bit_idx, uint8_t btmp)
{
    // off_sec是要写入的位(bit_idx)相对于parition->sb->block_bitmap_lba或者partition->sb->inode_bitmap_lba的扇区偏移数
    uint32_t off_sec = bit_idx / 4096;
    // off_size是要写入的位(bit_idx)相对于parition->block_bitmap.bits或者partition->inode_bitmap.bits的字节偏移数
    uint32_t off_size = off_sec * BLOCK_SIZE;

    uint32_t sec_lba;    // 需要同步位图的某一扇区号
    uint8_t *bitmap_off; // 扇区内的偏移

    switch (btmp)
    {
    case INODE_BITMAP:
        sec_lba = partition->sb->inode_bitmap_lba + off_sec;
        bitmap_off = partition->inode_bitmap.bits + off_size;
        break;
    case BLOCK_BITMAP:
        sec_lba = partition->sb->block_bitmap_lba + off_sec;
        bitmap_off = partition->block_bitmap.bits + off_size;
        break;
    default:
        PANIC("Invalid bitmap type");
        break;
    }

    ide_write(partition->my_disk, sec_lba, bitmap_off, 1);
}

/**
 * @brief file_write用于将buf中的count个字节写入到file指向的文件
 *
 * @param file 需要写入的文件描述符
 * @param buf 需要写入文件的数据
 * @param count 需要写入的字节数
 * @return int32_t 若写入成功则返回写入的字节数; 若失败则返回-1
 */
int32_t file_write(struct file *file, const void *buf, uint32_t count)
{
    // a. 判断是否会写超
    //  目前inode的13个数据块中前12个是直接数据块, 第13个是一个一级数据块, 所以目前最大支持写入的字节数是max_byte
    uint32_t max_bytes = (12 + 512 / 4) * BLOCK_SIZE;
    if (file->fd_inode->i_size + count > max_bytes)
    {
        printk("file_write: exceed maximum of file %d bytes, trying to make a file %d bytes", max_bytes, file->fd_inode->i_size + count);
        return -1;
    }

    uint32_t *all_blocks = (uint32_t *)sys_malloc(BLOCK_SIZE + 48); // 用来记录文件数据块的地址
    if (all_blocks == NULL)
    {
        printk("file_write: sys_malloc for all_blocks failed\n");
        return -1;
    }
    int32_t block_lba = -1;        // 块地址,lba地址
    uint32_t block_bitmap_idx = 0; // 记录空闲块对应于block_bitmap中的索引,做为参数传给bitmap_sync
    int32_t indirect_block_table;  // 用来获取一级间接表地址
    uint32_t block_idx;            // 块索引

    /* b.判断文件是否是第一次写,如果是,先为其分配一个块，这里是否可以用文件Size == 0 来判断 */
    if (file->fd_inode->i_sectors[0] == 0)
    {
        block_lba = block_bitmap_alloc(cur_part);
        if (block_lba == -1)
        {
            printk("file_write: block_bitmap_alloc failed\n");
            return -1;
        }
        file->fd_inode->i_sectors[0] = block_lba;

        /* 把分配一个块就将位图同步到硬盘 */
        block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
        ASSERT(block_bitmap_idx != 0);
        bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
    }
    // 未写入时， 文件占用的块数 + 1
    uint32_t file_has_used_blocks = file->fd_inode->i_size / BLOCK_SIZE + 1;
    // 写入后， 文件占用的块数 + 1
    uint32_t file_will_use_blocks = (file->fd_inode->i_size + count) / BLOCK_SIZE + 1;
    ASSERT(file_will_use_blocks <= 140);
    /* 通过此增量判断是否需要分配扇区,如增量为0,表示原扇区够用 */
    uint32_t add_blocks = file_will_use_blocks - file_has_used_blocks;

    // c.把可能用到的块地址收集到all_blocks,(系统中块大小等于扇区大小)

    if (add_blocks == 0)
    {
        // 在同一扇区内写入数据,不涉及到分配新扇区
        if (file_has_used_blocks <= 12) // 文件数据量将在12块之内
        {
            block_idx = file_has_used_blocks - 1; // 文件数据量将在12块之内
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
        }
        else
        {
            /* 未写入新数据之前已经占用了间接块,需要将间接块地址读进来 */
            ASSERT(file->fd_inode->i_sectors[12] != 0);
            indirect_block_table = file->fd_inode->i_sectors[12];
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
    }
    else
    {
        // 需要分配新块, 有三种情况

        /* 第一种情况:12个直接块地址 够分配使用*/
        if (file_will_use_blocks <= 12)
        {
            // 1. 先将有剩余空间的可继续用的扇区地址写入all_blocks
            block_idx = file_has_used_blocks - 1;
            ASSERT(file->fd_inode->i_sectors[block_idx] != 0);
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];

            /*2. 再将未来要用的扇区分配好后写入all_blocks */
            block_idx = file_has_used_blocks; // 指向第一个要分配的新扇区
            while (block_idx < file_will_use_blocks)
            {
                block_lba = block_bitmap_alloc(cur_part);
                if (block_lba == -1)
                {
                    printk("file_write: block_bitmap_alloc for situation 1 failed\n");
                    return -1;
                }

                // 写文件时,不应该存在块未使用但已经分配扇区的情况,当文件删除时,就会把块地址清0
                ASSERT(file->fd_inode->i_sectors[block_idx] == 0); // 确保尚未分配扇区地址
                file->fd_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;

                // 分配一个块，就同步一次块位图到硬盘
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

                block_idx++; // 下一个分配的新扇区
            }
        }
        else if (file_has_used_blocks <= 12 && file_will_use_blocks > 12)
        {
            /* 第二种情况: 旧数据在12个直接块内,新数据将使用间接块, 简介块未分配*/

            // 1.先将有剩余空间的可继续用的扇区地址收集到all_blocks
            block_idx = file_has_used_blocks - 1; // 指向旧数据所在的最后一个扇区
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];

            // 2.创建一级间接地址表
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1)
            {
                printk("file_write: block_bitmap_alloc for indirect_table situation 2 failed\n ");
                return -1;
            }
            ASSERT(file->fd_inode->i_sectors[12] == 0); // 确保一级间接块表未分配
            // 分配一级间接块索引
            indirect_block_table = file->fd_inode->i_sectors[12] = block_lba;

            // 3. 申请块，存储地址
            block_idx = file_has_used_blocks; // 第一个未使用的块,即本文件最后一个已经使用的直接块的下一块
            while (block_idx < file_will_use_blocks)
            {
                block_lba = block_bitmap_alloc(cur_part);
                if (block_lba == -1)
                {
                    printk("file_write: block_bitmap_alloc for situation 2 failed\n");
                    return -1;
                }

                if (block_idx < 12)
                {
                    // 新创建的0~11块直接存入all_blocks数组
                    ASSERT(file->fd_inode->i_sectors[block_idx] == 0); // 确保尚未分配扇区地址
                    file->fd_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;
                }
                else
                {
                    // 简介块会直接同步到硬盘
                    all_blocks[block_idx] = block_lba;
                }

                /* 每分配一个块就将位图同步到硬盘 */
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
                block_idx++; // 下一个新扇区
            }
            ide_write(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
        else if (file_has_used_blocks > 12)
        {
            /* 第三种情况:新数据占据间接块*/

            ASSERT(file->fd_inode->i_sectors[12] != 0);           // 已经具备了一级间接块表
            indirect_block_table = file->fd_inode->i_sectors[12]; // 获取一级间接表地址

            // 1.已使用的间接块也将被读入all_blocks
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);

            // 2. 申请数据块
            block_idx = file_has_used_blocks; // 第一个未使用的间接块,即已经使用的间接块的下一块
            while (block_idx < file_will_use_blocks)
            {
                block_lba = block_bitmap_alloc(cur_part);
                if (block_lba == -1)
                {
                    printk("file_write: block_bitmap_alloc for situation 3 failed\n");
                    return -1;
                }
                all_blocks[block_idx++] = block_lba;

                /* 每分配一个块就将空闲块位图同步到硬盘 */
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
            }

            // 3. 同步间接表
            ide_write(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
    }

    // d. 把buf写入硬盘
    uint8_t *io_buf = sys_malloc(BLOCK_SIZE); //  写硬盘的缓冲区
    if (io_buf == NULL)
    {
        printk("file_write: sys_malloc for io_buf failed\n");
        return -1;
    }
    bool first_write_block = true; // 含有剩余空间的扇区标识

    file->fd_pos = file->fd_inode->i_size - 1; // 置fd_pos为文件大小-1,下面在写数据时随时更新
                                               // 每次都是追加写？，这样有些功能无法实现吧

    const uint8_t *src = buf;   // src 指向 buf中带写入的数据
    uint32_t bytes_written = 0; // 用来记录已写入数据大小
    uint32_t size_left = count; // 记录未写入数据大小
    uint32_t sec_idx;           // 用来索引扇区
    uint32_t sec_lba;           // 扇区地址
    uint32_t sec_off_bytes;     // 扇区内字节偏移量
    uint32_t sec_left_bytes;    // 扇区内剩余空闲字节量
    uint32_t chunk_size;        // 每次写入硬盘的字节数量
    while (bytes_written < count)
    {
        memset(io_buf, 0, BLOCK_SIZE);
        sec_idx = file->fd_inode->i_size / BLOCK_SIZE; // 追加写
        sec_lba = all_blocks[sec_idx];                 //// 本次ide_write写入的磁盘块的地址

        // file->fd_inode->i_size表示文件目前的大小,所以整除之后得到的值就是表示上一次写到最后一个块的哪
        // 所以, sec_off_bytes主要给第一次接着写入没有写完的块用的, 后面因为每次都是写入的整个块
        // 所以第二次写入块的时候, sec_off_bytes就是0, 表示本次写入块从哪里开始写
        sec_off_bytes = file->fd_inode->i_size % BLOCK_SIZE;
        sec_left_bytes = 512 - sec_off_bytes;

        // 判断此次写入的数据的大小，还是第一次会有区别
        chunk_size = size_left < sec_left_bytes ? size_left : sec_left_bytes;

        // 第一次写的时候，需要把最后一个扇区从磁盘读出来，写在它的后面
        if (first_write_block)
        {
            ide_read(cur_part->my_disk, sec_lba, io_buf, 1);
            first_write_block = false;
        }

        // 写入缓冲区，由缓冲区写入硬盘
        memcpy(io_buf + sec_off_bytes, src, chunk_size);
        ide_write(cur_part->my_disk, sec_lba, io_buf, 1);

        // printk("file_write:data write at 0x%x\n", sec_lba);

        // 准备下一轮数据
        src += chunk_size;
        file->fd_inode->i_size += chunk_size;
        file->fd_pos += chunk_size;
        bytes_written += chunk_size;
        size_left -= chunk_size;
    }

    // e. 同步文件的inode
    void *inode_buf = sys_malloc(BLOCK_SIZE * 2);
    if (inode_buf == NULL)
    {
        printk("%s: sys_malloc for inode_buf failed!\n", __func__);
        printk("%s: inode failed sync to disk!!\n", __func__);
    }
    else
    {
        inode_sync(cur_part, file->fd_inode, inode_buf);
        sys_free(inode_buf);
    }

    // 释放缓冲区
    sys_free(all_blocks);
    sys_free(io_buf);
    return bytes_written;
}

/**
 * @description: file_read会从file->inode从读入count个字节存到buf
 * @param file* file 需要读的文件结构
 * @param void* buf  存放读出数据的内存
 * @param uint32_t count 需要读出的字节数
 * @return {*}
 */
int32_t file_read(struct file *file, void *buf, uint32_t count)
{
    uint8_t *buf_dst = (uint8_t *)buf;
    uint32_t size = count, size_left = size; // size需要读出的字节数, size_left剩余读的字节数

    // 如果要读取的字节大于文件剩余的字节, 则读取剩余全部字节
    if ((file->fd_pos + count) > file->fd_inode->i_size)
    {
        size_left = size = file->fd_inode->i_size - file->fd_pos;
        if (size == 0)
        {
            // 已无内容可读(到答文件尾) 则直接返回
            return -1;
        }
    }

    uint8_t *io_buf = (uint8_t *)sys_malloc(BLOCK_SIZE);
    if (io_buf == NULL)
    {
        printk("%s: sys_malloc for io_buf failed!\n", __func__);
        return -1;
    }

    uint32_t *all_blocks = (uint32_t *)sys_malloc(BLOCK_SIZE + 48);
    if (all_blocks == NULL)
    {
        printk("%s: sys_malloc for io_buf failed\n", __func__);
        return -1;
    }

    uint32_t block_read_start_idx = file->fd_pos / BLOCK_SIZE;        // 数据块起始地址
    uint32_t block_read_end_idx = (file->fd_pos + size) / BLOCK_SIZE; // 数据块的终止地址

    uint32_t read_block = block_read_start_idx - block_read_end_idx; // 需要读数据所占的扇区数
    ASSERT(block_read_start_idx < 139 && block_read_end_idx < 139);

    int32_t indirect_block_table; // 用来获取一级间接表地址
    uint32_t block_idx;           // 获取待读的块地址

    // 构建all_blocks块地址数组,读文件所属的得到要读数据的地址
    if (read_block == 0)
    {
        // 在一扇区内读数据, 不涉及跨扇区
        ASSERT(block_read_end_idx == block_read_start_idx)
        if (block_read_end_idx < 12)
        { // 在12个直接块之内
            block_idx = block_read_end_idx;
            all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
        }
        else
        { // 将一级简介块存储的块地址存入all_blocks
            indirect_block_table = file->fd_inode->i_sectors[12];
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
    }
    else
    {
        // 需要跨扇区读取数据

        // 情况1 起始块和终止块属于直接块, 数据结束所在的块属于直接块
        if (block_read_end_idx < 12)
        {
            block_idx = block_read_start_idx;
            while (block_idx <= block_read_end_idx)
            {
                all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
                block_idx++;
            }
        }
        // 情况2 待入读数据跨越直接快和间接块两类, 所以两种地址都需要存储
        else if (block_read_start_idx < 12 && block_read_end_idx >= 12)
        { // a. 首先记录直接块的地址在all_blocks中
            block_idx = block_read_start_idx;
            while (block_idx < 12)
            {
                all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
                block_idx++;
            }
            ASSERT(file->fd_inode->i_sectors[12] != 0);

            // b. 再记录一级间接地址在all_blocks中
            indirect_block_table = file->fd_inode->i_sectors[12];
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);

        } // 情况3 需要读入的数据块的地址是间接地址
        else
        {
            // 确保存在间接块
            ASSERT(file->fd_inode->i_sectors[12] != 0);

            indirect_block_table = file->fd_inode->i_sectors[12];
            ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
        }
    }

    // 地址已经获取得到, 开始读数据
    // 用来索引扇区, 扇区地址, 扇区内字节偏移量, 扇区内剩余字节量, 每次从硬盘读入的字节数量
    uint32_t sec_idx, sec_lba, sec_off_bytes, sec_left_bytes, chunk_size;
    uint32_t bytes_read = 0; // 已读入字节数

    // 和写文件十分相似
    while (bytes_read < size)
    {
        sec_idx = file->fd_pos / BLOCK_SIZE;
        sec_lba = all_blocks[sec_idx];

        sec_off_bytes = file->fd_pos % BLOCK_SIZE;
        sec_left_bytes = 512 - sec_off_bytes;

        chunk_size = size_left < sec_left_bytes ? size_left : sec_left_bytes;

        memset(io_buf, 0, BLOCK_SIZE);
        ide_read(cur_part->my_disk, sec_lba, io_buf, 1);
        memcpy(buf_dst, io_buf + sec_off_bytes, chunk_size);

        buf_dst += chunk_size;
        file->fd_pos += chunk_size;
        bytes_read += chunk_size;
        size_left -= chunk_size;
    }

    sys_free(all_blocks);
    sys_free(io_buf);
    return bytes_read;
}