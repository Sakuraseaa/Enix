/*
 * @date:
 */
#ifndef __FS_SUPER_BLOCK_H
#define __FS_SUPER_BLOCK_H
#include "stdint.h"

// 我们为了方便, 设置1块等于1扇区

// 超级块
typedef struct super_block
{
    uint32_t magic; // 魔数, 用于标志文件系统类型

    uint32_t sec_cnt;       // section_count, 本分区总共的扇区数
    uint32_t inode_cnt;     // indexNode_count(索引节点,索引,跟踪一个文件的所有块)本分区中inode的数量
    uint32_t part_lba_base; // Logical Block Address, 本分区的起始lba地址

    uint32_t block_bitmap_lba;   // 块位图起始扇区地址
    uint32_t block_bitmap_sects; // 块位图占用的扇区数量

    uint32_t inode_bitmap_lba;   // i结点位图起始扇区lba地址
    uint32_t inode_bitmap_sects; // i结点位图占用的扇区数量

    uint32_t inode_table_lba;   // inode数组起始扇区lba地址
    uint32_t inode_table_sects; // inode数组占用的扇区数量

    uint32_t data_start_lba; // 数据区开始的第一个扇区号
    uint32_t root_inode_no;  // 根目录所在的I结点号
    uint32_t dir_entry_size; // 目录项大小,Directory

    uint8_t pad[460]; // 加上460字节,凑够512字节1扇区大小
} __attribute__((packed)) super_block_t;

#endif