#ifndef __DEVICE_IDE_H
#define __DEVICE_IDE_H
#include "stdint.h"
#include "sync.h"
#include "bitmap.h"

// 描述分区
struct partition
{
    uint32_t start_lba;         // 起始扇区
    uint32_t sec_cnt;           // 扇区数
    struct disk *my_disk;       // 分区所属的硬盘
    struct list_elem part_tag;  // 用于队列中的标记
    char name[8];               // 分区名
    struct super_block *sb;     // 本分区的超级块
    struct bitmap block_bitmap; // 块位图
    struct bitmap inode_bitmap; // i节点位图
    struct list open_inodes;    // 本分区打开的i节点队列
};

// 描述硬盘
struct disk
{
    char name[8];                    // 本硬盘的名称
    struct ide_channel *my_channel;  // 本硬盘归属与哪个通道
    uint8_t dev_no;                  // 区分本硬盘是主盘还是从盘,主0从1
    struct partition prim_parts[4];  // 主分区顶多是4个
    struct partition logic_parts[8]; // 逻辑分区数量无限，我们设置支持8个
};

// 描述ata通道结构
struct ide_channel
{
    char name[8];               // 本ata通道名称
    uint16_t port_base;         // 本通道的端口基址
    uint8_t irq_no;             // 本通道所用的中断号, 可区分是Primary通道, 还是secondary通道
    struct lock lock;           // 通道锁
    bool expecting_intr;        // 表示等待硬盘的中断
    struct semaphore disk_done; // 用于阻塞, 唤醒驱动程序
    struct disk devices[2];     // 通道连接的两个硬盘一主一从
};

void ide_init(void);
void ide_read(struct disk *hd, uint32_t lba, void *buf, uint32_t sec_cnt);
void ide_write(struct disk *hd, uint32_t lba, void *buf, uint32_t sec_cnt);
void intr_hd_handler(uint8_t irq_no);
extern struct ide_channel channels[2];

#endif