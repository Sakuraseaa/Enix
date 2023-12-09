 %include "boot.inc"
   section loader vstart=LOADER_BASE_ADDR
   LOADER_STACK_TOP equ LOADER_BASE_ADDR
   
;构建gdt及其内部的描述符
   GDT_BASE:   dd    0x00000000 
	            dd    0x00000000

   CODE_DESC:  dd    0x0000FFFF 
	            dd    DESC_CODE_HIGH4

   DATA_STACK_DESC:  dd    0x0000FFFF
		               dd    DESC_DATA_HIGH4

   VIDEO_DESC: dd    0x80000007	       ;limit=(0xbffff-0xb8000)/4k=0x7
	            dd    DESC_VIDEO_HIGH4   ;此时dpl为0

   GDT_SIZE   equ   $ - GDT_BASE
   GDT_LIMIT   equ   GDT_SIZE -	1 
   times 60 dq 0					 ; 此处预留60个描述符的空位(slot), dq 8字节 dd 4字节 dw 2字节 
   SELECTOR_CODE equ (0x0001<<3) + TI_GDT + RPL0    ; 相当于(CODE_DESC - GDT_BASE)/8 + TI_GDT + RPL0
   SELECTOR_DATA equ (0x0002<<3) + TI_GDT + RPL0	 ; 同上
   SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0	 ; 同上 

   ; total_mem_bytes用于保存内存容量,以字节为单位,此位置比较好记。
   ; 当前偏移loader.bin文件头0x200字节,loader.bin的加载地址是0x900, total_mem_bytes的位置就是0xb00
   ; 故total_mem_bytes内存中的地址是0xb00.将来在内核中咱们会引用此地址
   total_mem_bytes dd 0					 
   ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

   ;以下是定义gdt的指针，前2字节是gdt界限，后4字节是gdt起始地址
   gdt_ptr  dw  GDT_LIMIT 
	    dd  GDT_BASE

   ;人工对齐:total_mem_bytes4字节+gdt_ptr6字节+ards_buf244字节+ards_nr2,共256字节
   ards_buf times 244 db 0
   ards_nr dw 0		      ;用于记录ards结构体数量

;下面是代码，此处偏移文件头是 0x300字节            
loader_start:
   
;-------  int 15h eax = 0000E820h ,edx = 534D4150h ('SMAP') 获取内存布局  -------

   xor ebx, ebx		      ;第一次调用时，ebx值要为0
   mov edx, 0x534d4150	      ;edx只赋值一次，循环体中不会改变
   mov di, ards_buf	      ;ards结构缓冲区
.e820_mem_get_loop:	      ;循环获取每个ARDS内存范围描述结构
   mov eax, 0x0000e820	      ;执行int 0x15后,eax值变为0x534d4150,所以每次执行int前都要更新为子功能号。
   mov ecx, 20		      ;ARDS地址范围描述符结构大小是20字节
   int 0x15
   jc .e820_failed_so_try_e801   ;若cf位为1则有错误发生，尝试0xe801子功能
   add di, cx		      ;使di增加20字节指向缓冲区中新的ARDS结构位置
   inc word [ards_nr]	      ;记录ARDS数量
   cmp ebx, 0		      ;若ebx为0且cf不为1,这说明ards全部返回，当前已是最后一个
   jnz .e820_mem_get_loop

;在所有ards结构中，找出(base_add_low + length_low)的最大值，即内存的容量。
   mov cx, [ards_nr]	      ;遍历每一个ARDS结构体,循环次数是ARDS的数量
   mov ebx, ards_buf 
   xor edx, edx		      ;edx为最大的内存容量,在此先清0
.find_max_mem_area:	      ;无须判断type是否为1,最大的内存块一定是可被使用
   mov eax, [ebx]	      ;base_add_low
   add eax, [ebx+8]	      ;length_low
   add ebx, 20		      ;指向缓冲区中下一个ARDS结构
   cmp edx, eax		      ;冒泡排序，找出最大,edx寄存器始终是最大的内存容量
   jge .next_ards
   mov edx, eax		      ;edx为总内存大小
.next_ards:
   loop .find_max_mem_area
   jmp .mem_get_ok

;------  int 15h ax = E801h 获取内存大小,最大支持4G  ------
; 返回后, ax cx 值一样,以KB为单位,bx dx值一样,以64KB为单位
; 在ax和cx寄存器中为低16M,在bx和dx寄存器中为16MB到4G。
.e820_failed_so_try_e801:
   mov ax,0xe801
   int 0x15
   jc .e801_failed_so_try88   ;若当前e801方法失败,就尝试0x88方法

;1 先算出低15M的内存,ax和cx中是以KB为单位的内存数量,将其转换为以byte为单位
   mov cx,0x400	     ;cx和ax值一样,cx用做乘数
   mul cx 
   shl edx,16
   and eax,0x0000FFFF
   or edx,eax
   add edx, 0x100000 ;ax只是15MB,故要加1MB
   mov esi,edx	     ;先把低15MB的内存容量存入esi寄存器备份

;2 再将16MB以上的内存转换为byte为单位,寄存器bx和dx中是以64KB为单位的内存数量
   xor eax,eax
   mov ax,bx		
   mov ecx, 0x10000	;0x10000十进制为64KB
   mul ecx		;32位乘法,默认的被乘数是eax,积为64位,高32位存入edx,低32位存入eax.
   add esi,eax		;由于此方法只能测出4G以内的内存,故32位eax足够了,edx肯定为0,只加eax便可
   mov edx,esi		;edx为总内存大小
   jmp .mem_get_ok

;-----------------  int 15h ah = 0x88 获取内存大小,只能获取64M之内  ----------
.e801_failed_so_try88: 
   ;int 15后，ax存入的是以kb为单位的内存容量
   mov  ah, 0x88
   int  0x15
   jc .error_hlt
   and eax,0x0000FFFF
      
   ;16位乘法，被乘数是ax,积为32位.积的高16位在dx中，积的低16位在ax中
   mov cx, 0x400     ;0x400等于1024,将ax中的内存容量换为以byte为单位
   mul cx
   shl edx, 16	     ;把dx移到高16位
   or edx, eax	     ;把积的低16位组合到edx,为32位的积
   add edx,0x100000  ;0x88子功能只会返回1MB以上的内存,故实际内存大小要加上1MB

.mem_get_ok:
   mov [total_mem_bytes], edx	 ;将内存换为byte单位后存入total_mem_bytes处。


;-----------------   准备进入保护模式   -------------------
;1 打开A20
;2 加载gdt
;3 将cr0的pe位置1

   ;-----------------  打开A20  ----------------
   in al,0x92
   or al,0000_0010B
   out 0x92,al

   ;-----------------  加载GDT  ----------------
   lgdt [gdt_ptr]

   ;-----------------  cr0第0位置1  ----------------
   mov eax, cr0
   or eax, 0x00000001
   mov cr0, eax

   jmp dword SELECTOR_CODE:p_mode_start	     ; 刷新流水线，避免分支预测的影响,这种cpu优化策略，最怕jmp跳转，
					     ; 这将导致之前做的预测失效，从而起到了刷新的作用。

.error_hlt:		      ;出错则挂起
   hlt

[bits 32]
p_mode_start:
   ; 初始化各种段选择器
   mov ax, SELECTOR_DATA
   mov ds, ax
   mov es, ax
   mov ss, ax
   mov esp,LOADER_STACK_TOP
   mov ax, SELECTOR_VIDEO
   mov gs, ax
;-------------------------------------------------------
;------------加载kernel, 把kernel代码复制到内存-----------
;-------------------------------------------------------
   mov eax, KERNEL_START_SECTOR  ;kernel.bin所在的扇区号
   mov ebx, KERNEL_BIN_BASE_ADDR ;从磁盘读取后 写到ebx指定的地址
   mov ecx, 200
   call rd_disk_m_32

;-------------------------------------------------------
;-----  创建页目录及页表并初始化页内存位图 ----------------
;----把GDT表, 栈段, 显存段移动内核的虚拟地址空间-----------
;-------------------------------------------------------
   ; 1.开启页表，第一步
   call setup_page

   ;要将描述符表地址及偏移量写入内存gdt_ptr,一会用新地址重新加载
   ;为何重新加载 
   ;    答：因为想要把栈段,现存段, GDT 移动到内核空间, 接受内核保护, 不要轻易被访问
   ;为何不在开启保护模式后, 把GDT直接加载到内核处，而是在开启保护模式之前就加载了, 现在这里又要重新加载？
   ;    答: 开启保护模式第二步需要加载GDT
   sgdt [gdt_ptr]	      ; 存储到原来gdt所有的位置


   ;将GDT中显存描述符段中的基地址+0xc000_0000
    ;为什么要加 3GB ? 因为内核的虚拟地址空间对应于 3GB ~　4GB
   mov ebx, [gdt_ptr + 2]  
   or dword [ebx + 0x18 + 4], 0xc0000000      ;视频段是第3个段描述符,每个描述符是8字节,故0x18。
	;段描述符的高4字节的最高位是段基址的31~24位

   ;将gdt的基址加上0xc0000000使其成为内核所在的高地址
   add dword [gdt_ptr + 2], 0xc0000000

   add esp, 0xc0000000        ; 将栈指针同样映射到内核地址

;--------------------------------------------------
;-----------------开启页表三部曲-----第一步在上面---------------
;--------------------------------------------------
   ; 2.把页目录地址赋给cr3
   mov eax, PAGE_DIR_TABLE_POS
   mov cr3, eax

   ; 3.打开cr0的pg位(第31位)
   mov eax, cr0
   or eax, 0x80000000
   mov cr0, eax

   ;在开启分页后,用gdt新的地址重新加载
   lgdt [gdt_ptr]             ; 重新加载

   ; mov byte [gs:160], 'S'     ;视频段段基址已经被更新,用字符v表示virtual addr
   ; mov byte [gs:162], 'k'    
   ; mov byte [gs:164], ' '     
   ; mov byte [gs:166], 'L'    
   ; mov byte [gs:168], 'o'     
   ; mov byte [gs:170], 'a'
   ; mov byte [gs:172], 'd'     
   ; mov byte [gs:174], 'e'
   ; mov byte [gs:176], 'r'            

;;;;;;;;;;;;;;;;;;;;;;;;;;;;  此时不刷新流水线也没问题  ;;;;;;;;;;;;;;;;;;;;;;;;
;由于一直处在32位下,原则上不需要强制刷新,经过实际测试没有以下这两句也没问题.
;但以防万一，还是加上啦，免得将来出来莫句奇妙的问题.
   jmp SELECTOR_CODE:enter_kernel	  ;强制刷新流水线,更新gdt
enter_kernel:    
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
   ; mov byte [gs:320], 'k'     ;在显存输出一些文字, 表示进人内核
   ; mov byte [gs:322], 'e'     
   ; mov byte [gs:324], 'r'     
   ; mov byte [gs:326], 'n'     
   ; mov byte [gs:328], 'e'     
   ; mov byte [gs:330], 'l'     

   ; mov byte [gs:480], 'w'     
   ; mov byte [gs:482], 'h'     
   ; mov byte [gs:484], 'i'    
   ; mov byte [gs:486], 'l'    
   ; mov byte [gs:488], 'e'     
   ; mov byte [gs:490], '('    
   ; mov byte [gs:492], '1'    
   ; mov byte [gs:494], ')'     
   ; mov byte [gs:496], ';'     
   
   call kernel_init
   
   mov esp, 0xc009_f000
   
   jmp KERNEL_ENTRY_POINT     ;用地址0x1500访问测试，结果ok

;---------------kernel_init-----------------------------
;------ 功能：把内核文件复制到相对于的虚拟内存 -------------
;-------------------------------------------------------
kernel_init:
   xor eax,eax
   xor ebx, ebx   ;ebx记录程序头表地址
   xor ecx, ecx   ;cx记录程序头表中program head数量
   xor edx, edx   ;edx记录program header尺寸, 就是e_phentsize

   mov ebx, [KERNEL_BIN_BASE_ADDR + 28] ;偏移文件开始部分28字节的地方是e_phoff,表示第1 个program header在文件中的偏移量
	add ebx, KERNEL_BIN_BASE_ADDR        ;其实该值是0x34,不过还是谨慎一点，这里来读取实际值
   
   mov cx, [KERNEL_BIN_BASE_ADDR + 44] ;e_phnum, 程序头表中条目的多少 
   
   mov dx, [KERNEL_BIN_BASE_ADDR + 42] ;e_phentsizes属性, 程序头表中一个条目的大小
   
.each_segment:
   cmp byte [ebx + 0], PT_NULL 
   je .PTNULL
   
   ;为函数memcpy压入参数,参数是从右往左依然压入.函数原型类似于 memcpy(dst,src,size)
   push dword [ebx + 16]      ;rogram header中偏移16字节的地方是p_filesz,压入函数memcpy的第三个参数:size
   
   mov eax, [ebx + 4]         ;距程序头偏移量为4字节的位置是p_offset
   add eax, KERNEL_BIN_BASE_ADDR
   push eax                   ;压入函数memcpy的第二个参数:源地址

   push dword [ebx + 8]       ;压入函数memcpy的第一个参数:目的地址,偏移程序头8字节的位置是p_vaddr，这就是目的地址
   
   call mem_cpy

   add esp, 12                ;清理栈中压入的三个参数
.PTNULL:
   add ebx, edx         ;edx程序头表中一个条目的大小,即e_phentsize,在此ebx指向下一个program header 
   loop .each_segment  
   ret


;-------------------------------------------------------
;-------------   setup_page     ------------------------
;功能: 1.在页目录表的起始地址的(0x10_0000),清空4KB个字节,来存放页目录表
;      2.在第0个页目录项和第768个目录表项中填入第一个页表的地址,我们规划内存,管理内存, 
;     将第一个页表的物理地址设在10_1000(页目录表之后),一次类推第二个页表的物理地址是10_2000 
;     第三个是10_3000, 
;     低的1MB内存保存着BIOS,显存地址,中断向量表,对应物理地址是 [0 ~ FFFFF] , 等等在开启分页之后
;     我们要保证这些虚拟地址 对应的 物理地址 是一致的。
;     解释一下: 实模式的时候, 我们通过 段地址 + 段内偏移 计算结果得出的线性地址 直接等于 物理地址
;              保护模式,我们通过段选择子, 去查GDT(全局描述符表), 得到段基址,  在加上段内偏移 得到虚拟地址
;              保护模式下 线性地址就等于虚拟地址, 因为我们在32位机器下工作, 内存大小为4GB, 只需要使用段内偏移
;              就可以访问到所有内存,所以把段基址设计为0, 只就是平坦模型, 只在一个段下工作。 说回来
;              此时的虚拟地址是需要通过二级页表转化成为物理地址的, 所以我们要保证虚拟地址转化后还对应的是原本的物理地址
;    怎么保证 ？ (其实是依靠我们自己规划内存)
;           一个页表对应4MB, 我们规定第一个页表中的物理块对应低段1MB(实际物理地址0 ~ FFFFF), 在这1MB里面, 我们也会写内核代码。
;           因为我们要满足我们“保证”,那虚拟地址FFFFF来说, 我们就要把第一个页表的地址填入第 0 个页目录项
;           因为要满足虚拟地址3G以上的虚拟地址空间对应这1MB, 我们就要把第一个页表的地址填入 等768个页目录项
;    3. 然后我们初始化第一页表里面的页表项, 因为暂时只用到低端1MB, 我们只初始化4MB中的前1MB, 也就是256个页表项
;    初始化也简单, 根据FFFFF(会对应255个页表项（从0开始）)理解, 
;    我们只要在第一个页表项里面填入内存起始地址0x 0000_0000(第一个物理块的地址), 
;    在第二个页表项里面填入 0x0000_1000(第二个物理块的地址),在第三个页表项里面填入0x0000_2000(第三个物理块的地址)....
;    为什么要这样填？ 因为只有这样填, 才可以满足我们的那个“保证”,未来的我如果不太理解, 就使用低端的地址模拟一下二级页表的转换过程,
;    只有, 只有, 只有这样填才是正确的,这是必然。(感觉里面包含了一些深层数制的东西, 非常奇妙, 非常巧合) 
;    4. 最后一步,  操作系统的虚拟地址是0xc000_0000 ~ 0xffff_feff，可以计算出映射的应该是(768 ~ 1022)个页目录项,
;       我们规定1023页目录项存储页目录表的地址。 按理说一个页表已经足够映射我们的操作系统代码，但为了真正实现内核被所有进程共享，
;       我们格外初始化了[769 , 1022]所有页目录项, 用第2个页表地址初始化第769个页目录项,用第3个页表地址初始化第770个页目录项.. 一次类推
;       [769 , 1022]的页目录项对应的页表并没有被初始化.
;       问 什么叫真正实现内核被所有进程共享？
;补充阅读:
;由于我们在32位机器下制作操作系统, 我们的操作系统代码不会超过1MB
;一个页目录表共有1024个页目录项，一个页目录项存放一个页表地址，
;一个页目录项大小是4字节，所以页目录表占用的物理内存位4KB, 一个物理页。
;一个页表共有1024个页表项, 一个页表项存放一个物理页地址, 一个页表项大小是4字节,所以一个页表占用的物理内存位4KB, 一个物理页。
;一个页表可以映射的大小为4MB。一个页目录表可以映射的大小为4GB,32位机器，内存最大是4GB
;-------------------------------------------------------
setup_page:
;先把页目录占用的空间逐字节清0
   mov ecx, 4096
   mov esi, 0
.clear_page_dir:
   mov byte [PAGE_DIR_TABLE_POS + esi], 0
   inc esi
   loop .clear_page_dir

;开始创建页目录项(PDE)
.create_pde:				     ; 创建Page Directory Entry
   mov eax, PAGE_DIR_TABLE_POS
   add eax, 0x1000 			     ; 此时eax为第一个页表的位置及属性
   mov ebx, eax				     ; 此处为ebx赋值，是为.create_pte做准备，ebx为基址。

;   下面将页目录项0和 768 都存为第一个页表的地址，
;   一个页表可表示4MB内存,这样0xc03fffff以下的地址和0x003fffff以下的地址都指向相同的页表，
;   这是为将地址映射为内核地址做准备
   or eax, PG_US_U | PG_RW_W | PG_P	     ; 页目录项的属性RW和P位为1,US为1,表示用户属性,所有特权级别都可以访问.
   mov [PAGE_DIR_TABLE_POS + 0x0], eax       ; 第1个目录项,在页目录表中的第1个目录项写入第一个页表的位置(0x101000)及属性(7)
   mov [PAGE_DIR_TABLE_POS + 0xc00], eax     ; 一个页表项占用4字节,0xc00表示第768个页表占用的目录项,0xc00以上的目录项用于内核空间,
					     ; 也就是页表的0xc0000000~0xffffffff共计1G属于内核,0x0~0xbfffffff共计3G属于用户进程.
   
   sub eax, 0x1000
   mov [PAGE_DIR_TABLE_POS + 4092], eax	     ; 使最后一个目录项指向页目录表自己的地址

;下面创建页表项(PTE)
   mov ecx, 256				     ; 1M低端内存 / 每页大小4k = 256
   mov esi, 0
   mov edx, PG_US_U | PG_RW_W | PG_P	     ; 属性为7,US=1,RW=1,P=1
.create_pte:				     ; 创建Page Table Entry
   mov [ebx+esi*4],edx			     ; 此时的ebx已经在上面通过eax赋值为0x101000,也就是第一个页表的地址 
   add edx,4096      ; edx
   inc esi
   loop .create_pte

;创建内核其它页表的PDE
   mov eax, PAGE_DIR_TABLE_POS
   add eax, 0x2000 		     ; 此时eax为第二个页表的位置
   or eax, PG_US_U | PG_RW_W | PG_P  ; 页目录项的属性US,RW和P位都为1
   mov ebx, PAGE_DIR_TABLE_POS
   mov ecx, 254			     ; 范围为第769~1022的所有目录项数量
   mov esi, 769
.create_kernel_pde:
   mov [ebx+esi*4], eax
   inc esi
   add eax, 0x1000
   loop .create_kernel_pde
   ret

;-------------------------------------------------------
;----------------mem_cpy(dst, src, size)--------
;输入: 栈中三个参数(dst 源地址, src 目的地址, size 字节大小)
;功能：从 dst 的位置逐字节拷贝到 src 的位置
;-------------------------------------------------------
mem_cpy:
   cld         ;控制 esi, edi 递增，由 ds:esi 传送到 es:edi。正向传送,每传送一次cx的内容自动减一
	push ebp
   mov ebp, esp
   push ecx    ; rep指令用到了ecx，但ecx对于外层段的循环还有用，故先入栈备份
   
   mov edi, [ebp + 8]   ;dst
   mov esi, [ebp + 12]  ;src
   mov ecx, [ebp + 16]  ;size
   rep movsb

   ;恢复环境
   pop ecx
   pop ebp
   ret
;----------------------------------------------------------------
;--------------------------- rd_disk_m_32 -----------------------
;输入:  eax=LBA扇区号, ebx=将数据写入的内存地址, ecx=读入的扇区数
;功能:  读取硬盘n个扇区
;----------------------------------------------------------------
rd_disk_m_32:	   
      mov esi,eax	  ;备份eax
      mov di,cx	  ;备份cx
;读写硬盘:
;第1步：设置要读取的扇区数
      mov dx,0x1f2
      mov al,cl
      out dx,al            ;读取的扇区数

      mov eax,esi	   ;恢复ax

;第2步：将LBA地址存入0x1f3 ~ 0x1f6

      ;LBA地址7~0位写入端口0x1f3
      mov dx,0x1f3                       
      out dx,al                          

      ;LBA地址15~8位写入端口0x1f4
      mov cl,8
      shr eax,cl
      mov dx,0x1f4
      out dx,al

      ;LBA地址23~16位写入端口0x1f5
      shr eax,cl
      mov dx,0x1f5
      out dx,al

      shr eax,cl
      and al,0x0f	   ;lba第24~27位
      or al,0xe0	   ; 设置7～4位为1110,表示lba模式
      mov dx,0x1f6
      out dx,al

;第3步：向0x1f7端口写入读命令，0x20 
      mov dx,0x1f7
      mov al,0x20                        
      out dx,al
;;;;;;; 至此,硬盘控制器便从指定的lba地址(eax)处,读出连续的cx个扇区,下面检查硬盘状态,不忙就能把这cx个扇区的数据读出来

;第4步：检测硬盘状态
  .not_ready:
      ;同一端口，写时表示写入命令字，读时表示读入硬盘状态
      nop
      in al,dx
      and al,0x88	   ;第4位为1表示硬盘控制器已准备好数据传输，第7位为1表示硬盘忙
      cmp al,0x08
      jnz .not_ready	   ;若未准备好，继续等。

;第5步：从0x1f0端口读数据
      mov ax, di
      mov dx, 256
      mul dx
      mov cx, ax	   ; di为要读取的扇区数，一个扇区有512字节，每次读入一个字，
			   ; 共需di*512/2次，所以di*256
      mov dx, 0x1f0
  .go_on_read:
      in ax,dx
      mov [ebx],ax
      add ebx,2		  
      loop .go_on_read
           ; 由于在实模式下偏移地址为16位,所以用bx只会访问到0~FFFFh的偏移。
			  ; loader的栈指针为0x900,bx为指向的数据输出缓冲区,且为16位，
			  ; 超过0xffff后,bx部分会从0开始,所以当要读取的扇区数过大,待写入的地址超过bx的范围时，
			  ; 从硬盘上读出的数据会把0x0000~0xffff的覆盖，
			  ; 造成栈被破坏,所以ret返回时,返回地址被破坏了,已经不是之前正确的地址,
			  ; 故程序出会错,不知道会跑到哪里去。
			  ; 所以改为ebx代替bx指向缓冲区,这样生成的机器码前面会有0x66和0x67来反转。
      ret