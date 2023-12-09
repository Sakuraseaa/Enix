#include "stdio.h"
#include "interrupt.h"
#include "global.h"
#include "string.h"
#include "syscall.h"
#include "print.h"
#include "file.h"

/* 将整型转换成字符(integer to ascii) */
static void itoa(uint32_t value, char **buf_ptr_addr, uint8_t base)
{
   uint32_t m = value % base; // 求模,最先掉下来的是最低位
   uint32_t i = value / base; // 取整
   if (i)
   { // 如果倍数不为0则递归调用。
      itoa(i, buf_ptr_addr, base);
   }
   if (m < 10)
   {                                  // 如果余数是0~9
      *((*buf_ptr_addr)++) = m + '0'; // 将数字0~9转换为字符'0'~'9'
   }
   else
   {                                       // 否则余数是A~F
      *((*buf_ptr_addr)++) = m - 10 + 'A'; // 将数字A~F转换为字符'A'~'F'
   }
}

/* 将参数ap按照格式format输出到字符串str,并返回替换后str长度 */
uint32_t vsprintf(char *str, const char *format, va_list ap)
{
   char *buf_ptr = str;
   const char *index_ptr = format;
   char index_char = *index_ptr;

   int32_t arg_int;
   char *arg_str;
   while (index_char)
   {
      if (index_char != '%')
      {
         *(buf_ptr++) = index_char;
         index_char = *(++index_ptr);
         continue;
      }
      index_char = *(++index_ptr); // 得到%后面的字符
      switch (index_char)
      {
      // 输出个字符
      case 'c':
         *buf_ptr++ = va_arg(ap, char);
         index_char = *(++index_ptr);
         break;
      // 输出整数
      case 'd':
         arg_int = va_arg(ap, int);
         if (arg_int < 0)
         {
            arg_int = 0 - arg_int;
            *buf_ptr++ = '-';
         }
         itoa(arg_int, &buf_ptr, 10);
         index_char = *(++index_ptr);
         break;
      // 输出字符串
      case 's':
         arg_str = va_arg(ap, char *);
         strcpy(buf_ptr, arg_str);
         buf_ptr += strlen(arg_str);
         index_char = *(++index_ptr);
         break;
      // 输出16进制
      case 'x':
         arg_int = va_arg(ap, int);
         itoa(arg_int, &buf_ptr, 16);
         index_char = *(++index_ptr); // 跳过格式字符并更新index_char
         break;
      }
   }
   return strlen(str);
}

/* 格式化输出字符串format */
uint32_t printf(const char *format, ...)
{
   va_list args;
   va_start(args, format); // 使args指向format
   char buf[1024] = {0};   // 用于存储拼接后的字符串
   vsprintf(buf, format, args);
   va_end(args);

   return write(1, buf, strlen(buf));
}

/* 格式化输出字符串format */
uint32_t sprintf(char *buf, const char *format, ...)
{
   va_list args;
   uint32_t retval;
   va_start(args, format); // 使args指向format
   retval = vsprintf(buf, format, args);
   va_end(args);
   return retval;
}

char cmd_line[64] = {0};
static void readline(char *buf, int32_t count)
{
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
         {         // 阻止删除非本次输入的信息
            --pos; // 退回到缓冲区cmd_line中上一个字符
            putchar('\b');
         }
         break;
      default:
         putchar(*pos);
         pos++;
      }
   }
}

int32_t get_char(char *buf)
{
   readline(cmd_line, 1);
   buf[0] = cmd_line[0];
   return 1;
}