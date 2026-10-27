# libgte/libkmath equivalence test: PS1 originals (DuckStation) vs the port's versions (PCSX2): recompiled
# libgte/libgs, C libkmath.
# `make` builds both; `python3 run.py` runs both and prints the comparison.
ROOT := ../../..
HAL  ?= $(ROOT)/../psx-to-ps2-HAL
GTE_TEST := ../gte_test
CC_PS1 := mipsel-linux-gnu-gcc
CFLAGS_PS1 := -DLIB_TEST_PS1 -march=r3000 -mabi=32 -mfp32 -msoft-float -mno-abicalls -fno-pic -G0 -O2 -nostdlib -ffreestanding -fno-builtin -std=gnu89
PS2DEV ?= $(HOME)/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
CC_EE := $(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-gcc
CFLAGS_EE := -O2 -G0 -std=gnu89 -fno-builtin -I$(ROOT)/include -I$(HAL)/include -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include
LIBGTE_OBJS := $(wildcard $(ROOT)/lib/libgte/*.o)
LIBGS_OBJS := $(wildcard $(ROOT)/lib/libgs/*.o)
LIBGPU_OBJS := $(wildcard $(ROOT)/lib/libgpu/*.o)
LIBKMATH := $(ROOT)/build/USA/src/bodyprog/libkmath/libkmath.s.o
RECOMP := $(ROOT)/build/port/recomp
# GsTMDfast* (g3/g4/gt3/gt4) need libgs and real model data: tested with libgs instead.
NOT_TESTED := g3 g4 gt3 gt4
RECOMP_C := $(filter-out $(foreach o,$(NOT_TESTED),$(RECOMP)/libgte_$(o).c),$(wildcard $(RECOMP)/libgte_*.c) $(wildcard $(RECOMP)/libgs_*.c))

all: lib_test_ps1.exe

lib_tests.h: gen.py
	python3 gen.py

crt0.o: $(GTE_TEST)/crt0.s
	$(CC_PS1) $(CFLAGS_PS1) -c $< -o $@

lib_test_ps1.o: lib_test.c lib_tests.h
	$(CC_PS1) $(CFLAGS_PS1) -c $< -o $@

# Only the original objects that define something the test needs are pulled in (--whole-archive off).
libgte.a: $(LIBGTE_OBJS)
	rm -f $@; mipsel-linux-gnu-ar rcs $@ $(LIBGTE_OBJS)

libgs.a: $(LIBGS_OBJS)
	rm -f $@; mipsel-linux-gnu-ar rcs $@ $(LIBGS_OBJS)

libgpu.a: $(LIBGPU_OBJS)
	rm -f $@; mipsel-linux-gnu-ar rcs $@ $(LIBGPU_OBJS)

lib_test_ps1.elf: crt0.o lib_test_ps1.o libgte.a libgs.a libgpu.a
	$(CC_PS1) $(CFLAGS_PS1) -static -no-pie -T $(GTE_TEST)/link.ld -Wl,--no-warn-rwx-segments -o $@ crt0.o lib_test_ps1.o $(LIBKMATH) -Wl,--start-group libgs.a libgte.a libgpu.a -Wl,--end-group -lgcc

lib_test_ps1.exe: lib_test_ps1.elf
	mipsel-linux-gnu-objcopy -O binary -j .text -j .rodata -j .data $< lib_test_ps1.bin
	python3 $(GTE_TEST)/mkexe.py lib_test_ps1.bin 0x$$(mipsel-linux-gnu-nm $< | awk '$$3=="_start"{print $$1}') $@

# EE side: needs ps1_results.bin from run.py.
ps1_results.o: ps1_results.bin
	printf '.section .rodata\n.balign 16\n.globl ps1_results\nps1_results:\n.incbin "ps1_results.bin"\n' > ps1_results.s
	$(CC_EE) -c ps1_results.s -o $@

# The recompiled code as an archive (like tools/port/port_link.py), so only what's used is linked.
librecomp_ee.a: $(RECOMP_C) $(wildcard $(RECOMP)/bridge_*.c)
	rm -rf ee_objs $@; mkdir -p ee_objs
	for f in $(RECOMP_C) $(wildcard $(RECOMP)/bridge_*.c); do \
		b=ee_objs/$$(basename $$f .c); \
		$(CC_EE) $(CFLAGS_EE) -nostdinc -c $$f -o $$b.o && $(CC_EE) -c -G0 $${f%.c}.data.s -o $$b.data.o || exit 1; \
	done
	$(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-ar rcs $@ ee_objs/*.o

# The port's C libraries (SH1's src/port/libkmath_*.c; the HAL's libgte_*.c etc.), built as the game's port C is.
PORT_LIB_C := libkmath_port libkmath_table libgte_port libgte_tables libgpu_packets libgs_port
PORT_LIB_O := $(addsuffix .o,$(PORT_LIB_C))
vpath %.c $(ROOT)/src/port $(HAL)/src
$(PORT_LIB_O): %.o: %.c
	$(CC_EE) -O2 -G0 -std=gnu89 -nostdinc -I$(ROOT)/include -I$(HAL)/include -I$(ROOT)/include/psyq -I$(ROOT)/include/decomp -Wa,-I$(ROOT)/include \
		-D_LANGUAGE_C -DVER_USA -DSH_PORT -DNON_MATCHING -c $< -o $@

lib_test_ee.elf: lib_test.c lib_tests.h ps1_results.o librecomp_ee.a $(PORT_LIB_O)
	$(CC_EE) $(CFLAGS_EE) -T$(PS2SDK)/ee/startup/linkfile -L$(PS2SDK)/ee/lib -Wl,-zmax-page-size=128 -o $@ \
		lib_test.c ps1_results.o $(PORT_LIB_O) $(HAL)/src/gte.c $(HAL)/src/recomp_rt.c \
		-Wl,--start-group librecomp_ee.a -Wl,--end-group

clean:
	rm -rf *.o *.elf *.bin *.exe *.a ee_objs ps1_results.s lib_tests.h
