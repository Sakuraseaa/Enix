#include "ide.h"
#include "sync.h"
#include "stdio.h"
#include "stdio-kernel.h"
#include "interrupt.h"
#include "memory.h"
#include "debug.h"
#include "string.h"
#include "timer.h"
#include "io.h"
#include "string.h"
#include "list.h"
#include "console.h"

/* 定义硬盘各寄存器的端口号 1=读操作时, 2=写操作*/
#define reg_data(channel) (channel->port_base + 0)     // 读操作时.该寄存器存储数据,写操作.该寄存器存储数据 16位
#define reg_error(channel) (channel->port_base + 1)    // 读操作时.读失败的时候,记录失败信息, 写操作时,存储写的参数
#define reg_sect_cnt(channel) (channel->port_base + 2) // 用来指定待读取OR待写入的扇区数
#define reg_lba_l(channel) (channel->port_base + 3)    // 存储28位逻辑块地址 8位
#define reg_lba_m(channel) (channel->port_base + 4)    // 8位
#define reg_lba_h(channel) (channel->port_base + 5)    // 8位
// 低4位表示地址(8+8+8+4=28), 第四位表示通道上是0主盘还是1从盘,第6位设置LBA,逻辑块地址。第5，7位固定位1 MBS位
#define reg_dev(channel) (channel->port_base + 6)
// 1, 状态寄存器中有 0ERR, 3data request, 6DRDY 7BSY 分别表示硬盘是否出错,数据是否准备好了，硬盘是否可以正常工作,硬盘是否繁忙
#define reg_status(channel) (channel->port_base + 7)
// 2. 命令寄存器，再里面写入 0xec 表示硬盘识别
#define reg_cmd(channel) (reg_status(channel))
// 控制块寄存器
#define reg_alt_status(channel) (channel->port_base + 0x206)
#define reg_ctl(channel) reg_alt_status(channel)

// reg_alt_status寄存器的一些关键位
#define BIT_ALT_STAT_BSY 0x80  // 硬盘忙
#define BIT_ALT_STAT_DRDY 0x40 // 驱动器准备好
#define BIT_ALT_STAT_DRQ 0x8   // 数据传输准备好了

/* device寄存器的一些关键位 */
#define BIT_DEV_MBS 0xa0 // 第7位和第5位固定为1
#define BIT_DEV_LBA 0x40
#define BIT_DEV_DEV 0x10

/* 一些硬盘操作的指令 */
#define CMD_IDENTIFY 0xec     // identify指令
#define CMD_READ_SECTOR 0x20  // 读扇区指令
#define CMD_WRITE_SECTOR 0x30 // 写扇区指令

// 定义可读写的最大扇区数, 调试用的
#define max_lba ((80 * 1024 * 1024 / 512) - 1) // 只支持80MB硬盘
// 用于记录总扩展分区的起始lba, 初始化为0, 用来存分区表项
int32_t ext_lba_base = 0;
// 用来记录硬盘主分区和逻辑分区的下标
uint8_t p_no = 0, l_no = 0;
// 分区队列
struct list partition_list;

uint8_t channel_cnt;            // 按硬盘数计算的通道数
struct ide_channel channels[2]; // 有两个ide通道

// 构建一个16字节大小的结构体, 用来描述分区表项
struct partition_table_entry
{
    uint8_t bootable;   // 是否可引导
    uint8_t start_head; // 起始磁头号
    uint8_t start_sec;  // 起始扇区号
    uint8_t start_chs;  // 起始柱面号
    uint8_t fs_type;    // 分区类型
    uint8_t end_head;   // 结束磁头号
    uint8_t end_sec;    // 结束扇区号
    uint8_t end_chs;    // 结束柱面号

    uint32_t start_lba;    // 本分区起始扇区的lba地址
    uint32_t sec_cnt;      // 本分区的扇区数目
} __attribute__((packed)); // 阻止对齐

// 每个分区最开始446 + 64 + 2
struct boot_sector
{
    uint8_t other[446];                              // 引导代码
    struct partition_table_entry partition_table[4]; // 分区表中有4项, 共64字节
    uint16_t signature;
} __attribute__((packed)); // 启动扇区的结束标志是0x55,0xaa,

// 将dst中len个相邻字节交换位置后存入buf
static void swap_pairs_bytes(const char *dst, char *buf, uint32_t len)
{
    uint8_t idx;
    for (idx = 0; idx < len; idx += 2)
    {
        // buf中存储dst中两相邻元素交换位置后的字符串
        buf[idx + 1] = *dst++;
        buf[idx] = *dst++;
    }
    buf[idx] = '\0';
}

// 选择读写的硬盘
static void select_disk(struct disk *hd)
{
    uint8_t reg_device = BIT_DEV_MBS | BIT_DEV_LBA;
    if (hd->dev_no == 1) // 若是从盘就置DEV位为1
        reg_device |= BIT_DEV_DEV;

    outb(reg_dev(hd->my_channel), reg_device);
}

/* 向硬盘控制器写入要操作的扇区地址及要读写的扇区数 */
static void select_sector(struct disk *hd, uint32_t lba, uint8_t sec_cnt)
{
    ASSERT(lba <= max_lba);
    struct ide_channel *channel = hd->my_channel;

    // 写入要读写的扇区数
    outb(reg_sect_cnt(channel), sec_cnt);

    // 写入lba地址, 扇区号
    outb(reg_lba_l(channel), lba);       // lba地址的低8位,不用单独取出低8位.outb函数中的汇编指令outb %b0, %w1会只用al。
    outb(reg_lba_m(channel), lba >> 8);  // lba地址的8~15位
    outb(reg_lba_h(channel), lba >> 16); // lba地址的16~23位

    /* 因为lba地址的24~27位要存储在device寄存器的0～3位,
     无法单独写入这4位,所以在此处把device寄存器再重新写入一次*/
    outb(reg_dev(channel), BIT_DEV_MBS | BIT_DEV_LBA | (hd->dev_no == 1 ? BIT_DEV_DEV : 0) | lba >> 24);
}

/* 向通道channel发命令cmd */
static void cmd_out(struct ide_channel *channel, uint8_t cmd)
{
    /* 只要向硬盘发出了命令便将此标记置为true,硬盘中断处理程序需要根据它来判断 */
    channel->expecting_intr = true;
    outb(reg_cmd(channel), cmd);
}

/* 硬盘读入sec_cnt个扇区的数据到buf */
static void read_from_sector(struct disk *hd, void *buf, uint8_t sec_cnt)
{
    uint32_t size_in_byte;
    if (sec_cnt == 0)
    {
        /* 因为sec_cnt是8位变量,由主调函数将其赋值时,若为256则会将最高位的1丢掉变为0 */
        size_in_byte = 256 * 512;
    }
    else
        size_in_byte = sec_cnt * 512;

    // 一次读一个字, 2个字节
    insw(reg_data(hd->my_channel), buf, size_in_byte / 2);
}

/* 将buf中sec_cnt扇区的数据写入硬盘 */
static void write2sector(struct disk *hd, void *buf, uint8_t sec_cnt)
{
    uint32_t size_in_byte;
    if (sec_cnt == 0)
    {
        /* 因为sec_cnt是8位变量,由主调函数将其赋值时,若为256则会将最高位的1丢掉变为0 */
        size_in_byte = 256 * 512;
    }
    else
        size_in_byte = sec_cnt * 512;

    // 一次写一个字, 2个字节
    outsw(reg_data(hd->my_channel), buf, size_in_byte / 2);
}

// 等待30ms,硬盘正常工作返回true, 否则返回false
static bool busy_wait(struct disk *hd)
{
    struct ide_channel *channel = hd->my_channel;
    uint16_t time_limit = 32 * 1000; // 30s 转换成 30000毫秒
    while (time_limit -= 10 >= 0)
    {
        if (!(inb(reg_status(channel)) & BIT_ALT_STAT_BSY))
            return (inb(reg_status(channel)) & BIT_ALT_STAT_DRQ);
        else
            mtime_sleep(10);
    }
    return false;
}

/* 从硬盘hd位置为lba的地址, 读取sec_cnt个扇区到buf */
void ide_read(struct disk *hd, uint32_t lba, void *buf, uint32_t sec_cnt)
{
    ASSERT(lba <= max_lba);
    ASSERT(sec_cnt > 0);

    lock_acquire(&hd->my_channel->lock);
    /* 1 先选择操作的硬盘 */
    select_disk(hd);

    uint32_t secs_done = 0; // 已经完成的扇区数
    uint32_t secs_op = 0;   // 每次硬盘操作的扇区数

    while (secs_done < sec_cnt)
    {
        if ((sec_cnt - secs_done) >= 256)
            secs_op = 256;
        else
            secs_op = sec_cnt - secs_done;

        /* 2 写入待读取的扇区数和起始扇区号 */
        select_sector(hd, lba + secs_done, secs_op);
        /* 3 执行的命令写入reg_cmd寄存器 */
        cmd_out(hd->my_channel, CMD_READ_SECTOR); // 准备开始读数据

        /*********************   阻塞自己的时机  ***********************
        在硬盘已经开始工作(开始在内部读数据或写数据)后才能阻塞自己,现在硬盘已经开始忙了,
        将自己阻塞,等待硬盘完成读操作后通过中断处理程序唤醒自己*/
        sema_down(&hd->my_channel->disk_done);
        /*************************************************************/

        /* 4 检测硬盘状态是否可读 */
        if (!busy_wait(hd))
        { // 硬盘出错了
            char error[64];
            sprintf(error, "%s read sector %d failed!!!\n", hd->name, lba);
            PANIC(error);
        }

        /* 5 把数据从硬盘的缓冲区中读出 */
        read_from_sector(hd, (void *)((uint32_t)buf + secs_done * 512), secs_op);
        secs_done += secs_op;
    }

    lock_release(&hd->my_channel->lock);
}

// 将buf中sec_cnt中扇区数据写入硬盘
void ide_write(struct disk *hd, uint32_t lba, void *buf, uint32_t sec_cnt)
{
    ASSERT(sec_cnt <= max_lba);
    ASSERT(sec_cnt > 0);

    lock_acquire(&hd->my_channel->lock);
    /* 1 先选择操作的硬盘 */
    select_disk(hd);

    uint32_t secs_done = 0; // 已经完成的扇区数
    uint32_t secs_op = 0;   // 每次硬盘操作的扇区数

    while (secs_done < sec_cnt)
    {
        if ((sec_cnt - secs_done) >= 256)
            secs_op = 256;
        else
            secs_op = sec_cnt - secs_done;

        /* 2 写入待读取的扇区数和起始扇区号 */
        select_sector(hd, lba + secs_done, secs_op);
        /* 3 执行的命令写入reg_cmd寄存器 */
        cmd_out(hd->my_channel, CMD_WRITE_SECTOR); // 准备开始写数据

        /* 4 检测硬盘状态是否可写 */
        if (!busy_wait(hd))
        { // 硬盘出错了
            char error[64];
            sprintf(error, "%s read sector %d failed!!!\n", hd->name, lba);
            PANIC(error);
        }

        /* 5 将数据写入硬盘 */
        write2sector(hd, (void *)((uint32_t)buf + secs_done * 512), secs_op);

        // 在硬盘相应期间阻塞自己
        sema_down(&hd->my_channel->disk_done);

        secs_done += secs_op;
    }
    // 醒来后释放锁
    lock_release(&hd->my_channel->lock);
}

// 硬盘中断处理程序
void intr_hd_handler(uint8_t irq_no)
{
    ASSERT(irq_no == 0x2e || irq_no == 0x2f);
    uint8_t ch_no = irq_no - 0x2e; // 根据中断号, 来确定是通道1还是通道2
    struct ide_channel *channel = &channels[ch_no];
    ASSERT(channel->irq_no == irq_no)
    /* 不必担心此中断是否对应的是这一次的expecting_intr,
     * 每次读写硬盘时会申请锁,从而保证了同步一致性 */
    if (channel->expecting_intr)
    {
        channel->expecting_intr = false;
        sema_up(&channel->disk_done);

        /*读取状态寄存器使硬盘控制器认为此次的中断已被处理,从而硬盘可以继续执行新的读写 */
        inb(reg_status(channel));
    }
}

/* 扫描硬盘hd中地址为ext_lba的扇区中的分区表信息 */
// 这个递归函数竟然没有明显的递归终止条件? hh, 分区表中有类型为0x5的子扩展分区，就说明有新的扩展分区
// 那么就去扫描分区表信息，存储在我们的硬盘结构体的分区结构体指针里面。没有子扩展分区了，递归也就终止了
static void partition_scan(struct disk *hd, uint32_t ext_lba)
{
    // MBR, EBR引导扇区指针, 512字节,如果在栈中申请该空间太大了，可能破环数据(我们的栈内存 小于 4096)
    // 所以在堆里面申请
    struct boot_sector *bs = sys_malloc(sizeof(struct boot_sector));
    ide_read(hd, ext_lba, bs, 1);
    uint8_t part_idx = 0;
    // 分区表指针
    struct partition_table_entry *p = bs->partition_table;

    /* 遍历分区表4个分区表项 */
    while (part_idx++ < 4)
    {
        if (p->fs_type == 0x05)
        {
            if (ext_lba_base != 0)
            {
                /* 子扩展分区的start_lba是相对于主引导扇区中的总扩展分区地址 */
                partition_scan(hd, p->start_lba + ext_lba_base);
            }
            else
            {
                // ext_lba_base为0表示是第一次读取引导块,也就是主引导记录所在的扇区，MBR
                /* 记录下扩展分区的起始lba地址,后面所有的扩展分区地址都相对于此 */
                ext_lba_base = p->start_lba;
                partition_scan(hd, p->start_lba);
            }
        }
        else if (p->fs_type != 0)
        {
            if (ext_lba == 0)
            {
                // 此时全是主分区,0,1,2,3
                hd->prim_parts[p_no].start_lba = ext_lba + p->start_lba;
                hd->prim_parts[p_no].sec_cnt = p->sec_cnt;
                hd->prim_parts[p_no].my_disk = hd;
                list_append(&partition_list, &hd->prim_parts[p_no].part_tag);
                sprintf(hd->prim_parts[p_no].name, "%s%d", hd->name, p_no + 1);
                p_no++;
                ASSERT(p_no < 4);
            }
            else
            {
                // 只支持8个逻辑分区,避免数组越界
                hd->logic_parts[l_no].start_lba = ext_lba + p->start_lba;
                hd->logic_parts[l_no].sec_cnt = p->sec_cnt;
                hd->logic_parts[l_no].my_disk = hd;
                list_append(&partition_list, &hd->logic_parts[l_no].part_tag);
                sprintf(hd->logic_parts[l_no].name, "%s%d", hd->name, l_no + 5);
                l_no++;
                if (l_no >= 8)
                    return;
            }
        }
        p++;
    }
    sys_free(bs);
}

/* 打印分区信息 */
static bool partition_info(struct list_elem *pelem, int arg)
{
    struct partition *part = elem2entry(struct partition, part_tag, pelem);
    printk("    %s start_lba:0x%x, sec_cnt:0x%x\n", part->name, part->start_lba, part->sec_cnt);

    /* 在此处return false与函数本身功能无关,
    只是为了让主调函数list_traversal继续向下遍历元素 */
    return false;
}

// 获得硬盘参数信息
static void identify_disk(struct disk *hd)
{
    char id_info[512];
    select_disk(hd);
    cmd_out(hd->my_channel, CMD_IDENTIFY);

    /* 向硬盘发送指令后便通过信号量阻塞自己,
     * 待硬盘处理完成后,通过中断处理程序将自己唤醒 */
    sema_down(&hd->my_channel->disk_done);

    /* 醒来后开始执行下面代码*/
    if (!busy_wait(hd))
    {
        //  若失败
        char error[64];
        sprintf(error, "%s identify failed!!!!!!\n", hd->name);
        PANIC(error);
    }
    read_from_sector(hd, id_info, 1);

    // 此时硬盘的数据已经存储在id_info中, 我们只挑选
    // 硬盘序列号(第10个字开始, 20字节), 硬盘型号(第27个字开始, 40字节), 可供用户使用的扇区数(第60个字开始, 2字节)
    char buf[64];
    uint8_t sn_start = 10 * 2, sn_len = 20, md_start = 27 * 2, md_len = 40;
    // 硬盘序列号
    swap_pairs_bytes(&id_info[sn_start], buf, sn_len);
    printk("    disk %s info:\n     SN: %s\n", hd->name, buf);
    memset(buf, 0, sizeof(buf));
    // 硬盘型号
    swap_pairs_bytes(&id_info[md_start], buf, md_len);
    printk("    MODULE:%s\n", buf);
    // 可供用户使用的扇区数
    uint32_t sectors = *(uint32_t *)&id_info[60 * 2];
    printk("    SECTORS:%d\n", sectors);
    printk("    CAPACITY:%dMB\n", sectors * 512 / 1024 / 1024);
}

// 硬盘数据结构初始化
void ide_init()
{
    printk("ide_init start\n");
    // 获取硬盘的数量,BIOS识别出来的
    uint8_t hd_cnt = *((uint8_t *)(0x475));
    printk("   ide_init hd_cnt:%d\n", hd_cnt);
    ASSERT(hd_cnt > 0);
    list_init(&partition_list);
    // 一个ide通道上有两个硬盘,根据硬盘数量反推有几个ide通道
    channel_cnt = DIV_ROUND_UP(hd_cnt, 2);

    struct ide_channel *channel;
    uint8_t channel_no = 0, dev_no = 0;

    // 遍历通道,我们的系统只用到了第一个通道连接的两块硬盘
    while (channel_no < channel_cnt)
    {
        channel = channels + channel_no;
        sprintf(channel->name, "ide%d", channel_no);

        // 为每个ide通道初始化端口基质及中断向量
        switch (channel_no)
        {
        case 0:
            channel->port_base = 0x1f0;  // 一个ide通道上有两个硬盘,根据硬盘数量反推有几个ide通道
            channel->irq_no = 0x20 + 14; // 从片8259a上倒数第二的中断引脚,硬盘,也就是ide0通道的的中断向量号
            break;
        case 1:
            channel->port_base = 0x170;  // ide1通道的起始端口号是0x170
            channel->irq_no = 0x20 + 15; // 从8259A上的最后一个中断引脚,我们用来响应ide1通道上的硬盘中断
            break;
        }

        channel->expecting_intr = false;
        lock_init(&channel->lock);

        /* 初始化为0,目的是向硬盘控制器请求数据后,硬盘驱动sema_down此信号量会阻塞线程,
        直到硬盘完成后通过发中断,由中断处理程序将此信号量sema_up,唤醒线程. */
        sema_init(&channel->disk_done, 0);

        register_handler(channel->irq_no, intr_hd_handler);

        /*一个通道连接两块硬盘,获取两个硬盘的参数及分区信息 */
        while (dev_no < 2)
        {
            struct disk *hd = &channel->devices[dev_no];
            hd->my_channel = channel;
            hd->dev_no = dev_no;
            /*
                硬盘命名规则[x]d[y][n]
                [x]: s = SCSI磁盘, h = IDE磁盘
                d: disk
                y: 区分第几个设备，a是1, b是2, c是3, 以此类推
                n: 分区号, 数字1开始
            */
            sprintf(hd->name, "sd%c", 'a' + channel_no * 2 + dev_no);
            // 获取硬盘参数
            identify_disk(hd);
            if (dev_no != 0)
            {
                // 内核本身的裸硬盘(hd60M.img)不处理
                // 扫描该硬盘上的分区
                partition_scan(hd, 0);
            }
            p_no = 0, l_no = 0;
            dev_no++;
        }
        dev_no = 0;
        channel_no++; // 下一个channel
    }
    printk("\n all partition info\n");
    // 打印所有分区信息
    list_traversal(&partition_list, partition_info, (int)NULL);
    printk("ide_init done\n");
}
