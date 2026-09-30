BIN = bin

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -O2 -Wall \
         -fcf-protection=none -fno-asynchronous-unwind-tables -fno-ident

all: $(BIN)/os.img

$(BIN):
	mkdir -p $(BIN)

$(BIN)/boot.bin: boot.asm | $(BIN)
	nasm -f bin boot.asm -o $@

$(BIN)/kernel_entry.o: kernel_entry.asm | $(BIN)
	nasm -f elf32 kernel_entry.asm -o $@

$(BIN)/kernel.o: kernel.c | $(BIN)
	gcc $(CFLAGS) -c kernel.c -o $@

$(BIN)/kernel.bin: $(BIN)/kernel_entry.o $(BIN)/kernel.o linker.ld
	ld -m elf_i386 -T linker.ld --build-id=none --oformat binary \
	   $(BIN)/kernel_entry.o $(BIN)/kernel.o -o $@

$(BIN)/os.img: $(BIN)/boot.bin $(BIN)/kernel.bin
	cat $(BIN)/boot.bin $(BIN)/kernel.bin > $@
	truncate -s 8704 $@      # 512 (boot) + 16*512 (kernel sectors)

run: $(BIN)/os.img
	qemu-system-i386 -drive format=raw,file=$(BIN)/os.img

debug: $(BIN)/os.img
	qemu-system-i386 -drive format=raw,file=$(BIN)/os.img -no-reboot -d int,cpu_reset -D $(BIN)/qemu.log

clean:
	rm -rf $(BIN)

.PHONY: all run debug clean