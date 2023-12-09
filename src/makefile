CC = gcc
AS = nasm
LD = ld

# 可执行文件目录
BUILD_DIR = /home/steven/SKOS/src/build

# 源文件根目录
SRC_DIR = /home/steven/SKOS/src
# 编译参数。
CFLAGS = -O0 -Wall -I $(SRC_DIR)/lib/ -I $(SRC_DIR)/kernel/ -I $(SRC_DIR)/device/ \
		-I $(SRC_DIR)/thread/ -I $(SRC_DIR)/userprog/ -I $(SRC_DIR)/fs/  -I $(SRC_DIR)/shell\
		-fno-builtin -m32 -fno-stack-protector -nostdinc -fno-pic -march=i386 \
		-fno-pie -nostdlib -c -g \

# 需要被链接的目标文件
OBJS =  $(BUILD_DIR)/main.o $(BUILD_DIR)/init.o $(BUILD_DIR)/interrupt.o \
		$(BUILD_DIR)/timer.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/print.o \
		$(BUILD_DIR)/debug.o $(BUILD_DIR)/memory.o $(BUILD_DIR)/bitmap.o \
		$(BUILD_DIR)/string.o $(BUILD_DIR)/thread.o $(BUILD_DIR)/list.o \
		$(BUILD_DIR)/switch_to.o $(BUILD_DIR)/sync.o $(BUILD_DIR)/console.o \
		$(BUILD_DIR)/keyboard.o $(BUILD_DIR)/ioqueue.o $(BUILD_DIR)/tss.o \
		$(BUILD_DIR)/process.o $(BUILD_DIR)/syscall.o $(BUILD_DIR)/syscall-init.o \
		$(BUILD_DIR)/stdio.o $(BUILD_DIR)/stdio-kernel.o $(BUILD_DIR)/ide.o	\
		$(BUILD_DIR)/fs.o $(BUILD_DIR)/dir.o $(BUILD_DIR)/file.o $(BUILD_DIR)/inode.o \
		$(BUILD_DIR)/fork.o $(BUILD_DIR)/shell.o $(BUILD_DIR)/assert.o \
		$(BUILD_DIR)/buildin_cmd.o $(BUILD_DIR)/exec.o $(BUILD_DIR)/wait_exit.o \
		$(BUILD_DIR)/pipe.o

all: $(BUILD_DIR)/mbr.bin $(BUILD_DIR)/loader.bin $(BUILD_DIR)/kernel.bin

$(BUILD_DIR)/mbr.bin: $(SRC_DIR)/boot/mbr.s
	nasm -I $(SRC_DIR)/boot/include/ -o $@ $^
	dd if=$(BUILD_DIR)/mbr.bin of=/home/steven/SKOS/bochs/hd60M.img bs=512 count=1 conv=notrunc 

$(BUILD_DIR)/loader.bin: $(SRC_DIR)/boot/loader.s
	nasm -I $(SRC_DIR)/boot/include/ -o $@ $^
	dd if=$(BUILD_DIR)/loader.bin of=/home/steven/SKOS/bochs/hd60M.img bs=512 count=4 seek=2 conv=notrunc
 
$(BUILD_DIR)/kernel.bin: $(OBJS)
	@ld  -Ttext 0xc0001500 -e main -melf_i386 -static -Map  $(BUILD_DIR)/kernel.map $(OBJS) -o $(BUILD_DIR)/kernel.bin 
	dd if=$(BUILD_DIR)/kernel.bin of=/home/steven/SKOS/bochs/hd60M.img bs=512 count=200 seek=9 conv=notrunc
	@echo '感谢世界!系统编译构建完成！ ^_^'

$(BUILD_DIR)/print.o: $(SRC_DIR)/lib/print.S
	@$(AS) -f elf32  -g $< -o $@

$(BUILD_DIR)/kernel.o: $(SRC_DIR)/kernel/kernel.S
	@$(AS) -f elf32  -g $< -o $@

$(BUILD_DIR)/switch_to.o: $(SRC_DIR)/thread/switch_to.S
	@$(AS) -f elf32  -g $< -o $@

$(BUILD_DIR)/main.o: $(SRC_DIR)/kernel/main.c
	@$(CC) $(CFLAGS)  $< -o $@

$(BUILD_DIR)/interrupt.o: $(SRC_DIR)/kernel/interrupt.c
	@$(CC) $(CFLAGS)  $< -o $@

$(BUILD_DIR)/init.o: $(SRC_DIR)/kernel/init.c
	@$(CC) $(CFLAGS)  $< -o $@

$(BUILD_DIR)/timer.o: $(SRC_DIR)/device/timer.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/debug.o: $(SRC_DIR)/kernel/debug.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/string.o: $(SRC_DIR)/lib/string.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/bitmap.o: $(SRC_DIR)/lib/bitmap.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/memory.o: $(SRC_DIR)/kernel/memory.c 
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/thread.o: $(SRC_DIR)/thread/thread.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/list.o: $(SRC_DIR)/lib/list.c 
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/sync.o: $(SRC_DIR)/thread/sync.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/console.o: $(SRC_DIR)/device/console.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/keyboard.o: $(SRC_DIR)/device/keyboard.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/ioqueue.o: $(SRC_DIR)/device/ioqueue.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/tss.o: $(SRC_DIR)/userprog/tss.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/process.o: $(SRC_DIR)/userprog/process.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/syscall-init.o: $(SRC_DIR)/userprog/syscall-init.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/syscall.o: $(SRC_DIR)/lib/syscall.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/stdio.o: $(SRC_DIR)/lib/stdio.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/stdio-kernel.o: $(SRC_DIR)/lib/stdio-kernel.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/ide.o: $(SRC_DIR)/device/ide.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/fs.o: $(SRC_DIR)/fs/fs.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/dir.o: $(SRC_DIR)/fs/dir.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/file.o: $(SRC_DIR)/fs/file.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/inode.o: $(SRC_DIR)/fs/inode.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/fork.o: $(SRC_DIR)/userprog/fork.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/shell.o: $(SRC_DIR)/shell/shell.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/assert.o: $(SRC_DIR)/lib/assert.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/buildin_cmd.o: $(SRC_DIR)/shell/buildin_cmd.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/exec.o: $(SRC_DIR)/userprog/exec.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/wait_exit.o: $(SRC_DIR)/userprog/wait_exit.c
	@$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/pipe.o: $(SRC_DIR)/shell/pipe.c
	@$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f $(BUILD_DIR)/*

.PHONY: bochs 
bochs:
	bochs -q -unlock -f /home/steven/SKOS/bochs/bochsrc
