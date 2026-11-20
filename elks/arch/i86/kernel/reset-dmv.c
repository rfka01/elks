/*
 * Restart of the NCR Decision Mate V
 *
 * There is no warm start. The machine is handed back to the Z80, which
 * then runs its cold start (self test, boot from floppy or hard disk),
 * the same way as REBOOT.COM in rfka01/dmv_os:
 *   - bank 0 on E0h: the register selects the bank for DMA AND the one
 *     the Z80 sees; with another bank the Z80's test of the 8088 fails
 *   - unit 0 on the WD1001 (A0h to C6h), else the Z80 ROM never sees the
 *     hard disk ready
 *   - Z80 code OUT (11h),A / RST 0 at 0:F110
 *   - far jump to FF00:0018, the "back to the Z80" exit of the card ROM
 * rfka01/dmv_hardware, docs/DMV-Hardware-Boot-und-Bildschirm.md, sections 3 and 6.
 */

#include <linuxmt/config.h>
#include <arch/system.h>

void ctrl_alt_del(void)
{
}

void hard_reset_now(void)
{
    asm volatile (
        "cli\n"
        "out %%al,$0xE0\n"
        "mov $0xA0,%%al\n"
        "out %%al,$0xC6\n"
        "xor %%ax,%%ax\n"
        "mov %%ax,%%ds\n"
        "mov %%ax,%%ss\n"
        "mov %%ax,%%sp\n"
        "movw $0x11D3,0xF110\n"
        "movb $0xC7,0xF112\n"
        "ljmp $0xFF00,$0x0018\n"
        : : : "ax", "memory");
}

void apm_shutdown_now(void)
{
    hard_reset_now();
}
