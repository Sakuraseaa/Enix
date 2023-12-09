#ifndef __DEVICE_TIME_H
#define __DEVICE_TIME_H
#include "stdint.h"

typedef struct timer
{
    int8_t sec;   // 秒
    int8_t min;   // 分
    int8_t hour;  // 小时
    int8_t day;   // 天
    int8_t month; // 月
    int8_t year;  // 年
} tm_t;

#define MINUTE 60
#define HOUR (60 * MINUTE)

void timer_init(void);
void mtime_sleep(uint32_t m_seconds);
void sys_date();
#endif
