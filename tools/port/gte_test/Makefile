# GTE bit-exactness test. `make` builds the PS1 EXE and the host checker; `make run` needs DuckStation.
CC_PS1  := mipsel-linux-gnu-gcc
CFLAGS_PS1 = -DGTE_TEST_SEED=$(SEED) -march=r3000 -mabi=32 -mfp32 -msoft-float -mno-abicalls -fno-pic -G0 -O2 -nostdlib -ffreestanding -fno-builtin
ROOT := ../../..
SEED ?= 0

all: gte_test.exe check

cmds.h cmds.s: gen.py
	python3 gen.py

crt0.o: crt0.s
	$(CC_PS1) $(CFLAGS_PS1) -c $< -o $@
cmds.o: cmds.s
	$(CC_PS1) $(CFLAGS_PS1) -c $< -o $@
test_ps1.o: test_ps1.c cmds.h inputs.h
	$(CC_PS1) $(CFLAGS_PS1) -c $< -o $@

gte_test.elf: crt0.o cmds.o test_ps1.o link.ld
	$(CC_PS1) $(CFLAGS_PS1) -static -no-pie -T link.ld -Wl,--no-warn-rwx-segments -o $@ crt0.o cmds.o test_ps1.o -lgcc

gte_test.exe: gte_test.elf
	mipsel-linux-gnu-objcopy -O binary -j .text -j .rodata -j .data $< gte_test.bin
	python3 mkexe.py gte_test.bin 0x$$(mipsel-linux-gnu-nm $< | awk '$$3=="_start"{print $$1}') $@

check: check.c inputs.h cmds.h $(ROOT)/src/port/gte.c
	cc -O2 -Wall -DGTE_TEST_SEED=$(SEED) -I$(ROOT)/include -o $@ check.c $(ROOT)/src/port/gte.c

clean:
	rm -f *.o *.elf *.bin *.exe check results.bin cmds.h cmds.s
