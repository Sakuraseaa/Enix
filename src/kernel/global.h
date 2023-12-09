#ifndef __KERNEL_GLOBAL_H
#define __KERNEL_GLOBAL_H
#include "stdint.h"

// ----------------  GDT描述符属性  ----------------

#define DESC_G_4K 1  // G位(指定段界限单位的大小)_高32位中的第23位_置 1 代表颗粒度为4K -0 代表 1字节
#define DESC_D_32 1  // D/B位_高32位中的第22位_置 1 表示指令, 操作数都是32位,使用eip和esp寄存器
#define DESC_L 0     // 64位代码标记，此处标记为0便可。
#define DESC_AVL 0   // cpu不用此位，暂置为0
#define DESC_P 1     // 高32位中的第15位_Present_置为1代表 段在内存 中
#define DESC_DPL_0 0 // 高32位中的第13~14位, 描述4种特权级,
#define DESC_DPL_1 1 // 00 01 10 11
#define DESC_DPL_2 2
#define DESC_DPL_3 3
/*
   代码段和数据段属于存储段，tss和各种门描述符属于系统段
   s为1时表示存储段,为0时表示系统段.
*/
#define DESC_S_CODE 1
#define DESC_S_DATA DESC_S_CODE
#define DESC_S_SYS 0
#define DESC_TYPE_CODE 8 // x=1,c=0,r=0,a=0 代码段是可执行的,非依从的,不可读的,已访问位a清0.  1000
#define DESC_TYPE_DATA 2 // x=0,e=0,w=1,a=0 数据段是不可执行的,向上扩展的,可写的,已访问位a清0. 0011
#define DESC_TYPE_TSS 9  // B位为0,不忙 1001

#define RPL0 0
#define RPL1 1
#define RPL2 2
#define RPL3 3

#define TI_GDT 0
#define TI_LDT 1

#define SELECTOR_K_CODE ((1 << 3) + (TI_GDT << 2) + RPL0) // 0000-1000第一个段描述符的选择字
#define SELECTOR_K_DATA ((2 << 3) + (TI_GDT << 2) + RPL0) // 0001-0000第二个段描述符的选择字
#define SELECTOR_K_STACK SELECTOR_K_DATA                  // 内核栈段和内核数据段共用一个选择子
#define SELECTOR_K_GS ((3 << 3) + (TI_GDT << 2) + RPL0)   // 0001-1000第三个段描述符的选择字
/* 第3个段描述符是显存,第4个是tss */
#define SELECTOR_U_CODE ((5 << 3) + (TI_GDT << 2) + RPL3) // 0010-1011第五个段描述符的选择字
#define SELECTOR_U_DATA ((6 << 3) + (TI_GDT << 2) + RPL3) // 0011-0011第六个段描述符的选择字
#define SELECTOR_U_STACK SELECTOR_U_DATA

// 1100_0000     对应高32位 20 ~ 23位
#define GDT_ATTR_HIGH ((DESC_G_4K << 7) + (DESC_D_32 << 6) + (DESC_L << 5) + (DESC_AVL << 4))

// 1111_1000    对应高32位 8 ~ 15位
#define GDT_CODE_ATTR_LOW_DPL3 ((DESC_P << 7) + (DESC_DPL_3 << 5) + (DESC_S_CODE << 4) + DESC_TYPE_CODE)
// 1111_0011
#define GDT_DATA_ATTR_LOW_DPL3 ((DESC_P << 7) + (DESC_DPL_3 << 5) + (DESC_S_DATA << 4) + DESC_TYPE_DATA)

//---------------  TSS描述符属性  ------------
#define TSS_DESC_D 0 // 表示该内存段中的指令的操作数和有效地址是16位的

// 1000-0000
#define TSS_ATTR_HIGH ((DESC_G_4K << 7) + (TSS_DESC_D << 6) + (DESC_L << 5) + (DESC_AVL << 4) + 0x0)

// 1000-1001
#define TSS_ATTR_LOW ((DESC_P << 7) + (DESC_DPL_0 << 5) + (DESC_S_SYS << 4) + DESC_TYPE_TSS)
// 选择子 是16位的， 0010-0000
#define SELECTOR_TSS ((4 << 3) + (TI_GDT << 2) + RPL0)

/*定义GDT中描述符的结构*/
struct gdt_desc
{
   uint16_t limit_low_word;
   uint16_t base_low_word;
   uint8_t base_mid_byte;
   uint8_t attr_low_byte;
   uint8_t limit_high_attr_high;
   uint8_t base_high_byte;
};

//--------------   IDT描述符属性  ------------
#define IDT_DESC_P 1
#define IDT_DESC_DPL0 0
#define IDT_DESC_DPL3 3
#define IDT_DESC_32_TYPE 0xE // 32位的门
#define IDT_DESC_16_TYPE 0x6 // 16位的门，不用，定义它只为和32位门区分
#define IDT_DESC_ATTR_DPL0 ((IDT_DESC_P << 7) + (IDT_DESC_DPL0 << 5) + IDT_DESC_32_TYPE)
#define IDT_DESC_ATTR_DPL3 ((IDT_DESC_P << 7) + (IDT_DESC_DPL3 << 5) + IDT_DESC_32_TYPE)

//---------------    eflags属性    ----------------

/********************************************************
--------------------------------------------------------------
        Intel 8086 Eflags Register
--------------------------------------------------------------
*
*     15|14|13|12|11|10|F|E|D C|B|A|9|8|7|6|5|4|3|2|1|0|
*      |  |  |  |  |  | | |  |  | | | | | | | | | | | '---  CF……Carry Flag
*      |  |  |  |  |  | | |  |  | | | | | | | | | | '---  1 MBS
*      |  |  |  |  |  | | |  |  | | | | | | | | | '---  PF……Parity Flag
*      |  |  |  |  |  | | |  |  | | | | | | | | '---  0
*      |  |  |  |  |  | | |  |  | | | | | | | '---  AF……Auxiliary Flag
*      |  |  |  |  |  | | |  |  | | | | | | '---  0
*      |  |  |  |  |  | | |  |  | | | | | '---  ZF……Zero Flag
*      |  |  |  |  |  | | |  |  | | | | '---  SF……Sign Flag
*      |  |  |  |  |  | | |  |  | | | '---  TF……Trap Flag
*      |  |  |  |  |  | | |  |  | | '---  IF……Interrupt Flag
*      |  |  |  |  |  | | |  |  | '---  DF……Direction Flag
*      |  |  |  |  |  | | |  |  '---  OF……Overflow flag
*      |  |  |  |  |  | | |  '----  IOPL……I/O Privilege Level
*      |  |  |  |  |  | | '-----  NT……Nested Task Flag
*      |  |  |  |  |  | '-----  0
*      |  |  |  |  |  '-----  RF……Resume Flag
*      |  |  |  |  '------  VM……Virtual Mode Flag
*      |  |  |  '-----  AC……Alignment Check
*      |  |  '-----  VIF……Virtual Interrupt Flag
*      |  '-----  VIP……Virtual Interrupt Pending
*      '-----  ID……ID Flag
*
*
**********************************************************/
#define EFLAGS_MBS (1 << 1)     // 此项必须要设置
#define EFLAGS_IF_1 (1 << 9)    // if为1,开中断,中断标志位，从第0位开始数，第9位
#define EFLAGS_IF_0 0           // if为0,关中断
#define EFLAGS_IOPL_3 (3 << 12) // IOPL3,用于测试用户程序在非系统调用下进行IO
#define EFLAGS_IOPL_0 (0 << 12) // IOPL0, input output privilegs level 特权级标志位

#define NULL ((void *)0)
#define bool int
#define true 1
#define false 0

#define PG_SIZE 4096

#define DIV_ROUND_UP(X, STEP) ((X + STEP - 1) / (STEP))

#endif