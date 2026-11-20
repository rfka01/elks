/*
 * Memory layout of the NCR Decision Mate V
 *
 * The kernel data segment starts low (KERNEL_DATA), as for every ROM
 * kernel. Linear 0F000h-0FFFFh belongs to the firmware: the K235 ROM keeps
 * cursor and screen variables at F800h-F90Bh, the Z80 and the 8088 talk
 * through the mailbox at FE00h, and a restart puts its Z80 stub at F110h.
 * So the near heap ends below 0F000h, and application memory starts at
 * 64K (SETUP_USERHEAPSEG) and ends below the ROMFS that the boot loader
 * put into RAM (SETUP_MEM_KBYTES).
 */

#include <linuxmt/config.h>
#include <linuxmt/types.h>
#include <linuxmt/kernel.h>
#include <linuxmt/wait.h>
#include <linuxmt/sched.h>
#include <linuxmt/fs.h>
#include <linuxmt/heap.h>
#include <linuxmt/memory.h>
#include <linuxmt/debug.h>

#include <arch/segment.h>
#include <arch/system.h>
#include <arch/io.h>

seg_t membase, memend;  /* start and end segment of available main memory */
unsigned int heapsize;  /* max size of kernel near heap */
byte_t sys_caps;        /* system capabilities bits */
unsigned char arch_cpu; /* CPU type from cputype.S */

unsigned int INITPROC setup_arch(void)
{
    unsigned int endbss, limit;

    endbss = (unsigned int)(_endbss + 1) & ~1;
    limit = (unsigned int)(DMV_FW_LINEAR - ((unsigned long)kernel_ds << 4));
    if (endbss >= limit)
        panic("kernel data reaches the DMV firmware area");
    heapsize = limit - endbss;
    debug("endbss %x heap %x\n", endbss, heapsize);

    membase = SETUP_USERHEAPSEG;
    memend = SETUP_MEM_KBYTES << 6;

    arch_cpu = SETUP_CPU_TYPE;
    sys_caps = SYS_CAPS;

    return endbss;
}

void INITPROC kernel_banner_arch(void)
{
    printk("NCR Decision Mate V, ");
}
