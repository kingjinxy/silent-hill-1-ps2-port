#ifndef _PORT_SPR_H
#define _PORT_SPR_H

/** @brief Places a zero-initialised variable in the EE's 16 KB scratchpad (SPR, 0x70000000):
 * single-cycle loads and stores that never miss or evict anything from the 8 KB data cache.
 *
 * `.spad` is NOBITS and NOLOAD (tools/port/linkfile), so it's no part of the ELF image: main()
 * (src/port/port_main.c) zeroes it before anything else runs. Only for variables without an
 * initial value, and never for anything a DMA reads or writes (the SPR isn't RAM to the DMAC).
 * The "#" ends GCC's own section flags (@progbits) as a comment, leaving @nobits.
 *
 * Only in port builds linked by tools/port/port_link.py (SH_PORT_SPR; SH1_NO_SPR=1 leaves it
 * out). Host and test builds keep these variables in ordinary memory. */
#ifdef SH_PORT_SPR
#define SPR __attribute__((section(".spad,\"aw\",@nobits #")))
#else
#define SPR
#endif

#endif
