BIN = bin
FS_IMAGE = disk/fs.img
PROGRAM_IMAGE = disk/HELLO.JXE
USER_CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -O2 -Wall -Wextra -fcf-protection=none -fno-asynchronous-unwind-tables -fno-ident -Iinclude
USER_LDFLAGS = -m elf_i386
KERNEL_SECTORS = 64

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector \
         -nostdlib -O2 -Wall -Wextra -fcf-protection=none \
         -fno-asynchronous-unwind-tables -fno-ident

all: $(BIN)/os.img

$(BIN):
	mkdir -p $(BIN)

disk:
	mkdir -p disk

$(BIN)/boot.bin: boot.asm | $(BIN)
	nasm -f bin boot.asm -o $@
	@test $$(stat -c%s $@) -eq 512

$(BIN)/kernel_entry.o: kernel_entry.asm | $(BIN)
	nasm -f elf32 kernel_entry.asm -o $@

$(BIN)/kernel.o: kernel.c | $(BIN)
	gcc $(CFLAGS) -c kernel.c -o $@

$(BIN)/kernel.bin: $(BIN)/kernel_entry.o $(BIN)/kernel.o linker.ld
	ld -m elf_i386 -T linker.ld --build-id=none --oformat binary \
	   $(BIN)/kernel_entry.o $(BIN)/kernel.o -o $@
	@test $$(stat -c%s $@) -le $$(($(KERNEL_SECTORS)*512))

USER_BIN = bin/user/hello.bin
USER_OBJ = bin/user/hello.o
USER_CRT = bin/user/crt0.o

$(USER_CRT): user/crt0.S | $(BIN)
	mkdir -p bin/user
	gcc $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ): user/hello.c include/jupiter.h | $(BIN)
	mkdir -p bin/user
	gcc $(USER_CFLAGS) -c $< -o $@

$(USER_BIN): $(USER_CRT) $(USER_OBJ) user/linker.ld | $(BIN)
	ld $(USER_LDFLAGS) -T user/linker.ld --build-id=none --oformat binary \
	   $(USER_CRT) $(USER_OBJ) -o $@
	@test $$(stat -c%s $@) -le 4080

$(PROGRAM_IMAGE): $(USER_BIN) tools/make_jexe.py | disk
	python3 tools/make_jexe.py $(USER_BIN) $@

LOOP_BIN = bin/user/loop.bin
LOOP_OBJ = bin/user/loop.o
LOOP_JXE = disk/LOOP.JXE

$(LOOP_OBJ): user/loop.c include/jupiter.h | $(BIN)
	mkdir -p bin/user
	gcc $(USER_CFLAGS) -c $< -o $@

$(LOOP_BIN): $(USER_CRT) $(LOOP_OBJ) user/linker.ld | $(BIN)
	ld $(USER_LDFLAGS) -T user/linker.ld --build-id=none --oformat binary \
	   $(USER_CRT) $(LOOP_OBJ) -o $@
	@test $$(stat -c%s $@) -le 4080

$(LOOP_JXE): $(LOOP_BIN) tools/make_jexe.py | disk
	python3 tools/make_jexe.py $(LOOP_BIN) $@

$(FS_IMAGE): tools/mkfs.py $(PROGRAM_IMAGE) $(LOOP_JXE) | disk
	python3 tools/mkfs.py

$(BIN)/os.img: $(BIN)/boot.bin $(BIN)/kernel.bin $(FS_IMAGE)
	cat $(BIN)/boot.bin $(BIN)/kernel.bin > $@
	truncate -s $$(( (1 + $(KERNEL_SECTORS)) * 512 )) $@
	cat $(FS_IMAGE) >> $@
	truncate -s $$(( (1 + $(KERNEL_SECTORS) + 64) * 512 )) $@

run: $(BIN)/os.img
	qemu-system-i386 -drive format=raw,file=$(BIN)/os.img,if=ide -boot c

debug: $(BIN)/os.img
	qemu-system-i386 -drive format=raw,file=$(BIN)/os.img,if=ide -boot c -no-reboot -d int,cpu_reset -D $(BIN)/qemu.log

# Re-create the filesystem and restore the default files/programs.
format: $(PROGRAM_IMAGE) $(LOOP_JXE) | disk
	python3 tools/mkfs.py

# The filesystem in disk/ is intentionally preserved by clean.
clean:
	rm -rf $(BIN)

.PHONY: all run debug format clean user

user: $(PROGRAM_IMAGE) $(LOOP_JXE)
