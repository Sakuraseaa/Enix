#ifndef __INODE_H
#define __INODE_H
#include "stdint.h"
#include "list.h"
#include "ide.h"

// inode结构,文件最大支持140个扇区, 70MB
struct inode
{
    uint32_t i_no; // inode 编号

    /* 当此inode是文件时,i_size是指文件大小,
    若此inode是目录,i_size是指该目录下所有目录项大小之和*/
    uint32_t i_size;

    uint32_t i_open_cnts; // 记录此文件被打开的次数
    bool write_deny;      // 写文件不能并行, 进程写文件前检查此标志

    uint32_t i_sectors[13]; /* i_sectors[0-11]是直接块, i_sectors[12]用来存储一级间接块指针 */
    struct list_elem inode_tag;
};
typedef struct inode inode_t;
struct inode *inode_open(struct partition *part, uint32_t inode_no);
void inode_sync(struct partition *part, struct inode *inode, void *io_buf);
void inode_init(uint32_t inode_no, struct inode *new_inode);
void inode_close(struct inode *inode);
void inode_release(struct partition *part, uint32_t inode_no);
#endif