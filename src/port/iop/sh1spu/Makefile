# sh1spu.irx (see main.c). Built by tools/port/port_link.py: make -C src/port/iop/sh1spu
IOP_BIN  = sh1spu.irx
IOP_OBJS = main.o xa.o log.o imports.o
IOP_OBJS_DIR = ../../../../build/port/iop/sh1spu/
IOP_BIN := $(IOP_OBJS_DIR)$(IOP_BIN)

all: $(IOP_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
