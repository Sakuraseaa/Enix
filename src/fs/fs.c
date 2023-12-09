#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "dir.h"
#include "stdint.h"
#include "stdio-kernel.h"
#include "list.h"
#include "string.h"
#include "ide.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "file.h"
#include "timer.h"
#include "console.h"
#include "thread.h"
#include "ioqueue.h"
#include "pipe.h"
// 在ide.c中声明
extern uint8_t channel_cnt;
extern struct ide_channel channels[2]; ///< 系统当前最大支持两个 ide 通道
extern struct list partition_list;     ///< 分区链表, 在ide.c中的partition_scan中已经建立好了
// 再keyboard.c中声明
extern struct ioqueue kbd_buf;

// 默认情况下操作的是哪个分区
struct partition *cur_part;

// 挂载分区，把文件的系统的元信息复制到内存，元信息放在cur_part里
/* 在分区链表中找到名为part_name的分区,并将其指针赋值给cur_part */
// 挂载分区的实际动作只是把某分区中的文件系统的元数据(空闲块位图，inode位图，超级块)，读入到内存，为了更好的，在分区上操作
/**
 * @brief mount_partition用于挂载分区, 即设置current_partition.
 *          ide.c中的ide_init函数在初始化硬盘的时候, 就已经扫描了硬盘中所有的分区, 并且将所有分区插入到partition_list的全局链表中
 *          因此, mount_partition函数用于在partition_list中进行扫描, 然后将current_partition设置为需要设置的分区
 *        需要注意的是, 在ide.c的partition_scan中只会计算记录partition的起始lba号等信息, 而诸如记录分区内具体得文件系统信息的
 *        super_block, 记录打开的inode的open_inode list, 记录已经分配出去的block的block_bitmap, 已经分配出的的inode的
 *        inode_bitmap等数据都没有从磁盘中读出来, 或者在内存中初始化
 *        所以, mount_partition除了设置current_partition以外, 还会完成上面说的这些内容, 即:
 *          1. 在内存中初始化partition.sb, 并从磁盘中读取该分区的super_block信息, 复制到partition.sb中
 *          2. 在内存中初始化partition.block_bitmap, 并从磁盘中读取该分区的block_bitmap信息, 复制到partition.block_bitmap中
 *          3. 在内存中初始化partition.inode_bitmap, 并从磁盘中读取该分区的inode_bitmap信息, 复制到partition.inode_bitmap中
 *          4. 在内存中初始化partition.open_inode list
 *
 * @param elem 一开始的时候, 要将其设置为list_partition中的首个partition
 * @param arg 需要挂载的分区名
 * @return true 无意义, 用于给list_traversal停止遍历
 * @return false 无意义, 用于给list_traversal继续遍历
 */
static bool mount_partition(struct list_elem *pelem, int arg)
{
    char *part_name = (char *)arg;
    struct partition *part = elem2entry(struct partition, part_tag, pelem);
    if (!strcmp(part->name, part_name))
    {
        cur_part = part;
        struct disk *hd = cur_part->my_disk;

        /* sb_buf用来存储从硬盘上读入的超级块 */
        struct super_block *sb_buf = (struct super_block *)sys_malloc(SECTOR_SIZE);

        /* 在内存中创建分区cur_part的超级块 */
        cur_part->sb = (struct super_block *)sys_malloc(sizeof(struct super_block));
        if (cur_part->sb == NULL)
        {
            PANIC("alloc memory failed!");
        }

        // 读入超级块
        memset(sb_buf, 0, SECTOR_SIZE);
        ide_read(hd, cur_part->start_lba + 1, sb_buf, 1);
        // 把sb_buf中超级块的星系复制到分区的超级块sb中
        memcpy(cur_part->sb, sb_buf, sizeof(struct super_block));

        /**********     将硬盘上的块位图读入到内存    ****************/
        cur_part->block_bitmap.bits = (uint8_t *)sys_malloc(sb_buf->block_bitmap_sects * SECTOR_SIZE);
        if (cur_part->block_bitmap.bits == NULL)
        {
            PANIC("alloc memory failed!");
        }
        cur_part->block_bitmap.btmp_bytes_len = sb_buf->block_bitmap_sects * SECTOR_SIZE;
        /* 从硬盘上读入块位图到分区的block_bitmap.bits */
        ide_read(hd, sb_buf->block_bitmap_lba, cur_part->block_bitmap.bits, sb_buf->block_bitmap_sects);
        /**********************************************************/

        /**********     将硬盘上的inode位图读入到内存    ************/
        cur_part->inode_bitmap.bits = (uint8_t *)sys_malloc(sb_buf->inode_bitmap_sects * SECTOR_SIZE);
        if (cur_part->inode_bitmap.bits == NULL)
        {
            PANIC("alloc memory failed!");
        }
        cur_part->inode_bitmap.btmp_bytes_len = sb_buf->inode_bitmap_sects * SECTOR_SIZE;
        // 从硬盘上读入inode位图到分区的inode_bitmap.bits
        ide_read(hd, sb_buf->inode_bitmap_lba, cur_part->inode_bitmap.bits, sb_buf->inode_bitmap_sects);
        /**********************************************************/

        list_init(&cur_part->open_inodes);
        printk("mount %s done!\n", part->name);

        /* 此处返回true是为了迎合主调函数list_traversal的实现,与函数本身功能无关。
        只有返回true时list_traversal才会停止遍历,减少了后面元素无意义的遍历.*/
        return true;
    }

    // 使list_traversal继续遍历
    return false;
}

// 格式化分区, 也就是初始化分区的元信息, 创建文件系统
// 在硬盘对应的分区创建超级块, 空闲块位图, inode位图, inode表以及根目录
/**
 * @brief partition_format用于对hd指向的硬盘中的patition分区进行格式化.
 *      注意, 本系统中的文件系统, 一个块占用一个扇区, 因此block_cnt和sec_cnt是等价的
 *
 * @details 一个分区的文件系统包含:
 *              1. OS Loader Block          可能占用多个块, 但是我们的系统中loader占用1个块
 *              2. Super Block              记录文件系统元信息的超级块, 同样, 别的系统可能会占用多个块, 但是我们的系统的超级块只会占用一个扇区
 *              3. Block Bitmap             Block bitmap的位数取决于磁盘剩余多少个块, 所以同样长度不确定, 可能占用1~多个扇区
 *              4. Inode Bitmap             Inode bitmap的位数取决于文件系统的设定, 我们设定SKOS中只有4096个块, 因此只会使用一个扇区
 *              5. Inode Table              一个Inode占用一个扇区, 所以这里会占用4096个扇区
 *
 *          因此, 为了在一个分区上创建文件系统, 本函数干的事情如下:
 *              1. 略过OS Loader, 这个是不该由格式化函数来写入到磁盘中
 *              2. 计算本文件系统(分区)的元信息, 并将其写入到Super Block中
 *              3. Block Bitmap初始化, 一开始只分配出去了根目录的inode所在块, 所以写入的block bitmap就是10000000.......
 *              4. Inode Bitmap初始化, 一开始分配出去的inode只有根目录的inode, 所以写入的inode bitmap就是1000000.......
 *              5. Inode Table初始化, 同样, 第一个inode就是根目录, 其他的全都设置为0
 *              6. 创建了根目录, 在根目录中注册了 . 和 .. 目录项。我们根目录就定在数据区第一个扇区, 占用的也是inode表的第一个位置
 *
 * @param hd 指向需要格式化的分区所在的硬盘
 * @param partition 指向需要格式化的分区
 */
static void partition_format(struct partition *part)
{
    /* 为方便实现,一个块大小是一扇区 */

    // 系统引导块占用的扇区数
    uint32_t boot_sector_sects = 1;
    // 超级块占用的扇区数
    uint32_t super_block_sects = 1;
    // I结点位图占用的扇区数.规定每个分区最多支持4096个文件
    uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);
    // inode表占用的扇区数
    uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct inode)) * MAX_FILES_PER_PART), SECTOR_SIZE);
    // 已使用的扇区数
    uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
    // 空闲的扇区数
    uint32_t free_sects = part->sec_cnt - used_sects;

    /************** 空闲块位图占据的扇区数 ***************/
    uint32_t block_bitmap_sects;
    block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);
    /* block_bitmap_bit_len是位图中位的长度,也是可用块的数量 */
    uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects;
    block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR);
    /*********************************************************/

    /* 超级块初始化 */
    struct super_block sb;
    sb.magic = 0x19590318;
    sb.sec_cnt = part->sec_cnt;
    sb.inode_cnt = MAX_FILES_PER_PART;
    sb.part_lba_base = part->start_lba;

    sb.block_bitmap_lba = sb.part_lba_base + 2; // 第0块是引导块,第1块是超级块
    sb.block_bitmap_sects = block_bitmap_sects;

    sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
    sb.inode_bitmap_sects = inode_bitmap_sects;

    sb.inode_table_lba = sb.inode_bitmap_lba + sb.inode_bitmap_sects;
    sb.inode_table_sects = inode_table_sects;

    sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
    sb.root_inode_no = 0;
    sb.dir_entry_size = sizeof(struct dir_entry);

    printk("%s info:\n", part->name);
    printk("   magic:0x%x\n   part_lba_base:0x%x\n   all_sectors:0x%x\n   inode_cnt:0x%x\n   block_bitmap_lba:0x%x\n   block_bitmap_sectors:0x%x\n   inode_bitmap_lba:0x%x\n   inode_bitmap_sectors:0x%x\n   inode_table_lba:0x%x\n   inode_table_sectors:0x%x\n   data_start_lba:0x%x\n", sb.magic, sb.part_lba_base, sb.sec_cnt, sb.inode_cnt, sb.block_bitmap_lba, sb.block_bitmap_sects, sb.inode_bitmap_lba, sb.inode_bitmap_sects, sb.inode_table_lba, sb.inode_table_sects, sb.data_start_lba);

    struct disk *hd = part->my_disk;
    /*******************************
     * 1 将超级块写入本分区的1扇区 *
     ******************************/
    ide_write(hd, part->start_lba + 1, &sb, 1);
    printk("   super_block_lba:0x%x\n", part->start_lba + 1);

    /* 找出数据量最大的元信息,用其尺寸做存储缓冲区*/
    uint32_t buf_size = (sb.block_bitmap_sects >= sb.inode_bitmap_sects ? sb.block_bitmap_sects : sb.inode_bitmap_sects);
    buf_size = (buf_size >= sb.inode_table_sects ? buf_size : sb.inode_table_sects) * SECTOR_SIZE;
    uint8_t *buf = (uint8_t *)sys_malloc(buf_size); // 申请的内存由内存管理系统清0后返回

    /**************************************
     * 2 将块位图初始化并写入sb.block_bitmap_lba *
     *************************************/
    /* 初始化块位图block_bitmap */
    buf[0] |= 0x01; // 第0个块预留给根目录,位图中先占位
    uint32_t block_bitmap_last_byte = block_bitmap_bit_len / 8;
    uint8_t block_bitmap_last_bit = block_bitmap_bit_len % 8;

    // last_size是位图所在最后一个扇区中，不足一扇区的其余部分
    uint32_t last_size = SECTOR_SIZE - (block_bitmap_last_byte % SECTOR_SIZE);

    /* 1 先将位图最后一字节到其所在的扇区的结束全置为1,即超出实际块数的部分直接置为已占用*/
    // 这里如果buf缓存区的地址不够大, 那么可能造成错误
    memset(&buf[block_bitmap_last_byte], 0xff, last_size);

    /* 2 再将上一步中覆盖的最后一字节内的有效位重新置0 */
    uint8_t bit_idx = 0;
    while (bit_idx <= block_bitmap_last_bit)
        buf[block_bitmap_last_byte] &= ~(1 << bit_idx++);

    ide_write(hd, sb.block_bitmap_lba, buf, sb.block_bitmap_sects);

    /***************************************
     * 3 将inode位图初始化并写入sb.inode_bitmap_lba *
     ***************************************/
    /* 先清空缓冲区*/
    memset(buf, 0, buf_size);
    buf[0] |= 0x1; // 第0个inode分给了根目录
    /* 由于inode_table中共4096个inode,位图inode_bitmap正好占用1扇区,
     * 即inode_bitmap_sects等于1, 所以位图中的位全都代表inode_table中的inode,
     * 无须再像block_bitmap那样单独处理最后一扇区的剩余部分,
     * inode_bitmap所在的扇区中没有多余的无效位 */
    ide_write(hd, sb.inode_bitmap_lba, buf, sb.inode_bitmap_sects);

    /***************************************
     * 4 将inode数组初始化并写入sb.inode_table_lba
     ***************************************/
    /* 准备写inode_table中的第0项,即根目录所在的inode */
    memset(buf, 0, buf_size); // 先清空缓冲区buf
    struct inode *i = (struct inode *)buf;
    i->i_size = sb.dir_entry_size * 2;   // .和..
    i->i_no = 0;                         // 根目录占inode数组中第0个inode
    i->i_sectors[0] = sb.data_start_lba; // 由于上面的memset,i_sectors数组的其它元素都初始化为0
    ide_write(hd, sb.inode_table_lba, buf, sb.inode_table_sects);

    /***************************************
     * 5 将根目录初始化并写入sb.data_start_lba
     ***************************************/
    /* 写入根目录的两个目录项.和.. */
    memset(buf, 0, buf_size);
    struct dir_entry *p_de = (struct dir_entry *)buf;

    /* 初始化当前目录"." */
    memcpy(p_de->filename, ".", 1);
    p_de->i_no = 0;
    p_de->f_type = FT_DIRECTORY;
    p_de++;

    /* 初始化当前目录父目录".." */
    memcpy(p_de->filename, "..", 2);
    p_de->i_no = 0; // 根目录的父目录依然是根目录自己
    p_de->f_type = FT_DIRECTORY;

    /* sb.data_start_lba已经分配给了根目录,里面是根目录的目录项 */
    ide_write(hd, sb.data_start_lba, buf, 1);

    printk("   root_dir_lba:0x%x\n", sb.data_start_lba);
    printk("%s format done\n", part->name);
    sys_free(buf);
}

/**
 * @brief path_parse用于获得文件路径pathname的顶层路径, 顶层路径存入到name_store中
 *
 * @example pathname="/home/sk", char name_store[10]
 *          path_parse(pathname, name_store) -> return "/sk", *name_store="home"
 *
 * @param pathname 需要解析的文件路径
 * @param name_store 主调函数提供的缓冲区
 * @return char* 指向除顶层路径之外的子路径字符串的地址
 */
char *path_parse(char *pathname, char *name_store)
{
    // 根目录不需要解析, 跳过即可
    if (pathname[0] == '/')
        while (*(++pathname) == '/')
            ; // 跳过'//a', '///b'

    while (*pathname != '/' && *pathname != 0)
        *name_store++ = *pathname++;

    if (pathname[0] == 0) // pathname为空, 则表示路径已经结束了, 此时返回NULL
        return NULL;

    return pathname;
}

/**
 * @brief path_depth_cnt用于返回路径pathname的深度. 注意, 所谓路径的深度是指到文件的深度, 例如: /a的深度是1, /a/b的深度
 *        是2, /a/b/c/d/e的深度是5
 *
 * @param pathname 需要判断深度的路径名
 * @return uint32_t 路径的深度
 */
int32_t path_depth_cnt(char *pathname)
{
    ASSERT(pathname != NULL);

    char *p = pathname;
    char name[MAX_FILE_NAME_LEN];
    uint32_t depth = 0;
    p = path_parse(p, name);
    while (name[0])
    {
        depth++;
        memset(name, 0, MAX_FILE_NAME_LEN);
        if (p)
            p = path_parse(p, name);
    }
    return depth;
}

/**
 * @brief search_file用于搜索给定的文件. 若能找到, 则返回要搜索的文件的inode号, 若找不到则返回-1
 *
 * @param pathname 要搜索的文件的绝对路径
 * @param searched_record 路径搜索记录结构体
 * @return int 若成功, 则返回文件的inode号, 若找不到则返回-1
 */
static int search_file(const char *pathname, struct path_search_record *searched_record)
{
    // 如果待查找的是根目录,为避免下面无用的查找,直接返回已知根目录信息
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") || !strcmp(pathname, "/.."))
    {
        searched_record->parent_dir = &root_dir;
        searched_record->file_type = FT_DIRECTORY;
        searched_record->searched_path[0] = 0; // 搜索路径置空
        return 0;
    }

    uint32_t path_len = strlen(pathname);
    // 保证pathname至少是这样的路径 /name 且 小于最大长度
    ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);
    char *sub_path = (char *)pathname;
    struct dir *parent_dir = &root_dir;
    struct dir_entry dir_e;

    // 记录路径解析出来的各级名称, 如路径“/a/b/c”
    //  -- name 每次的值分别是 "a","b","c"
    char name[MAX_FILE_NAME_LEN] = {0};

    searched_record->parent_dir = parent_dir; // 记录当前查找的父目录
    searched_record->file_type = FT_UNKNOWN;  // 记录当前查找的文件的类型
    uint32_t parent_inode_no = 0;             // 此变量维护的是当前要搜索的文件的所属目录的inode号

    sub_path = path_parse(sub_path, name);
    while (name[0])
    { // 若第一个字符就是结束符,结束循环
        // 记录查找过的路径, 但不能超过searched_path的长度 512 字节
        ASSERT(strlen(searched_record->searched_path) < 512);

        // 记录已解析的路径
        strcat(searched_record->searched_path, "/");
        strcat(searched_record->searched_path, name);

        if (search_dir_entry(cur_part, parent_dir, name, &dir_e))
        {
            memset(name, 0, MAX_FILE_NAME_LEN);
            // 若sub_path不等于NULL,也就是未结束时继续拆分路径
            if (sub_path)
                sub_path = path_parse(sub_path, name);

            if (FT_DIRECTORY == dir_e.f_type)
            { // 如果被打开的是目录
                parent_inode_no = parent_dir->inode->i_no;
                dir_close(parent_dir);

                // 把下一层目录的inode加载到内存，继续在下一层目录中寻找
                parent_dir = dir_open(cur_part, dir_e.i_no);
                searched_record->parent_dir = parent_dir;
                continue;
            }
            else if (FT_REGULAR == dir_e.f_type)
            { // 若是普通文件
                searched_record->file_type = FT_REGULAR;
                return dir_e.i_no;
            }
        }
        else
        { // 若找不到,则返回-1
            // 找不到目录项时,要留着parent_dir不要关闭,
            // 若是创建新文件的话需要在parent_dir中创建
            return -1;
        }
    }

    // 执行到此,必然是遍历了完整路径并且查找的文件或目录, 只有同名目录存在
    dir_close(searched_record->parent_dir);

    // 保存被查找目录的直接父目录
    searched_record->parent_dir = dir_open(cur_part, parent_inode_no);
    searched_record->file_type = FT_DIRECTORY;

    return dir_e.i_no;
}

/* 打开或创建文件成功后,返回文件描述符,否则返回-1 */
// brief: 函数在查找pathname, 如果文件不存在也没有要求创建， 回返回 -1，如果文件要创建，或者文件存在 返回 句柄
int32_t sys_open(const char *pathname, uint8_t flags)
{
    /* 对目录要用dir_open,这里只有open文件 */
    if (pathname[strlen(pathname) - 1] == '/')
    {
        printk("can's open a directory %s\n", pathname);
        return -1;
    }
    ASSERT(flags <= 7);
    int32_t fd = -1; // 文件描述符

    // 可以参见 操作系统真相还原 - 630页
    struct path_search_record searched_record; // 在找文件的过程中, 记录所查找的路径, 该路径总是包含文件系统实体路径的下一个子路
    memset(&searched_record, 0, sizeof(struct path_search_record));

    /* 计算路径深度.帮助判断中间某个目录不存在的情况 */
    uint32_t pathname_depth = path_depth_cnt((char *)pathname);

    // a. 先检查文件是否存在,这个函数也打开了pathname的父目录，准确来说是加载pathname的父目录到了内存
    int inode_no = search_file(pathname, &searched_record);
    bool found = inode_no != -1 ? true : false;

    if (searched_record.file_type == FT_DIRECTORY)
    { // 不能打开目录
        printk("can's open a direcotry with open(), use opendir() to instead\n");
        dir_close(searched_record.parent_dir);
        return -1;
    }

    uint32_t path_search_depth = path_depth_cnt(searched_record.searched_path);

    /* b. 先判断是否把pathname的各层目录都访问到了,即是否在某个中间目录就失败了 */
    if (pathname_depth != path_search_depth)
    {
        // 说明没有访问到全部路径，中间断掉了
        printk("cannot access %s: Not a directory, subpath %s is't exist\n", pathname, searched_record.searched_path);
        dir_close(searched_record.parent_dir);
        return -1;
    }

    /* c. 若是在最后一个路径上没找到,并且并不是要创建文件,直接返回-1 */
    if (!found && !(flags & O_CREAT))
    {
        printk("in path %s, file %s is't exist\n", searched_record.searched_path, (strrchr(searched_record.searched_path, '/') + 1));
        dir_close(searched_record.parent_dir);
        return -1;
    }
    else if (found && (flags & O_CREAT))
    { // 若要创建的文件已存在

        printk("%s has already exist!\n", pathname);
        dir_close(searched_record.parent_dir);
        return -1;
    }

    switch (flags & O_CREAT)
    {
    case O_CREAT:
        printk("creating file\n");
        // 给文件创建inode,在目录里面记录该文件
        fd = file_create(searched_record.parent_dir, (strrchr(pathname, '/') + 1), flags);
        dir_close(searched_record.parent_dir);
        break;
    default:
        /* 其余情况均为打开已存在文件:
         * O_RDONLY,O_WRONLY,O_RDWR */
        fd = file_open(inode_no, flags);
    }

    /* 此fd是指任务pcb->fd_table数组中的元素下标,
     * 并不是指全局file_table中的下标 */
    return fd;
}

/* 在磁盘上搜索分区,若有未格式化发分区，则格式化分区创建文件系统 */
/* 挂载sdb1分区，打开根目录*/
// 在ide.c硬盘初始化过程中，得到了每个分区的起始地址, 大小, 名字。channels[]
// 本函数在每个分区中创建文件系统，并且挂载指定姓名的文件系统(分区)
void filesys_init()
{
    uint8_t channel_no = 0, dev_no, part_idx = 0;

    /* sb_buf用来存储从硬盘上读入的超级块 */
    struct super_block *sb_buf = (struct super_block *)sys_malloc(SECTOR_SIZE);

    if (sb_buf == NULL)
        PANIC("alloc memory failed!");

    printk("searching filesystem......\n");
    // 遍历通道
    while (channel_no < channel_cnt)
    {
        dev_no = 0;
        // 遍历硬盘
        while (dev_no < 2)
        {
            if (dev_no == 0)
            { // 跨过裸盘hd60M.img
                dev_no++;
                continue;
            }
            struct disk *hd = &channels[channel_no].devices[dev_no];
            struct partition *part = hd->prim_parts;
            // 遍历分区
            while (part_idx < 12)
            { // 4个主分区+8个逻辑
                if (part_idx == 4)
                { // 开始处理逻辑分区
                    part = hd->logic_parts;
                }

                /* channels数组是全局变量,默认值为0,disk属于其嵌套结构,
                 * partition又为disk的嵌套结构,因此partition中的成员默认也为0.
                 * 若partition未初始化,则partition中的成员仍为0.
                 * 下面处理存在的分区. */
                if (part->sec_cnt != 0)
                { // 如果分区存在
                    memset(sb_buf, 0, SECTOR_SIZE);

                    /* 读出分区的超级块,根据魔数是否正确来判断是否存在文件系统 */
                    ide_read(hd, part->start_lba + 1, sb_buf, 1);

                    /* 只支持自己的文件系统.若磁盘上已经有文件系统就不再格式化了 */
                    if (sb_buf->magic == 0x19590318)
                    {
                        printk("%s has filesystem\n", part->name);
                    }
                    else
                    { // 其它文件系统不支持,一律按无文件系统处理
                        printk("formatting %s`s partition %s......\n", hd->name, part->name);
                        partition_format(part);
                    }
                }
                part_idx++;
                part++; // 下一分区
            }
            dev_no++; // 下一磁盘
        }
        channel_no++; // 下一通道
    }
    sys_free(sb_buf);

    // 确认默认操作的分区
    char default_part[8] = "sdb1";
    // 挂载分区
    list_traversal(&partition_list, mount_partition, (int)default_part);

    // 将当前分区的根目录打开
    open_root_dir(cur_part);

    // 初始化文件表
    uint32_t fd_idx = 0;
    while (fd_idx < MAX_FILE_OPEN)
    {
        file_table[fd_idx++].fd_inode = NULL;
    }
    printk("filesystem_init done!\n");
}

/**
 * @brief fd_local2global将进程的文件描述符 转换到文件表下标
 *
 * @param local_fd 文件描述符
 * @return uint32_t 全局文件表的下标
 */
uint32_t fd_local2global(uint32_t local_fd)
{
    struct task_struct *cur = running_thread();
    int32_t global_fd = cur->fd_table[local_fd];
    ASSERT(global_fd >= 0 && global_fd < MAX_FILE_OPEN);
    return (uint32_t)global_fd;
}

// 关闭文件描述符fd指向的文件，成功返回0， 否则返回 -1
int32_t sys_close(int32_t fd)
{
    int ret = -1; // 默认返回失败
    if (fd > 2)
    {
        uint32_t _fd = fd_local2global(fd);
        if (is_pipe(fd))
        {
            if (--file_table[_fd].fd_pos == 0)
            {
                mfree_page(PF_KERNEL, file_table[_fd].fd_inode, 1);
                file_table[_fd].fd_inode = NULL;
            }
            ret = 0;
        }
        else
            ret = file_close(&file_table[_fd]);
        running_thread()->fd_table[fd] = -1; // 标记为空
    }
    return ret;
}

/**
 * @brief sys_write： 把 buf 缓冲区中count个字节 写入 文件描述符为 fd 的文件。
 *                    file_write()的套壳
 *
 * @param fd    文件描述符
 * @param buf   数据缓冲区
 * @param count 要写入的字节数
 *
 * @return 失败返回 -1, 成功返回写入的字节数
 */
int32_t sys_write(int32_t fd, const void *buf, uint32_t count)
{
    if (fd < 0)
    {
        printk("sys_write: fd error\n");
        return -1;
    }
    // 写 到 显示屏上
    if (fd == stdout_no)
    {
        if (is_pipe(fd))
        {
            return pipe_write(fd, buf, count);
        }
        else
        {
            char tmp_buf[1024] = {0};
            memcpy(tmp_buf, buf, count);
            console_put_str(tmp_buf);
            return count;
        }
    }
    else if (is_pipe(fd))
        return pipe_write(fd, buf, count);

    uint32_t _fd = fd_local2global(fd);
    struct file *wr_file = &file_table[_fd];
    if (wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR)
    {
        uint32_t bytes_written = file_write(wr_file, buf, count);
        return bytes_written;
    }
    else
    {
        console_put_str("sys_write: not allowed to write file without flag O_RDWR or O_WRONLY\n");
        return -1;
    }
}

/**
 * @description: 读入文件 file_read函数的套壳
 * @param {int32_t} fd 文件描述符
 * @param {void*} buf   存储读入数据的内存空间
 * @param {uint32_t} count 需要读入的字节数
 * @return {*}
 */
int32_t sys_read(int32_t fd, void *buf, uint32_t count)
{
    ASSERT(buf != NULL)
    int ret = -1;
    if (fd < 0 || fd == stdout_no || fd == stderr_no)
        printk("sys_read: fd error\n");
    else if (fd == stdin_no)
    {
        if (is_pipe(fd))
        {
            return pipe_write(fd, buf, count);
        }
        else
        {
            char *buffer = buf;
            uint32_t bytes_read = 0;
            while (bytes_read < count)
            {
                *buffer = ioq_getchar(&kbd_buf);
                bytes_read++;
                buffer++;
            }
            ret = (bytes_read == 0 ? -1 : (int32_t)bytes_read);
        }
    }
    else if (is_pipe(fd))
        return pipe_read(fd, buf, count);
    else
    {
        uint32_t _fd = fd_local2global(fd);
        ret = file_read(&file_table[_fd], buf, count);
    }
    return ret;
}

/**
 * @description: 修改文件读写操作的偏移指针
 * @param {int32_t} fd 被操作的文件描述符
 * @param {int32_t} offset  偏移量
 * @param {uint8_t} whence  标志(SEEK_SET文件开头， SEEK_CUR当前位置, SEEK_END 文件结尾)
 * @return {*} 返回修改后的文件指针
 */
int32_t sys_Iseek(int32_t fd, int32_t offset, uint8_t whence)
{
    if (fd < 0)
    {
        printk("sys_Iseek: fd error\n");
        return -1;
    }

    ASSERT(whence > 0 && whence < 4);
    uint32_t _fd = fd_local2global(fd);
    struct file *pf = &file_table[_fd];
    int32_t new_pos = 0;                               // 记录新的偏移量
    int32_t file_size = (int32_t)pf->fd_inode->i_size; // 文件大小

    switch (whence)
    {
    case SEEK_SET:
        /* SEEK_SET 新的读写位置是相对于文件开头再增加offset个位移量 */
        new_pos = 0 + offset;
        break;
    case SEEK_CUR:
        /* SEEK_CUR 新的读写位置是相对于当前的位置增加offset个位移量 */
        new_pos = (int32_t)pf->fd_pos + offset;
        break;
    case SEEK_END:
        /* SEEK_END 新的读写位置是相对于文件尺寸再增加offset个位移量 */
        new_pos = file_size + offset;
    }
    if (new_pos < 0 || new_pos > (file_size - 1))
        return -1;

    pf->fd_pos = new_pos;
    return pf->fd_pos;
}

/**
 * @description: 删除文件, 需要修改父目录数据块，
 * @param {char*} pathname 被删除文件路径
 * @return 成功返回 0, 失败返回 -1
 */
int32_t sys_unlink(const char *pathname)
{
    ASSERT(strlen(pathname) < MAX_PATH_LEN);

    // 先检查待删除的文件是否存在
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));

    int inode_no = search_file(pathname, &searched_record);
    ASSERT(inode_no != 0);
    if (inode_no == -1)
    {
        printk("file %s not found!\n", pathname);
        dir_close(searched_record.parent_dir);
        return -1;
    }
    if (searched_record.file_type == FT_DIRECTORY)
    {
        printk("can't delete a dircotry with unlink(), use rm dir() to instead\n");
        dir_close(searched_record.parent_dir);
        return -1;
    }

    /* 检查是否在已打开文件列表(文件表)中 */
    uint32_t file_idx = 0;
    while (file_idx < MAX_FILE_OPEN)
    {
        if (file_table[file_idx].fd_inode != NULL && (uint32_t)inode_no == file_table[file_idx].fd_inode->i_no)
        {
            break;
        }
        file_idx++;
    }
    if (file_idx < MAX_FILE_OPEN)
    {
        dir_close(searched_record.parent_dir);
        printk("file %S is in use, not allow to delete!\n", pathname);
        return -1;
    }
    ASSERT(file_idx == MAX_FILE_OPEN);

    // 为delete_dir_entry申请缓冲区
    void *io_buf = sys_malloc(SECTOR_SIZE + SECTOR_SIZE);
    if (io_buf == NULL)
    {
        dir_close(searched_record.parent_dir);
        printk("sys_unlink: malloc for io_buf failed\n");
        return -1;
    }

    struct dir *parent_dir = searched_record.parent_dir;
    delete_dir_entry(cur_part, parent_dir, inode_no, io_buf);
    inode_release(cur_part, inode_no);
    sys_free(io_buf);
    dir_close(searched_record.parent_dir);

    return 0;
}

/**
 * @description:  删除目录
 * @param {char*} path
 * @return {int32_t} 成功返回0, 失败返回 -1
 */
int32_t sys_mkdir(const char *pathname)
{
    uint8_t rollback_step = 0; // 用于操作失败时回滚各资源状态

    // 1.创建IO缓存区, 用来读写磁盘
    void *io_buf = sys_malloc(SECTOR_SIZE * 2);
    if (io_buf == NULL)
    {
        printk("sys_mkdir: sys_malloc for io_buf failed\n");
        return -1;
    }

    // 2. 判断要创建的目录是否存在
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = -1;
    inode_no = search_file(pathname, &searched_record);
    if (inode_no != -1)
    { // // 2.1 确保不存在同名文件或者目录
        printk("%s: file or directory %s exist!\n", __func__, pathname);
        rollback_step = 1;
        goto rollback;
    }
    else
    { //  2.2判断是否完全遍历了目标路径
        // 利用文件的深度和已经搜索的路径的深度来判断是否已经全部遍历完
        uint32_t pathname_depth = path_depth_cnt((char *)pathname);
        uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
        // 深度不等, 说明中间某个文件目录不存在
        if (pathname_depth != path_searched_depth)
        { // 说明并没有访问到全部的路径,某个中间目录是不存在的
            printk("sys_mkdir: can`t access %s, subpath %s is`t exist\n", pathname, searched_record.searched_path);
            rollback_step = 1;
            goto rollback;
        }
    }

    struct dir *parent_dir = searched_record.parent_dir;
    /* 目录名称后可能会有字符'/',所以最好直接用searched_record.searched_path,无'/' */
    char *dirname = strrchr(searched_record.searched_path, '/') + 1;
    // 3. 为新目录 分配inode
    inode_no = inode_bitmap_alloc(cur_part);
    if (inode_no == -1)
    {
        printk("%s: allocate inode for directory_%s failed!\n", __func__, dirname);
        rollback_step = 1;
        goto rollback;
    }

    struct inode new_dir_inode;
    inode_init(inode_no, &new_dir_inode); // 初始化i结点
    // 4. 为新目录分配数据块
    uint32_t block_bitmap_idx = 0; // 用来记录block对应于block_bitmap中的索引
    int32_t block_lba = -1;
    /* 为目录分配一个块,用来写入目录.和.. */
    block_lba = block_bitmap_alloc(cur_part);
    if (block_lba == -1)
    {
        printk("%s: allocate block for directory_%s failed\n", __func__, dirname);
        rollback_step = 2;
        goto rollback;
    }
    new_dir_inode.i_sectors[0] = block_lba;
    /* 每分配一个块就将位图同步到硬盘 */
    block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
    ASSERT(block_bitmap_idx != 0);

    // 5. 为新目录中创建两个目录项 "." 和 ".." 并同步到硬盘
    /* 将当前目录的目录项'.'和'..'写入目录 */
    memset(io_buf, 0, SECTOR_SIZE * 2); // 清空io_buf
    struct dir_entry *p_de = (struct dir_entry *)io_buf;

    /* 初始化当前目录"." */
    memcpy(p_de->filename, ".", 1);
    p_de->i_no = inode_no;
    p_de->f_type = FT_DIRECTORY;

    p_de++;
    /* 初始化当前目录".." */
    create_dir_entry("..", parent_dir->inode->i_no, FT_DIRECTORY, p_de);
    ide_write(cur_part->my_disk, new_dir_inode.i_sectors[0], io_buf, 1);

    new_dir_inode.i_size = 2 * cur_part->sb->dir_entry_size;

    // 6. 在新目录的父目录中添加新目录项的目录项 并同步到硬盘
    struct dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct dir_entry));
    create_dir_entry(dirname, inode_no, FT_DIRECTORY, &new_dir_entry);
    memset(io_buf, 0, SECTOR_SIZE * 2); // 清空io_buf
    if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf))
    { // sync_dir_entry中将block_bitmap通过bitmap_sync同步到硬盘
        printk("%s: sync_dir_entry to disk filed!\n", __func__);
        rollback_step = 3;
        goto rollback;
    }

    // 7. 同步块位图到硬盘
    bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

    /* 父目录的inode同步到硬盘 */
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(cur_part, parent_dir->inode, io_buf);

    /* 将新创建目录的inode同步到硬盘 */
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(cur_part, &new_dir_inode, io_buf);

    /* 将inode位图同步到硬盘 */
    bitmap_sync(cur_part, inode_no, INODE_BITMAP);

    sys_free(io_buf);

    /* 关闭所创建目录的父目录 */
    dir_close(searched_record.parent_dir);
    return 0;

/*创建文件或目录需要创建相关的多个资源,若某步失败则会执行到下面的回滚步骤 */
rollback: // 因为某步骤操作失败而回滚
    switch (rollback_step)
    {
    case 3:
        // 回收块
        bitmap_set(&cur_part->block_bitmap, block_bitmap_idx, 0);
    case 2:
        bitmap_set(&cur_part->inode_bitmap, inode_no, 0); // 如果新文件的inode创建失败,之前位图中分配的inode_no也要恢复
    case 1:
        /* 关闭所创建目录的父目录 */
        dir_close(searched_record.parent_dir);
        break;
    }
    sys_free(io_buf);
    return -1;
}

/**
 * @description: 打开目录, 把目录的inode放到内存
 * @param char* pathname 需要打开目录的路径名称
 * @return dir_t* 若打开成功, 则返回目录指针; 若打开失败则返回NULL
 */
struct dir *sys_opendir(const char *pathname)
{
    ASSERT(strlen(pathname) < MAX_FILE_NAME_LEN);

    dir_t *ret = NULL;

    // 根目录直接返回
    if (pathname[0] == '/' && (pathname[1] == '0' || pathname[1] == '.'))
        return &root_dir;

    path_search_record_t searched_record;
    memset(&searched_record, 0, sizeof(path_search_record_t));
    uint32_t inode_no = search_file(pathname, &searched_record);
    if (inode_no == -1)
    {
        printk("%s: In %s, sub path not exitsts\n", __func__, searched_record.searched_path);
    }
    else
    {
        if (searched_record.file_type == FT_REGULAR)
            printk("%s: %s is regular file\n", __func__, pathname, searched_record.searched_path);
        else if (searched_record.file_type == FT_DIRECTORY)
        { // 目录存在
            ret = dir_open(cur_part, inode_no);
        }
    }

    dir_close(searched_record.parent_dir);
    return ret;
}

/**
 * @brief sys_closedir是closedir系统调用的实现函数. 用于关闭一个文件夹
 *
 * @param dir 需要关闭的文件夹
 * @return int32_t 若关闭成功, 则返回0; 若关闭失败, 则返回-1
 */
int32_t sys_closedir(struct dir *dir)
{
    int32_t ret = -1;
    if (dir != NULL)
    {
        dir_close(dir);
        ret = 0;
    }
    return ret;
}

/**
 * @brief 读取一个目录项, dir_read的套壳
 *
 * @param dir 被读目录
 * @return dir_entry_t* 目录项指针
 */
struct dir_entry *sys_readdir(struct dir *dir)
{
    ASSERT(dir != NULL);
    return dir_read(dir);
}

/**
 * @brief 使得dir_pos为0
 *
 * @param dir 操作的目录结构体指针
 */
void sys_rewinddir(struct dir *dir)
{
    dir->dir_pos = 0;
}

/**
 * @brief 删除目录
 *
 * @param pathname 被删除的文件的路径名字
 * @return int32_t 成功返回0, 失败返回 -1
 */
int32_t sys_rmdir(const char *pathname)
{
    // 找文件
    path_search_record_t searched_record;
    memset(&searched_record, 0, sizeof(path_search_record_t));
    int32_t inode_no = search_file(pathname, &searched_record);

    ASSERT(inode_no != 0); // 0 是根目录
    int32_t ret_val = -1;

    if (inode_no == -1)
    {
        printk("%s: In %s, subpath %s not exitsts!\n", __func__, pathname, searched_record.searched_path);
    }
    else
    {
        if (searched_record.file_type == FT_REGULAR)
        {
            printk("%s: %s is regular file!\n", __func__, pathname);
        }
        else
        {
            dir_t *dir = dir_open(cur_part, inode_no);
            if (!dir_is_empty(dir))
            {
                printk("%s: dir %s is not empty!!!\n", __func__, pathname);
            }
            else
            {
                if (!dir_remove(searched_record.parent_dir, dir))
                    ret_val = 0;
            }
            dir_close(dir);
        }
    }
    dir_close(searched_record.parent_dir);
    return ret_val;
}

/**
 * @brief get_parent_dir_inode_nr用于获得获取目录所在的父目录的inode编号.
 *        原理就是子目录中的'..'目录项存储了父目录的inode号, 所以读取子目录的内容,
 *        然后返回其中文件名为'..'的这个目录项的inode号即可
 *
 * @param child_inode_no 文件的inode号
 * @param io_buf 由调用者提供的io_buf, 用于读写硬盘用
 * @return uint32_t 文件所在的父目录的inode编号
 */
static uint32_t get_parent_dir_inode_nr(uint32_t child_inode_nr, void *io_buf)
{
    struct inode *child_dir_inode = inode_open(cur_part, child_inode_nr);
    // 目录中的目录项 “..” 中包括父亲目录 inode 编号, ".."位于目录的 第0块
    uint32_t block_lba = child_dir_inode->i_sectors[0];
    ASSERT(block_lba >= cur_part->sb->data_start_lba);
    inode_close(child_dir_inode);

    ide_read(cur_part->my_disk, block_lba, io_buf, 1);
    struct dir_entry *dir_e = (struct dir_entry *)io_buf;

    // 创建文件后, 第一个目录项是"." 第二个是".."
    ASSERT(dir_e[1].i_no < 4096 && dir_e[1].f_type == FT_DIRECTORY);
    return dir_e[1].i_no;
}

/**
 * @brief get_child_dir_name用于在编号为p_inode_no的目录中循找inode编号为c_inode_no的子目录的名称,
 *        名称将存入path中
 *
 * @param p_inode_no 要搜索的父目录的inode编号
 * @param c_inode_no 要寻找的子目录的inode编号
 * @param path inode编号为c_inode_no的子目录名称
 * @param io_buf 调用者提供的读取硬盘时候的io_buf
 * @return int32_t 若读取成功则返回0, 失败则返回-1
 */
static int32_t get_child_dir_name(uint32_t p_inode_nr, uint32_t c_inode_nr, char *path, void *io_buf)
{
    struct inode *parent_dir_inode = inode_open(cur_part, p_inode_nr);
    // 得到所有块地址
    uint32_t block_idx = 0, block_cnt = 12;
    uint32_t all_blocks_lba[140] = {0};
    // 直接块
    while (block_idx < block_cnt)
    {
        all_blocks_lba[block_idx] = parent_dir_inode->i_sectors[block_idx];
        block_idx++;
    }
    // 间接块
    if (parent_dir_inode->i_sectors[12])
    {
        ide_read(cur_part->my_disk, parent_dir_inode->i_sectors[12], all_blocks_lba + 12, 1);
        block_cnt = 140;
    }
    inode_close(parent_dir_inode);

    block_idx = 0;
    dir_entry_t *de = (dir_entry_t *)io_buf;
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
    uint32_t dir_entry_pre_sec = SECTOR_SIZE / dir_entry_size;
    while (block_idx < block_cnt)
    {
        if (all_blocks_lba[block_idx])
        {
            ide_read(cur_part->my_disk, all_blocks_lba[block_idx], io_buf, 1);
            // 逐个遍历目录项
            uint32_t de_idx = 0;
            while (de_idx < dir_entry_pre_sec)
            {
                if ((de + de_idx)->i_no == c_inode_nr)
                {
                    strcat(path, "/");
                    strcat(path, (de + de_idx)->filename);
                    return 0;
                }
                de_idx++;
            }
        }
        block_idx++;
    }
    return -1;
}

/**
 * @brief sys_getcwd用于将当前运行线程的工作目录的绝对路径写入到buf中
 *
 * @param buf 由调用者提供存储工作目录路径的缓冲区, 若为NULL则将由操作系统进行分配
 * @param size 若调用者提供buf, 则size为buf的大小
 * @return char* 若成功且buf为NULL, 则操作系统会分配存储工作目录路径的缓冲区, 并返回首地址; 若失败则为NULL
 */
char *sys_getcwd(char *buf, uint32_t size)
{
    /* 确保buf不为空,若用户进程提供的buf为NULL,
   系统调用getcwd中要为用户进程通过malloc分配内存 */
    ASSERT(buf != NULL);
    void *io_buf = sys_malloc(SECTOR_SIZE * 3);
    if (io_buf == NULL)
        return NULL;

    struct task_struct *cur_thread = running_thread();
    int32_t parent_inode_no = 0;
    int32_t child_inode_no = cur_thread->cwd_inode_no; // 进程的工作目录
    ASSERT(child_inode_no >= 0 && child_inode_no < 4096);

    /* 若当前目录是根目录,直接返回'/' */
    if (child_inode_no == 0)
    {
        buf[0] = '/';
        buf[1] = 0;
        return buf;
    }

    memset(buf, 0, size);
    char full_path_reverse[MAX_PATH_LEN] = {0};
    // 从子目录开始逐层向上找, 一直找到根目录为止, 每次查找都会把当前的目录名复制到full_path_reverse中
    // 例如子目录现在是"/fd1/fd1.1/fd1.1.1/fd1.1.1.1",
    // 则运行结束之后, full_path_reverse为 "/fd1.1.1.1/fd1.1.1/fd1.1/fd1"
    while (child_inode_no)
    {
        parent_inode_no = get_parent_dir_inode_nr(child_inode_no, io_buf);
        if (get_child_dir_name(parent_inode_no, child_inode_no, full_path_reverse, io_buf) == -1)
        { // 或未找到名字,失败退出
            sys_free(io_buf);
            printk("%s:get name faild!!!", __func__);
            return NULL;
        }
        child_inode_no = parent_inode_no;
    }
    ASSERT(strlen(full_path_reverse) <= size);

    /* 至此full_path_reverse中的路径是反着的,
     * 即子目录在前(左),父目录在后(右) ,
     * 现将full_path_reverse中的路径反置 */
    char *last_slash; // 用于记录字符串中最后一个 / 的地址

    // 把full_path_reverse从后向前遇见 / 就截断一下
    // 添加到buf尾巴后面
    while ((last_slash = strrchr(full_path_reverse, '/')))
    {
        uint16_t len = strlen(buf);

        strcpy(buf + len, last_slash);
        // 最后一位设置为0, 这样就是下一次strrchr就从这里开始查起
        *last_slash = 0;
    }
    sys_free(io_buf);

    return buf;
}

/**
 * @brief 更改工作目录位path
 *
 * @param path 绝对路径
 * @return int32_t 成功返回 0, 失败返回 -1
 */
int32_t sys_chdir(const char *path)
{
    int32_t ret = -1;
    path_search_record_t searched_record;
    memset(&searched_record, 0, sizeof(path_search_record_t));
    int32_t inode_no = search_file(path, &searched_record);
    if (inode_no != -1)
    {
        if (searched_record.file_type != FT_REGULAR)
        {
            running_thread()->cwd_inode_no = inode_no;
            ret = 0;
        }
        else
            printk("%s : %s is regular file or other!\n", __func__, path);
    }
    dir_close(searched_record.parent_dir);
    return ret;
}

/**
 * @brief 获得文件path的属性 存储在buf中
 *
 * @param path 文件路径
 * @param buf 保存文件属性的结构体
 * @return int32_t 正确返回0 失败返回-1
 */
int32_t sys_stat(const char *path, struct stat *buf)
{
    // 若path是根目录
    if (!strcmp(path, "/") || !strcmp(path, "/.") || !strcmp(path, "/.."))
    {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_ino = 0;
        buf->st_size = root_dir.inode->i_size;
        return 0;
    }

    // 找文件
    int ret = -1;
    struct path_search_record search_record;
    memset(&search_record, 0, sizeof(search_record));
    int inode_no = search_file(path, &search_record);
    if (inode_no != -1)
    {
        inode_t *objInode = inode_open(cur_part, inode_no);
        buf->st_size = objInode->i_size; // 得到文件大小
        inode_close(objInode);
        buf->st_filetype = search_record.file_type;
        buf->st_ino = inode_no;
        ret = 0;
    }
    else
    {
        // printk("sys_stat: %s not found! \n", path);
    }

    dir_close(search_record.parent_dir);
    return ret;
}
