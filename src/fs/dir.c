#include "dir.h"
#include "stdint.h"
#include "inode.h"
#include "file.h"
#include "fs.h"
#include "stdio-kernel.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "string.h"
#include "interrupt.h"
#include "super_block.h"

// 在fs.c中声明
extern struct partition *cur_part;

struct dir root_dir;

/**
 * @brief open_root_dir用于打开分区partition中的根目录
 *
 * @param part 分区
 */
void open_root_dir(struct partition *part)
{
    root_dir.inode = inode_open(part, part->sb->root_inode_no);
    root_dir.dir_pos = 0;
}

/**
 * @brief dir_open 用于打开partition指向的分区上inode编号为inode_no的目录
 *
 * @param partition 指向需要打开的分区的指针
 * @param inode_no 需要打开的目录,在partition指向的分区的inode_table中的index
 */
struct dir *dir_open(struct partition *partition, uint32_t inode_no)
{
    struct dir *pdir = (struct dir *)sys_malloc(sizeof(struct dir));
    pdir->inode = inode_open(partition, inode_no);
    pdir->dir_pos = 0;
    return pdir;
}

/**
 * @brief dir_close 关闭目录dir
 *
 * @param dir 指向需要被关闭的目录 的结构体
 */
void dir_close(struct dir *dir)
{
    /*************      根目录不能关闭     ***************
     *1 根目录自打开后就不应该关闭,否则还需要再次open_root_dir();
     *2 root_dir所在的内存是低端1M之内,并非在堆中,free会出问题 */
    if (dir == &root_dir)
    {
        // 根目录，直接返回
        return;
    }
    inode_close(dir->inode);
    sys_free(dir);
}

/**
 * @brief create_dir_entry用于初始化p_de指向的目录项. 具体来说, 该函数将filename, inode_no以及file_type复制到
 *        p_de指向的目录项中
 *
 * @param filename 目录项的文件名
 * @param inode_no 目录项文件的inode编号
 * @param file_type 目录项该文件的类型
 * @param p_de 需要初始化的目录项
 */
void create_dir_entry(char *filename, uint32_t inode_no, uint8_t file_type, struct dir_entry *p_de)
{
    ASSERT(strlen(filename) <= MAX_FILE_NAME_LEN);

    // 初始化目录项
    memcpy(p_de->filename, filename, strlen(filename));
    p_de->i_no = inode_no;
    p_de->f_type = file_type;
}

/**
 * @brief search_dir_entry用于在partition指向的分区中pdir指向的目录中寻找名称为name的文件或者目录, 找到后将其目录项存入dir_e中
 *
 * @details 一个目录文件用一个inode存储, 一个inode中有12个直接块, 即12个一级文件数据块地址, 1个间接项,
 *          即二级文件数块地址，这里地址存放了128个一级文件数据块地址(一扇区
 *          因此存储一个文件, 最多用 12 + 128(512 / 4，一个扇区 512 字节, 一个地址 4 字节) = 140个块
 *          因此, 存储目录文件最多需要140个块. 而这140个块中, 每个块内包含的都是目录项, 所以逐块检查目录项.
 *          即逐个读取块, 然后检查其中的目录项
 *
 * @param partition 指向要寻找的文件或者目录在的扇区
 * @param dir 指向要寻找的文件或者目录在的父目录
 * @param name 要寻找的文件或者目录的名称
 * @param dir_e 存储寻找到的文件或者目录的目录项
 * @return true 若在dir指向的文件之找到了要寻找的目录或者文件, 则返回true
 * @return false 若在dir指向的文件之没有找到要寻找的目录或者文件, 则返回false
 */
bool search_dir_entry(struct partition *partition, struct dir *pdir, const char *name, struct dir_entry *dir_e)
{
    // 12个直接块地址大小+128个间接块地址大小,共560字节 */
    uint32_t *all_blocks = (uint32_t *)sys_malloc(48 + 512);
    if (all_blocks == NULL)
    {
        printk("search_dir_entry: sys_malloc for all_blocks failed");
        return false;
    }

    uint32_t block_idx = 0;
    while (block_idx < 12)
    {
        all_blocks[block_idx] = pdir->inode->i_sectors[block_idx];
        block_idx++;
    }

    if (pdir->inode->i_sectors[12] != 0)
    {
        // 若含有一级间接块表
        ide_read(partition->my_disk, pdir->inode->i_sectors[12], all_blocks + 12, 1);
    }
    /* 至此,all_blocks存储的是该文件或目录的所有扇区地址 */

    /* 写目录项的时候已保证目录项不跨扇区,
     * 这样读目录项时容易处理, 只申请容纳1个扇区的内存 */
    uint8_t *buf = (uint8_t *)sys_malloc(SECTOR_SIZE);
    struct dir_entry *p_de = (struct dir_entry *)buf;

    uint32_t dir_entry_size = partition->sb->dir_entry_size;
    // 1扇区内可容纳的目录项数量
    uint32_t dir_entry_cnt = SECTOR_SIZE / dir_entry_size;

    /**
     * @brief 一个目录文件数据块的地址用一个inode存储, 一个inode中有12个直接索引块, 1个间接索引表（128）,
     *     一个inode里面所有的文件数据块地址一共是140个,这140个lba地址已经存储在all_blocks里面,我们使用
     *      这些地址遍历每一个目录文件数据块，再数据块里面找出 name 文件
     */
    uint32_t block_cnt = 140;
    block_idx = 0;

    // 从文件数据块中查找目录项
    while (block_idx < block_cnt)
    {
        // 块地址为 0 时表示该块中无数据， 继续再其它块中找
        if (all_blocks[block_idx] == 0)
        {
            block_idx++;
            continue;
        }
        ide_read(partition->my_disk, all_blocks[block_idx], buf, 1);

        uint32_t dir_entry_idx = 0;
        // 遍历文件数据块中所有目录项
        while (dir_entry_idx < dir_entry_cnt)
        {
            if (!strcmp(p_de->filename, name))
            {
                memcpy(dir_e, p_de, dir_entry_size);
                sys_free(buf);
                sys_free(all_blocks);
                return true;
            }
            dir_entry_idx++;
            p_de++;
        }
        block_idx++;
        p_de = (struct dir_entry *)buf; // 此时p_de已经指向扇区内最后一个完整目录项了,需要恢复p_de指向为buf
        memset(buf, 0, SECTOR_SIZE);    // 将buf清0, 下次再用
    }

    sys_free(buf);
    sys_free(all_blocks);
    return false;
}

/**
 * @brief sync_dir_entry将p_de指向的目录项写入到其父目录中(parent_dir指向的目录), 并把内容持久化到硬盘
 *         // 写入的过程中会修改目录文件的大小，可能需要扩充文件，所以需要申请空闲块，修改空闲块位图
 *         // 关于位图与目录文件数据的修改是直接同步到硬盘的
 *
 * @details sync_dir_entry的具体流程就是首先从磁盘中读出来目录的inode到内存中, 根据inode,遍历整个目录，
 *          把文件的部分挨个读入内存，找出空的位置，把目录项写入，然后就完成了持久化到硬盘。
 *
 * @param parent_dir 指向目录项的父目录
 * @param p_de 指向需要写入到磁盘中的目录项
 * @param io_buf 因为每次ide_write都是整个扇区整个扇区写入的, 因此io_buf是由调用者提供的一个大于512字节的缓冲区. 用于ide_write的时候写入
 * @return true 同步成功
 * @return false 同步失败
 */
bool sync_dir_entry(struct dir *parent_dir, struct dir_entry *p_de, void *io_buf)
{
    struct inode *dir_inode = parent_dir->inode;            // 目录的inode
    uint32_t dir_size = dir_inode->i_size;                  // inode大小
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size; // 目录项大小
    // 因为是目录，目录里面只有目录项这种大小固定的元素， 按规则应该被整除
    ASSERT(dir_size % dir_entry_size == 0);

    uint32_t dir_entry_per_sec = (SECTOR_SIZE / dir_entry_size); // 一个扇区里存储目录项的理论最大数量

    int32_t block_lba = -1; // 将要存储目录项的lba地址, 初始化为0
    uint8_t block_idx = 0;
    uint32_t all_blocks_lba[140] = {0};

    // 注意, 这里不需要读出全部的140个块, 读出前12个直接块就可以直接开始搜索了
    // 前12个块是直接块, 所以直接复制lba地址即可
    while (block_idx < 12)
    {
        all_blocks_lba[block_idx] = dir_inode->i_sectors[block_idx];
        block_idx++;
    }

    // 注意:
    //      1. 一个文件(包括目录文件)最多占用140个块, 一个块512字节, 所以一个文件最大71680字节
    //         但是并不是所有文件(包括目录)文件会用完这140个块, 所以优先从前面的12个块开始搜索
    //         即直接开始搜索, 而非读取完目录项的140个块之后再开始检查. 当然如果12个块搜索结束，
    //         还没有找到合适的位置，那么还是需要填满all_blocks_lba数组的
    //
    //      2. 并且, 有可能存在一个情况, 就是目录文件本身可能一开始只用了第一个直接块. 那么在这个
    //         时候, 后面的11个直接块, 包括一级间接块里面其实都是没有记录块的地址的. 此时, 如果
    //         我们要向一个没有分配的块中写入的目录项的话, 还需要先分配一个块, 然后把该块的LBA地址
    //         写入到inode.i_sectors[0-11]或者inode.i_sectors[12]一级间接块中
    //      3. 最后, 因为有可能会删除文件, 所以目录文件中的目录项表并不是连续的, 所以得一个个检查

    // 综上, 下面这部分循找空余位置的代码, 逻辑就是从第一个直接块开始顺序检查
    // 如果:
    //      1. lba不为0, 说明这个文件数据块上有数据，遍历这个数据块，看看是否能找到空余空间，存储新的目录项
    //
    //
    //      2. 某个直接块的lba为0, 则表示前面的直接块都已经装满了目录项了, 此块空间未分配, 则此时需要重新分配一个快
    //         而后将需要写入的目录项写入到新增的块中
    //          2.1.a 若此时新分配的数据块，在前 11个直接块索引表中有空位, 则把数据块地址直接写入空位就好
    //          2.1.b 若前面的 12个直接块索引表刚好满了, 则需要建立一级间接索引块表。在第一次申请的lba地址建立间接表
    //              再申请一个块，作为文件数据块，而后将文件数据块的lba地址写入到一级间接表中, 持久化间接表
    //          2.1.c 若此时 13个数据块都已经填充满, 将数据块地址写入到间接表里面中， 持久化间接表
    //
    //          2.2 在新的内存块里面，写入目录项, 持久化到硬盘
    block_idx = 0;
    int32_t block_bitmap_idx = -1;
    // dir_e用来在io_buf中遍历目录项
    struct dir_entry *dir_e = (struct dir_entry *)io_buf;
    bool full_all_blocks_lba = true;
    while (block_idx < 140)
    { // 文件(包括目录)最大支持12个直接块+128个间接块＝140个块
        block_bitmap_idx = -1;
        if (full_all_blocks_lba && block_idx >= 12)
        {
            // 此时的情况是，遍历了12个文件数据块，还没有找到空闲位置，那么我们从补充整个间接块的地址到内存
            full_all_blocks_lba = false;
            ide_read(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks_lba + 12, 1);
        }
        // 情况 2 , 数据块没分配, 此时分配一个新的块, 然后将目录项写入其中
        if (all_blocks_lba[block_idx] == 0)
        {
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1)
            {
                printk("alloc block bitmap for sync_dir_entry failed\n");
                return false;
            }
            // 每分配一个块就同步一次block_bitmap
            block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            block_bitmap_idx = -1;
            if (block_idx < 12)
            { // 2.1.a 若为直接块
                dir_inode->i_sectors[block_idx] = all_blocks_lba[block_idx] = block_lba;
            }
            else if (block_idx == 12)
            {                                         // 2.1.b 若是尚未分配一级间接块表(block_idx等于12表示第0个间接块地址为0)
                dir_inode->i_sectors[12] = block_lba; // 将上面分配的块做为一级间接块表地址
                block_lba = -1;
                block_lba = block_bitmap_alloc(cur_part); // 再分配一个块做为第0个间接块
                if (block_lba == -1)
                {
                    // 申请内存失败，回滚操作
                    block_bitmap_idx = dir_inode->i_sectors[12] - cur_part->sb->data_start_lba; //(神来执笔)
                    bitmap_set(&cur_part->block_bitmap, block_bitmap_idx, 0);
                    bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
                    dir_inode->i_sectors[12] = 0;
                    printk("alloc block bitmap for sync_dir_entry failed\n");
                    return false;
                }

                // 持久化刚才申请的第0个间接块
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

                /* 把新分配的第0个间接块地址写入一级间接块表 */
                all_blocks_lba[12] = block_lba;
                ide_write(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks_lba + 12, 1);
            }
            else
            {
                //  2.1.c  间接块没有分配
                all_blocks_lba[block_idx] = block_lba;
                /* 把新分配的第[block_idx - 12]个间接块地址写入一级间接块表 */
                ide_write(cur_part->my_disk, dir_inode->i_sectors[12], all_blocks_lba + 12, 1);
            }
            //  2.1.c 将新目录项p_de写入新分配的文件数据块 */
            memset(io_buf, 0, 512);
            memcpy(io_buf, p_de, dir_entry_size);
            ide_write(cur_part->my_disk, all_blocks_lba[block_idx], io_buf, 1);
            dir_inode->i_size += dir_entry_size;
            return true;
        }

        /* 情况 1,若第block_idx块已存在,将其读进内存,然后在该块中查找空目录项 */
        ide_read(cur_part->my_disk, all_blocks_lba[block_idx], io_buf, 1);
        /* 在扇区内查找空目录项 */
        uint8_t dir_entry_idx = 0;
        while (dir_entry_idx < dir_entry_per_sec)
        {
            if ((dir_e + dir_entry_idx)->f_type == FT_UNKNOWN)
            {
                // FT_UNKNOWN为0,无论是初始化或是删除文件后,都会将f_type置为FT_UNKNOWN.
                memcpy(dir_e + dir_entry_idx, p_de, dir_entry_size);
                // 把修改了的数据块同步到硬盘
                ide_write(cur_part->my_disk, all_blocks_lba[block_idx], io_buf, 1);

                dir_inode->i_size += dir_entry_size;
                return true;
            }
            dir_entry_idx++;
        }
        block_idx++;
    }
    printk("directory is full!\n");
    return false;
}

/**
 * @description:  在分区part中，把目录pdir中编号为inode_no的目录项删除
 * @param {partition} *part     分区
 * @param {dir} *pdir   父目录
 * @param {uint32_t} inode_no   inode号
 * @param {void} *io_buf    I/O磁盘的缓存区
 * @return {*} true 表示删除成功
 */
bool delete_dir_entry(struct partition *part, struct dir *pdir, uint32_t inode_no, void *io_buf)
{
    struct inode *dir_inode = pdir->inode;
    uint32_t block_idx = 0, all_blocks[140] = {0};
    // a. 收集目录文件的全部数据块地址
    while (block_idx < 12)
    {
        all_blocks[block_idx] = dir_inode->i_sectors[block_idx];
        block_idx++;
    }
    if (dir_inode->i_sectors[12] != 0)
    {
        ide_read(part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
    }

    /* 目录项在存储时保证不会跨扇区 */
    uint32_t dir_entry_size = part->sb->dir_entry_size;
    uint32_t dir_entry_per_sec = (SECTOR_SIZE / dir_entry_size); // 每扇区最大存储的目录项数目
    struct dir_entry *dir_e = (struct dir_entry *)io_buf;
    struct dir_entry *dir_entry_found = NULL; // 存储被删除目录项地址
    uint8_t dir_entry_idx, dir_entry_cnt;     // 当前扇区目录项索引指针, 当前扇区目录项数目
    bool is_dir_first_block = false;          // 记录当前数据块 是否 是目录的第一个数据块

    // 遍历所有数据块, 寻找目录项
    block_idx = 0;
    while (block_idx < 140)
    {
        is_dir_first_block = false;
        if (all_blocks[block_idx] == 0)
        {
            block_idx++;
            continue;
        }
        dir_entry_idx = dir_entry_cnt = 0;
        memset(io_buf, 0, SECTOR_SIZE);
        // 从硬盘里得到数据块
        ide_read(part->my_disk, all_blocks[block_idx], io_buf, 1);

        // 遍历数据块里的目录项, 统计该扇区的目录项数量及是否有待删除的目录项
        while (dir_entry_idx < dir_entry_per_sec)
        {
            if ((dir_e + dir_entry_idx)->f_type != FT_UNKNOWN)
            { // 目录项存在
                if (!strcmp((dir_e + dir_entry_idx)->filename, "."))
                { // 目录项记录的是当前目录
                    is_dir_first_block = true;
                }
                else if (strcmp((dir_e + dir_entry_idx)->filename, ".") && // 该目录项不是. 且 。。
                         strcmp((dir_e + dir_entry_idx)->filename, ".."))
                {
                    // 统计此扇区内的目录项个数,用来判断删除目录项后是否回收该扇区
                    dir_entry_cnt++;

                    if ((dir_e + dir_entry_idx)->i_no == inode_no)
                    { // 确保目录中只有一个编号为inode_no的inode,找到一次后dir_entry_found就不再是NULL
                        ASSERT(dir_entry_found == NULL);
                        // 如果找到此i结点,就将其记录在dir_entry_found
                        dir_entry_found = dir_e + dir_entry_idx;
                    }
                }
            }
            /* 找到后也继续遍历,统计总共的目录项数 */
            dir_entry_idx++;
        }

        // 若此扇区未找到该目录项, 继续在下一个扇区中找
        if (dir_entry_found == NULL)
        {
            block_idx++;
            continue;
        }

        // 在此扇区中找到目录项后,清除该目录项并判断是否回收扇区,随后退出循环直接返回
        ASSERT(dir_entry_cnt >= 1);

        // 除目录第1扇区外, 若该扇区上只有该目录项自己, 则将整个扇区回收
        if (dir_entry_cnt == 1 && !is_dir_first_block)
        {
            // a 在块位图中回收该块
            uint32_t block_bitmap_idx = all_blocks[block_idx] - part->sb->data_start_lba;
            bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            // b 将块地址从数组i_sectors或索引表中去掉
            if (block_idx < 12)
            {
                dir_inode->i_sectors[block_idx] = 0;
            }
            else
            {
                // 在一级间接索引表中擦除该间接块地址

                /*先判断一级间接索引表中间接块的数量,如果仅有这1个间接块,连同间接索引表所在的块一同回收 */
                uint32_t indirect_blocks = 0;
                uint32_t indirect_block_idx = 12;
                while (indirect_block_idx < 140)
                {
                    if (all_blocks[indirect_block_idx] != 0)
                    {
                        indirect_blocks++;
                    }
                }

                ASSERT(indirect_blocks >= 1); // 包括当前间接块

                if (indirect_blocks > 1)
                {
                    // 间接索引表中还包括其它间接块,仅在索引表中擦除当前这个间接块地址
                    all_blocks[block_idx] = 0;
                    ide_write(part->my_disk, dir_inode->i_sectors[12], all_blocks + 12, 1);
                }
                else
                {
                    // 间接索引表中就当前这1个间接块,直接把间接索引表所在的块回收,然后擦除间接索引表块地址
                    /* 回收间接索引表所在的块 */
                    block_bitmap_idx = dir_inode->i_sectors[12] - part->sb->data_start_lba;
                    bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
                    bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

                    /* 将间接索引表地址清0 */
                    dir_inode->i_sectors[12] = 0;
                }
            }
        }
        else
        {
            // 仅将该目录项清空
            memset(dir_entry_found, 0, dir_entry_size);
            ide_write(part->my_disk, all_blocks[block_idx], io_buf, 1);
        }

        // 更新i结点信息并且同步到硬盘
        ASSERT(dir_inode->i_size >= dir_entry_size);
        dir_inode->i_size -= dir_entry_size;
        memset(io_buf, 0, SECTOR_SIZE * 2);
        inode_sync(part, dir_inode, io_buf);

        return true;
    }
    /* 所有块中未找到则返回false,若出现这种情况应该是serarch_file出错了 */
    return false;
}

/**
 * @description: 读目录， 每次都读取下一个目录项
 *
 * @details dir_read的读取原理就是利用dir_t.dir_pos来记录当前目录的读取位置
 *          每次调用dir_read的时候, 都会更新dir_t.dir_pos, 使其指向下一个目录项
 *          直到dir_t.dir_pos的值大于dir_t.dir_size, 即已经读取完了所有的目录项
 *
 * @param {dir*} dir 需要读取的目录
 * @return {*} 成功返回1个目录项, 失败返回NULL
 */
struct dir_entry *dir_read(struct dir *dir)
{
    dir_entry_t *dir_e = (dir_entry_t *)dir->dir_buf;
    inode_t *dir_inode = dir->inode;

    uint32_t all_block[140] = {0};

    // 复制140个数据块地址
    uint32_t block_idx = 0, dir_entry_idx = 0;
    while (block_idx < 12)
    {
        all_block[block_idx] = dir_inode->i_sectors[block_idx];
        block_idx++;
    }

    // 复制一级间接块
    if (dir_inode->i_sectors[12] != 0)
        ide_read(cur_part->my_disk, dir_inode->i_sectors[12], all_block + 12, 1);

    // 逐块遍历目录项
    block_idx = 0;
    uint32_t cur_dir_entry_pos = 0;
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
    uint32_t dir_entey_per_sce = SECTOR_SIZE / dir_entry_size;

    while (dir->dir_pos < dir_inode->i_size)
    {
        // 已经遍历完了所有的dir_entry, 此时直接返回NULL
        if (dir->dir_pos >= dir_inode->i_size)
            return NULL;

        if (all_block[block_idx] == 0)
        {
            block_idx++;
            continue;
        }

        memset(dir_e, 0, SECTOR_SIZE);
        ide_read(cur_part->my_disk, all_block[block_idx], dir_e, 1);
        dir_entry_idx = 0;
        // 遍历本块的所以目录项
        while (dir_entry_idx < dir_entey_per_sce)
        {
            if ((dir_e + dir_entry_idx)->f_type != FT_UNKNOWN)
            {
                if (cur_dir_entry_pos < dir->dir_pos)
                {
                    cur_dir_entry_pos += dir_entry_size;
                    dir_entry_idx++;
                    continue;
                }
                ASSERT(cur_dir_entry_pos == dir->dir_pos);
                // 更新dir_pos
                dir->dir_pos += dir_entry_size;
                return dir_e + dir_entry_idx;
            }
            dir_entry_idx++;
        }
        block_idx++;
    }
    return NULL;
}

/**
 * @brief 判断目录是否为空
 *
 * @param dir 被操作的目录
 * @return true 目录为空
 * @return false 目录不空
 */
bool dir_is_empty(struct dir *dir)
{
    struct inode *dir_inode = dir->inode;
    // 若目录下只有 . 和 .. 两个目录项, 则目录为空
    return (dir_inode->i_size == cur_part->sb->dir_entry_size * 2);
}

/**
 * @brief 在父目录parent_dir中删除 child_dir, 第一是删除自己的目录项，释放占用的数据块空间, 最后释放占用的inode表
 *          只删除空目录
 * @param parent_dir
 * @param child_dir
 * @return int32_t
 */
int32_t dir_remove(struct dir *parent_dir, struct dir *child_dir)
{
    inode_t *child_dir_inode = child_dir->inode;

    // 确保是空目录, 此时只有i_sectors[0]表示的块中有'.'和'..'
    int32_t block_idx = 1;
    while (block_idx < 12)
    {
        ASSERT(child_dir_inode->i_sectors[block_idx] == 0);
        block_idx++;
    }

    void *io_buf = sys_malloc(SECTOR_SIZE * 2);
    if (io_buf == NULL)
    {
        printk("%s: malloc for io_buf failed\n", __func__);
        return -1;
    }

    // 在父目录中删除子目录对应的目录项
    delete_dir_entry(cur_part, parent_dir, child_dir_inode->i_no, io_buf);

    // 回收inode中i_sector所占用的扇区 : 1.修改inode_bitmap 2.
    //  修改inode_bitmap 和 block_bitmap
    inode_release(cur_part, child_dir->inode->i_no);

    sys_free(io_buf);

    return 0;
}
