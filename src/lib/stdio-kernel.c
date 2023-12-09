#include "stdio-kernel.h"
#include "stdio.h"
#include "../device/console.h"

void printk(const char *format, ...)
{
    va_list args;
    // args = (char *)&format; == va_start(args, format)
    va_start(args, format);

    char buf[1024] = {0};
    vsprintf(buf, format, args);
    va_end(args);
    console_put_str(buf);
}