#ifndef __FS_FS_H
#define __FS_FS_H
#include "stdint.h"

#define MAX_FILES_PER_PART 4096 // 每个分区所支持最大创建的文件数
#define BITS_PER_SECTOR 4096    // 每扇区的位数
#define SECTOR_SIZE 512         // 扇区字节大小
#define BLOCK_SIZE SECTOR_SIZE  // 块字节大小
#define MAX_PATH_LEN 512        //  路径最大长度

// 文件类型
enum file_types
{
    FT_UNKNOWN,  // 未知
    FT_REGULAR,  // 普通文件
    FT_DIRECTORY // 目录
};

// 打开文件的选项
enum oflags
{
    O_RDONLY,   // 只读
    O_WRONLY,   // 只写
    O_RDWR,     // 读写
    O_CREAT = 4 // 创建
};

// 文件属性结构体
struct stat
{
    uint32_t st_ino;             // inode 编号
    uint32_t st_size;            // 尺寸
    enum file_types st_filetype; // 文件类型
};

// 文件读写位置偏移量
enum whence
{
    SEEK_SET = 1,
    SEEK_CUR,
    SEEK_END,
};

/* 路径搜索记录结构体, 用来记录查找文件过程中已找到的上级路径,也就是查找文件过程中"走过的地方" */
struct path_search_record
{
    char searched_path[MAX_PATH_LEN]; // 查找过程中的父路径
    struct dir *parent_dir;           // 文件或目录所在的直接父目录
    // 找到的是普通文件还是目录,找不到设为未知类型(FT_UNKNOWN)
    enum file_types file_type;
};

typedef struct path_search_record path_search_record_t;

/**
 * @brief fd_local2global将进程的文件描述符 转换到文件表下标
 *
 * @param local_fd 文件描述符
 * @return uint32_t 全局文件表的下标
 */
uint32_t fd_local2global(uint32_t local_fd);
void filesys_init(void);
int32_t path_depth_cnt(char *pathname);
char *path_parse(char *pathname, char *name_store);
int32_t sys_open(const char *pathname, uint8_t flags);
int32_t sys_close(int32_t fd);
int32_t sys_write(int32_t fd, const void *buf, uint32_t count);
int32_t sys_read(int32_t fd, void *buf, uint32_t count);
int32_t sys_Iseek(int32_t fd, int32_t offset, uint8_t whence);
int32_t sys_unlink(const char *pathname);
int32_t sys_mkdir(const char *path);
struct dir *sys_opendir(const char *pathname);
int32_t sys_closedir(struct dir *dir);
void sys_rewinddir(struct dir *dir);
struct dir_entry *sys_readdir(struct dir *dir);
int32_t sys_rmdir(const char *pathname);
char *sys_getcwd(char *buf, uint32_t size);
int32_t sys_chdir(const char *path);
int32_t sys_stat(const char *path, struct stat *buf);

extern struct partition *cur_part;

#endif