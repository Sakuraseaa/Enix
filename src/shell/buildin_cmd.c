#include "buildin_cmd.h"
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "fs.h"
#include "global.h"
#include "dir.h"
#include "shell.h"
#include "assert.h"

extern char final_path[MAX_PATH_LEN];

/**
 * @brief wash_path用于将包含相对路径的old_path转换为绝对路径后存入new_abs_path.
 *        例如将 /a/b/../c/./d 转换为/a/c/d
 *
 * @param old_abs_path 包含相对路径的old_path
 * @param new_abs_path 新的绝对路径
 */
static void wash_path(char *old_abs_path, char *new_abs_path)
{
    assert(old_abs_path[0] == '/');

    char name[MAX_FILE_NAME_LEN] = {0};
    char *sub_path = old_abs_path;
    sub_path = path_parse(sub_path, name);
    // 只输入了 '/'
    if (name[0] == 0)
    {
        new_abs_path[0] = '/';
        new_abs_path[1] = 0;
        return;
    }

    // 将new_abs_path "清空"
    new_abs_path[0] = 0;
    // 拼接根目录
    strcat(new_abs_path, "/");

    // 逐层向下遍历目录
    while (name[0])
    {
        // 如果当前目录是上级目录，则寻找上一个'/',然后删除上一个'/'的内容
        // 比如‘/a/b/..’ 设置为 ‘/a’
        if (!strcmp("..", name))
        {
            char *slash_ptr = strrchr(new_abs_path, '/');
            // 如果没有找到根目录的'/', 则截断
            if (slash_ptr != new_abs_path)
                *slash_ptr = 0;
            // 如果已经找到了根目录, 则截断为'/0xxxxx'
            else
                *(slash_ptr + 1) = 0;
            // 当前目录不是 '.' ,就将name拼接到new_abs_path
        }
        else if (strcmp(".", name))
        {
            if (strcmp(new_abs_path, "/")) // 如果new_abs_path不是"/",就拼接一个"/",此处的判断是为了避免路径开头变成这样"//"
                strcat(new_abs_path, "/");

            strcat(new_abs_path, name);
        }

        // 准备下次一遍历
        memset(name, 0, MAX_FILE_NAME_LEN);
        if (sub_path)
            sub_path = path_parse(sub_path, name);
    }
}

/**
 * @brief make_clear_abs_path用于将包含相对路径的目录path('.'和'..')处理不含相对路径的目录, 并存入final_path中
 *        使用系统调用获得(当前工作目录) + path, 使用wash_path删除多于的 . 和 ..

 * @param path 用户传入的绝对路径
 * @param final_path  不包含相对路径的目录
 */
void make_clear_abs_path(char *path, char *final_path)
{
    char abs_path[MAX_PATH_LEN] = {0};

    if (path[0] != '/')
    {
        memset(abs_path, 0, sizeof(abs_path));
        if (getcwd(abs_path, MAX_PATH_LEN))
        {
            if (!(abs_path[0] == '/' && abs_path[1] == 0))
                strcat(abs_path, "/");
        }
    }
    strcat(abs_path, path);
    wash_path(abs_path, final_path);
}

void make_default_path(char *path, char *final_path)
{
    memset(final_path, 0, MAX_PATH_LEN);
    char abs_path[MAX_PATH_LEN] = {0};
    memset(abs_path, 0, sizeof(abs_path));
    strcat(abs_path, "/"); // 根目录就是默认路径
    strcat(abs_path, path);
    wash_path(abs_path, final_path);
}

/**
 * @brief builtin_pwd是pwd内建命令的实现函数
 *
 * @param argc 参数个数, 用不到, 统一格式用
 * @param argv 参数个数, 用不到, 统一格式用
 */
void buildin_pwd(uint32_t argc, char **argv)
{
    if (argc != 1)
    {
        printf("pwd: no argument suprot!\n");
        return;
    }
    if (getcwd(final_path, MAX_FILE_NAME_LEN))
        printf("%s\n", final_path);
    else
    {
        printf("pwd: get current work directory failed \n");
    }
}

/**
 * @brief buildin_cd是cd内建命令的实现函数
 *
 * @param argc 参数个数, cd命令支持一个参数
 * @param argv 参数个数, cd命令的参数要求是目标目录
 */
char *buildin_cd(uint32_t argc, char **argv)
{
    if (argc > 2)
    {
        printf("cd: only support 1 argument!\n");
        return NULL;
    }
    // 若只是键入cd而无参数，直接返回到更根目录
    if (argc == 1)
    {
        final_path[0] = '/';
        final_path[1] = 0;
    }
    else
        make_clear_abs_path(argv[1], final_path);

    if (chdir(final_path) == -1)
    {
        printf("cd: no such directory %s\n", final_path);
        return NULL;
    }
    return final_path;
}

/**
 * @brief builtin_ps是ps内置命令的实现函数
 *
 * @param argc 参数个数
 * @param argv 参数值
 */
void buildin_ps(uint32_t argc, char **argv)
{
    if (argc != 1)
    {
        printf("ps: no argument support!\n");
        return;
    }
    ps();
}

/**
 * @brief builtin_clear是clear系统调用的实现函数
 *
 * @param argc 参数个数
 * @param argv 参数值
 */
void buildin_clear(uint32_t argc, char **argv)
{
    if (argc != 1)
    {
        printf("ps: no argument support!\n");
        return;
    }
    clear();
}

/**
 * @brief builtin_mkdir是mkdir命令的内建函数
 *
 * @param argc 参数个数
 * @param argv 参数值
 * @return int32_t 若创建成功, 则返回0; 若创建失败, 则返回-1
 */
int32_t buildin_mkdir(uint32_t argc, char **argv)
{
    int32_t ret = -1;
    if (argc != 2)
        printf("mkdir: only support 1 argument!\n");
    else
    {
        make_clear_abs_path(argv[1], final_path);
        if (strcmp("/", final_path))
        {
            if (mkdir(final_path) == 0)
                ret = 0;
            else
                printf("mkdir: create directory %s failed.\n", argv[1]);
        }
    }
    return ret;
}

/**
 * @brief builtin_rmdir是rmdir的内建函数
 *
 * @param argc 参数个数
 * @param argv 参数值
 * @return int32_t 若删除成功, 则返回0; 若删除失败, 则返回-1
 */
int32_t buildin_rmdir(uint32_t argc, char **argv)
{
    int32_t ret = -1;
    if (argc != 2)
        printf("rmdir: only support 1 argument!\n");
    else
    {
        make_clear_abs_path(argv[1], final_path);
        if (strcmp("/", final_path))
        {
            if (rmdir(final_path) == 0)
                ret = 0;
            else
                printf("rmdir: create directory %s failed.\n", argv[1]);
        }
    }
    return ret;
}

/**
 * @brief builtin_rm是rm内置命令的实现函数
 *
 * @param argc 参数个数
 * @param argv 参数值
 * @return int32_t 若删除成功, 则返回0; 若删除失败, 则返回-1
 */
int32_t buildin_rm(uint32_t argc, char **argv)
{
    int32_t ret = -1;
    if (argc != 2)
        printf("rm: only support 1 argument!\n");
    else
    {
        make_clear_abs_path(argv[1], final_path);
        if (strcmp("/", final_path))
        {
            if (unlink(final_path) == 0)
                ret = 0;
            else
                printf("rm: delete directory %s failed.\n", argv[1]);
        }
    }
    return ret;
}

/**
 * @brief touch 创建文件，
 *
 * @param argc 参数个数
 * @param argv 'touch' '文件名'
 * @return int32_t 件描述符
 */
int32_t buildin_touch(uint32_t argc, char **argv)
{
    int32_t ret = -1;
    if (argc != 2)
        printf("touch: touch receives only argument! \n");
    else
    {
        make_clear_abs_path(argv[1], final_path);
        // 创建的不是根目录
        if (strcmp("/", final_path))
        {
            int32_t fd = open(final_path, O_CREAT | O_RDWR);
            if (fd != -1)
            {
                ret = 0;
            }
            else
            {
                printf("touch: create file %s failed!\n", argv[1]);
            }
        }
    }
    return ret;
}

// ls命令的内建函数
void buildin_ls(uint32_t argc, char **argv)
{
    char *pathname = NULL; // 目录的路径
    struct stat file_stat; // 记录文件的星系
    memset(&file_stat, 0, sizeof(struct stat));

    bool long_info = false; // 标记是是否有参数 -l
    uint32_t arg_path_nr = 0;
    uint32_t arg_idx = 1; // 跨过argv[0],argv[0]是字符串“ls”

    while (arg_idx < argc)
    {
        // 处理命令行参数
        if (argv[arg_idx][0] == '-')
        {
            // -l参数
            if (!strcmp("-l", argv[arg_idx]))
                long_info = true;
            // -h参数
            else if (!strcmp("-h", argv[arg_idx]))
                printf(
                    "ls: list all file in current working directory (cwd). Wish builtin command\n"
                    "Usage:\n"
                    "    -l     list all infomation about the file.\n"
                    "    -h     show this help message.\n");
            else
                printf("ls: invalid option %s. Run `ls -h` for more infomation.\n");
        }
        else
        { // ls的路径参数
            if (arg_path_nr == 0)
            {
                pathname = argv[arg_idx];
                arg_path_nr = 1;
            }
            else
            {
                printf("ls: only support one path\n");
                return;
            }
        }
        arg_idx++;
    }

    // 得到当前的工作目录路径
    if (pathname == NULL)
    { // 若只输入了ls 或 ls -l,没有输入操作路径,默认以当前路径的绝对路径为参数.
        if (getcwd(final_path, MAX_PATH_LEN))
            pathname = final_path;
        else
        {
            printf("ls getcwd for default path failed\n");
            return;
        }
    }
    else
    {
        make_clear_abs_path(pathname, final_path);
        pathname = final_path;
    }

    if (stat(pathname, &file_stat) == -1)
    {
        printf("ls: cannot access %s: No such file or directory!\n", pathname);
        return;
    }

    // ls目录，则循环遍历目录下所有的文件
    if (file_stat.st_filetype == FT_DIRECTORY)
    {
        dir_t *dir = opendir(pathname);
        dir_entry_t *de = NULL;
        char sub_pathname[MAX_PATH_LEN] = {0};
        uint32_t pathname_len = strlen(pathname);
        uint32_t last_char_idx = pathname_len - 1;
        memcpy(sub_pathname, pathname, pathname_len);

        if (sub_pathname[last_char_idx] != '/')
        {
            sub_pathname[pathname_len] = '/';
            pathname_len++;
        }

        // 回归目录指针
        rewinddir(dir);

        if (long_info)
        {
            char ftype;
            printf("total:%d\n", file_stat.st_size);
            while ((de = readdir(dir))) // 读一个目录项
            {
                ftype = 'd';
                if (de->f_type == FT_REGULAR)
                    ftype = '-';

                sub_pathname[pathname_len] = 0;
                strcat(sub_pathname, de->filename);
                memset(&file_stat, 0, sizeof(struct stat));

                if (stat(sub_pathname, &file_stat) == -1)
                {
                    printf("ls: cannot access to %s: No such file or directory!\n", de->filename);
                    return;
                }
                printf("%c  %d  %d  %s\n", ftype, de->i_no, file_stat.st_size, de->filename);
            }
        }
        else
        {
            while ((de = readdir(dir)))
                printf("%s  ", de->filename);
            printf("\n");
        }
        closedir(dir);
    }
    // ls文件
    else
    {
        printf("-  %d  %d  %s\n", file_stat.st_ino, file_stat.st_size, pathname);
    }
}

void buildin_echo(uint32_t argc, char **argv)
{
    // echo
    if (argc <= 1)
    {
        printf("echo: echo needs at least 1 argument!\n");
        printf(
            "Usage:\n"
            "    echo something\n"
            "    echo something > file\n");
        return;
    }

    // echo somting || echo someting > file
    int file_idx = -1;
    for (int i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], ">"))
        {
            file_idx = i + 1; // 文件名下标
            break;
        }
    }

    // redirect stdout to file
    int32_t fd = -1;
    if (file_idx != -1)
    {
        // get file abs path
        char abs_path[512] = {0};

        if (argv[file_idx][0] != '/')
        {
            getcwd(abs_path, 512);
            uint32_t len = strlen(abs_path);
            if (abs_path[len - 1] != '/')
                abs_path[len] = '/';
            strcat(abs_path, argv[file_idx]);
        }
        else
            strcpy(abs_path, argv[file_idx]);

        // open file
        fd = open(abs_path, O_RDWR);
        if (fd == -1)
        {
            printf("echo: open file %s failed\n", abs_path);
            return;
        }
    }
    else
        fd = 1;

    uint32_t args_to_print = file_idx == -1 ? argc : file_idx - 1;
    for (uint32_t i = 1; i < args_to_print; i++)
    {
        write(fd, argv[i], strlen(argv[i]));
        write(fd, " ", 1);
    }
    write(fd, "\n", 1);

    if (file_idx != 1)
    {
        close(fd);
    }
    return;
}
