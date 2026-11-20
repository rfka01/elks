/*
 * Interrupt controller of the NCR Decision Mate V (8259 on the K235 card)
 *
 * The 8259 sits at 90h/91h. Wiring (K235 schematic 017-0033502, sheet 3):
 *   IR0 8253 channel 2 (timer)   IR1 8741 keyboard     IR2 INT/ of slots 2-6
 *   IR3 slot 3                   IR4 slot 4            IR5 slot 2a (hard disk)
 *   IR6 8272 floppy              IR7 slot 5 or 6
 * IR2..IR6 have pull-ups: an empty slot requests an interrupt all the time,
 * so only enable inputs that have a driver behind them.
 *
 * The K235 ROM leaves the 8259 level triggered with normal EOI and all
 * inputs masked. NCR's DOS BIOS switches to edge triggered and AEOI; so
 * do we, then irqtab.S needs no EOI.
 *
 * rfka01/dmv_hardware, docs/DMV-Hardware-Boot-und-Bildschirm.md, section 2.
 */

#include <linuxmt/config.h>
#include <linuxmt/errno.h>
#include <linuxmt/sched.h>
#include <linuxmt/types.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/ports.h>

int dmv_pic;                     /* 1: K235 with 8259, 0: K230, no interrupts */

void initialize_irq(void)
{
    outb(0x17, PIC1_CMD);       /* ICW1: edge triggered, single, ICW4 follows */
    outb(0x08, PIC1_DATA);      /* ICW2: IR0 is INT 08h */
    outb(0x03, PIC1_DATA);      /* ICW4: AEOI, 8086/88 mode */
    outb(0x5A, PIC1_DATA);      /* OCW1 with a test pattern: the K230 has */
    dmv_pic = (inb(PIC1_DATA) == 0x5A); /* nothing at 91h and reads FFh */
    outb(0xFF, PIC1_DATA);      /* OCW1: everything masked */
}

void enable_irq(unsigned int irq)
{
    if (irq > 7)
        return;
    outb(inb(PIC1_DATA) & ~(1 << irq), PIC1_DATA);
}

void disable_irq(unsigned int irq)
{
    flag_t flags;

    if (irq > 7)
        return;
    save_flags(flags);
    clr_irq();
    outb(inb(PIC1_DATA) | (1 << irq), PIC1_DATA);
    restore_flags(flags);
}

int remap_irq(int irq)
{
    if ((unsigned int)irq > 7)
        return -EINVAL;
    return irq;
}

/* IR0..IR7 are INT 08h..0Fh */
int irq_vector(int irq)
{
    return irq + 0x08;
}
