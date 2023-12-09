#include "timer.h"
#include "io.h"
#include "print.h"
#include "interrupt.h"
#include "thread.h"
#include "debug.h"
#include "string.h"
#include "stdio-kernel.h"

#define IRQ0_FREQUENCY 100 // 1秒100个时钟中断
#define INPUT_FREQUENCY 1193180
#define COUNTER0_VALUE INPUT_FREQUENCY / IRQ0_FREQUENCY
#define CONTRER0_PORT 0x40
#define COUNTER0_NO 0
#define COUNTER_MODE 2
#define READ_WRITE_LATCH 3
#define PIT_CONTROL_PORT 0x43

#define mil_seconds_per_intr (1000 / IRQ0_FREQUENCY) // 1个时钟中断10毫秒, 1000/100

uint32_t volatile ticks = 0; // ticks是内核自中断开启以来总共的嘀嗒数
tm_t time;

#define READ_COMS(addr) ({ \
   outb(0x70, addr);       \
   inb(0x71);              \
})

#define BCD_TO_BIN(val) ((val = (val & 0xf) + (val >> 4) * 10))

static void get_cur_time(void)
{
   do
   {
      time.sec = READ_COMS(0);
      time.min = READ_COMS(2);
      time.hour = READ_COMS(4);
      time.day = READ_COMS(7);
      time.month = READ_COMS(8);
      time.year = READ_COMS(9);
   } while (time.sec != READ_COMS(0));

   BCD_TO_BIN(time.sec);
   BCD_TO_BIN(time.min);
   BCD_TO_BIN(time.hour);
   BCD_TO_BIN(time.day);
   BCD_TO_BIN(time.month);
   BCD_TO_BIN(time.year);
}

// 得到系统当前的时间输出，只能计算本月内的时间，如果今天31号，但系统已经运行了48小时
// 那么得到的系统时间就是 33号 hhhhhhhhhhhhhhh, 这边只简单实现一下
// 这里个函数不对， ticks太多了，不知道为什么
void sys_date(void)
{
   uint32_t sec_already = ticks / 100;
   uint32_t add_hour = sec_already / HOUR;
   uint32_t add_min = (sec_already - (add_hour * HOUR)) / MINUTE;
   uint32_t add_sec = sec_already - (add_hour * HOUR) - (add_min * MINUTE);

   time.sec += add_hour;
   time.min += (add_min);
   time.hour += (add_hour);

   printk("Current_Time: %d/%d/%d %d:%d:%d\n", time.year, time.month, time.day, time.hour, time.min, time.sec);
}

/* 以tick为单位的sleep,任何时间形式的sleep会转换此ticks形式 */
static void ticks_to_sleep(uint32_t sleep_ticks)
{
   uint32_t Old_ticks = ticks;

   /* 若间隔的ticks数不够便让出cpu */
   while (ticks - Old_ticks < sleep_ticks)
      thread_yield();
}

/* 把操作的计数器counter_no、读写锁属性rwl、计数器模式counter_mode写入模式控制寄存器并赋予初始值counter_value */
static void frequency_set(uint8_t counter_port,
                          uint8_t counter_no,
                          uint8_t rwl,
                          uint8_t counter_mode,
                          uint16_t counter_value)
{
   // int b = (counter_no << 6 | rwl << 4 | counter_mode << 1);
   /* 往控制字寄存器端口0x43中写入控制字 */
   outb(PIT_CONTROL_PORT, (uint8_t)(counter_no << 6 | rwl << 4 | counter_mode << 1));
   // outb(PIT_CONTROL_PORT, 0x36);
   /* 先写入counter_value的低8位 */
   outb(counter_port, (uint8_t)counter_value);
   /* 再写入counter_value的高8位 */
   outb(counter_port, (uint8_t)(counter_value >> 8));
}

/* 时钟的中断处理函数 */
static void intr_timer_handler(int vectorNum)
{
   struct task_struct *cur_thread = running_thread();

   ASSERT(cur_thread->stack_magic == 0x19870916); // 检查栈是否溢出

   cur_thread->elapsed_ticks++; // 记录此线程占用的cpu时间嘀
   ticks++;                     // 从内核第一次处理时间中断后开始至今的滴哒数,内核态和用户态总共的嘀哒数

   if (cur_thread->ticks == 0)
   { // 若进程时间片用完就开始调度新的进程上cpu
      schedule();
   }
   else
   { // 将当前进程的时间片-1
      cur_thread->ticks--;
   }
}

/* 初始化PIT8253 */
void timer_init()
{
   put_str("timer_init start\n");
   /* 设置8253的定时周期,也就是发中断的周期 */
   frequency_set(CONTRER0_PORT, COUNTER0_NO, READ_WRITE_LATCH, COUNTER_MODE, COUNTER0_VALUE);
   register_handler(0x20, intr_timer_handler);
   get_cur_time();
   put_str("timer_init done\n");
}

/* 以毫秒为单位的sleep   1秒= 1000毫秒 */
void mtime_sleep(uint32_t m_seconds)
{
   uint32_t sleep_ticks = DIV_ROUND_UP(m_seconds, mil_seconds_per_intr);
   ASSERT(sleep_ticks > 0);
   ticks_to_sleep(sleep_ticks);
}