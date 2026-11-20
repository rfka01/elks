/*
 * System tick of the NCR Decision Mate V
 *
 * Channel 2 of the 8253 on the main board (82h data, 83h control word) is
 * clocked with 500 kHz and goes to IR0 of the 8259 on the K235. A count of
 * 5000 gives 10 ms, HZ = 100.
 *
 * The K230 has no 8259 and the 8088 gets no interrupts at all, but the
 * 8253 counts all the same. Without a PIC (dmv_pic = 0, see irq-dmv.c)
 * the idle loop therefore watches channel 2 instead of halting: when the
 * count has run through, it raises INT 08h itself. Time, timeouts and the
 * keyboard poll then work whenever the system is idle; a busy process
 * is not preempted.
 */

#include <linuxmt/config.h>
#include <linuxmt/timer.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/param.h>
#include <arch/ports.h>

#define TIMER_CH2_MODE2 0xB4        /* channel 2, LSB+MSB, mode 2 (rate), binary */
#define TIMER_CH2_LATCH 0x80        /* counter latch for channel 2 */
#define TIMER_COUNT     ((unsigned int)(TIMER_CLOCK_HZ / HZ))

extern int dmv_pic;

void enable_timer_tick(void)
{
    outb(TIMER_CH2_MODE2, TIMER_CMDS_PORT);
    outb((unsigned char)TIMER_COUNT, TIMER_DATA_PORT);
    outb((unsigned char)(TIMER_COUNT >> 8), TIMER_DATA_PORT);
}

void disable_timer_tick(void)
{
}

static unsigned int timer_count(void)
{
    unsigned int lo;

    outb(TIMER_CH2_LATCH, TIMER_CMDS_PORT);
    lo = inb(TIMER_DATA_PORT);
    return lo | (inb(TIMER_DATA_PORT) << 8);
}

void idle_halt(void)
{
    unsigned int last, now;

    if (dmv_pic) {
        asm volatile ("hlt");
        return;
    }
    /* K230: wait until the count reloads (counts down, so it grows), then tick */
    last = timer_count();
    for (;;) {
        now = timer_count();
        if (now > last)
            break;
        last = now;
    }
    asm volatile ("int $0x08");
}
