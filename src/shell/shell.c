#include "shell.h"
#include "stdint.h"
#include "fs.h"
#include "file.h"
#include "syscall.h"
#include "stdio.h"
#include "global.h"
#include "debug.h"
#include "string.h"
#include "buildin_cmd.h"
#include "assert.h"

#define cmd_len 128   // 最大支持键入128个字符的命令行输入
#define MAX_ARG_NR 16 // 加上命令名外,最多支持15个参数

// 存储输入的命令
static char cmd_line[cmd_len] = {0};

// 用于洗路径时的缓冲
char final_path[MAX_PATH_LEN] = {0};
// 用来记录当前目录，是当前目录的缓存，每次执行cd命令是会更新此内容
char cwd_cache[64] = {0};

// 输出提示符
void print_prompt(void)
{
    printf("[sk@Elephant %s]$ ", cwd_cache);
}

// 从键盘缓冲区读入count个字节到buf, 读入一行命令
static void readline(char *buf, int32_t count)
{
    ASSERT(buf != NULL && count > 0);
    char *pos = buf;
    while (read(stdin_no, pos, 1) != -1 && (pos - buf) < count)
    {
        switch (*pos)
        {
        // 找到回车换行符后认为键入的命令结束，直接返回
        case '\n':
        case '\r':
            *pos = 0;
            putchar('\n');
            return;
        // 删除键
        case '\b':
            if (cmd_line[0] != '\b')
            {          // 阻止删除非本次输入的信息
                --pos; // 退回到缓冲区cmd_line中上一个字符
                putchar('\b');
            }
            break;
        // ctrl + l 清屏
        case 'l' - 'a':
            // 1.将当前的字符'l' - 'a'置为0
            *pos = 0;
            // 2. 屏幕清空
            clear();
            // 3 打印提示符
            print_prompt();
            // 4. 将之前键入的内容再次打印
            printf("%s", buf);
            break;
        // ctrl + u 清空输入123
        case 'u' - 'a':
            while (buf != pos)
            {
                putchar('\b');
                *(pos--) = 0;
            }
            break;
        /* 非控制键则输出字符 */
        default:
            putchar(*pos);
            pos++;
        }
    }
    printf("readline: can`t find enter_key in the cmd_line, max num of char is 128\n");
}

// 分析字符串cmd_str中以token为分隔符的单词，存入argv数组
static int32_t cmd_parse(char *cmd_str, char **argv, char token)
{
    ASSERT(cmd_str != NULL);
    int32_t arg_idx = 0;
    // 初始化数组
    while (arg_idx < MAX_ARG_NR)
    {
        argv[arg_idx] = NULL;
        arg_idx++;
    }
    char *next = cmd_str;
    int argc = 0;

    // 外层循环处理整个命令行
    while (*next)
    {
        // 去除命令字或参数之前的空格
        while (*next == token)
        {
            next++;
        }

        if (*next == 0)
            break;

        argv[argc] = next;
        // 内存循环处理命令行中的每一个命令字及参数
        while (*next && *next != token)
            next++;

        // 如果遇见的是token字符，使得变为0，划分为单词
        if (*next)
            *next++ = 0;
        if (argc >= MAX_ARG_NR)
            return -1;
        argc++;
    }
    return argc;
}

static void help()
{
    printf("\
 buildin commands:\n\
       ls: show directory or file information\n\
       cd: change current work directory\n\
       mkdir: create a directory\n\
       rmdir: remove a empty directory\n\
       rm: remove a regular file\n\
       pwd: show current work directory\n\
       ps: show process information\n\
       clear: clear screen\n\
       touch: create a file\n\
       echo: display a line of text\n\
       date: display current time\n\
 shortcut key:\n\
       ctrl+l: clear screen\n\
       ctrl+u: clear input\n\n");
}

static void cmd_execute(uint32_t argc, char **argv)
{
    if (!strcmp("ls", argv[0]))
        buildin_ls(argc, argv);
    else if (!strcmp("cd", argv[0]))
    {
        if (buildin_cd(argc, argv))
        {
            memset(cwd_cache, 0, MAX_PATH_LEN);
            strcpy(cwd_cache, final_path);
        }
    }
    else if (!strcmp("pwd", argv[0]))
        buildin_pwd(argc, argv);
    else if (!strcmp("ps", argv[0]))
        buildin_ps(argc, argv);
    else if (!strcmp("clear", argv[0]))
        buildin_clear(argc, argv);
    else if (!strcmp("mkdir", argv[0]))
        buildin_mkdir(argc, argv);
    else if (!strcmp("rmdir", argv[0]))
        buildin_rmdir(argc, argv);
    else if (!strcmp("rm", argv[0]))
        buildin_rm(argc, argv);
    else if (!strcmp("touch", argv[0]))
        buildin_touch(argc, argv);
    else if (!strcmp("help", argv[0]))
        help();
    else if (!strcmp("echo", argv[0]))
        buildin_echo(argc, argv);
    else if (!strcmp("date", argv[0]))
        date();
    else
    { // 如果是外部命令,需要从磁盘上加载
        int32_t pid = fork();
        if (pid)
        { // 父进程
            int32_t status;
            int32_t child_pid = wait(&status);
            if (child_pid == -1)
            {
                panic("my_shell: no child\n");
            }
            // printf("\n my pid: %d child_pid: %d, it is status: %d\n", getpid(), child_pid, status);
        }
        else
        { // 子进程, final_path被操作后组成的是用户当前所在目录路径 + argv[0]
            char *temp = argv[0];
            make_clear_abs_path(argv[0], final_path);
            argv[0] = final_path;
            /* 先判断下文件是否存在 */
            struct stat file_stat;
            memset(&file_stat, 0, sizeof(struct stat));
            if (stat(argv[0], &file_stat) == -1)
            { // final_path被操作后，是 默认路径（/） + argv[0]
                make_default_path(temp, final_path);
                argv[0] = final_path;
                memset(&file_stat, 0, sizeof(struct stat));
                // 默认路径和当前路径都没有找到，那么就报错
                if (stat(argv[0], &file_stat) == -1)
                    printf("my_shell: cannot access %s: No such file or directory\n", argv[0]);
                else
                    goto EXE;
            }
            else
            {
            EXE:
                execv(argv[0], argv);
            }
            exit(0);
        }
    }
}

int32_t argc;                    // 参数个数
char *argv[MAX_ARG_NR] = {NULL}; // 存储参数
// 简单shell
void my_shell(void)
{
    cwd_cache[0] = '/';
    while (1)
    {
        print_prompt();
        memset(final_path, 0, MAX_PATH_LEN);
        memset(cmd_line, 0, cmd_len);
        readline(cmd_line, cmd_len);
        if (cmd_line[0] == 0)
        {
            continue;
            // 若只键入了一个回车
        }
        else
        {
            argc = -1;
            argc = cmd_parse(cmd_line, argv, ' ');
            if (argc == -1)
            {
                printf("num of argument exceed %d\n", MAX_ARG_NR);
                continue;
            }
            cmd_execute(argc, argv);
        }
    }

    PANIC("my_shell: should not be here");
}
void wish()
{
    char *pipe_symbol = strchr(cmd_line, '|');
    if (pipe_symbol)
    {
        /* 支持多重管道操作,如cmd1|cmd2|..|cmdn,
         * cmd1的标准输出和cmdn的标准输入需要单独处理 */
        // 1. 生成管道
        int32_t fd[2] = {-1}; // fd[0]用于输入,fd[1]用于输出
        pipe(fd);
        // 将标志输出重定向到fd[1], 使后面的输出星系定向到内核缓冲区
        fd_redirect(1, fd[1]);

        // 2.执行第一个命令
        char *each_cmd = cmd_line;
        pipe_symbol = strchr(each_cmd, '|');
        *pipe_symbol = 0;

        argc = -1;
        argc = cmd_parse(each_cmd, argv, ' ');
        cmd_execute(argc, argv);
        /* 跨过'|',处理下一个命令 */
        each_cmd = pipe_symbol + 1;

        /* 将标准输入重定向到fd[0],使之指向内核环形缓冲区*/
        fd_redirect(0, fd[0]);
        // 3. 中间命令的输入喝输出都指向环形缓冲区
        while ((pipe_symbol = strchr(each_cmd, '|')))
        {
            *pipe_symbol = 0;
            argc = -1;
            argc = cmd_parse(each_cmd, argv, ' ');
            cmd_execute(argc, argv);
            each_cmd = pipe_symbol + 1;
        }

        // 4.  处理管道中最后一个命令, 把标志输出恢复到屏幕
        fd_redirect(1, 1);

        argc = -1;
        argc = cmd_parse(each_cmd, argv, ' ');
        cmd_execute(argc, argv);
        // 5. 把标准输入恢复为键盘
        fd_redirect(0, 0);

        // 6 关闭管道
        close(fd[0]);
        close(fd[1]);
    }
}