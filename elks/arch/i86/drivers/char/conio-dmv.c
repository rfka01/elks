/*
 * conio API for the NCR Decision Mate V
 *
 * Output goes straight to the uPD7220 GDC in character mode, input comes
 * from the 8741 keyboard controller by polling. No firmware is used, so
 * nothing here touches the firmware area at 0F000h-0FFFFh.
 *
 * Hardware facts from rfka01/dmv_hardware, docs/DMV-Hardware-Boot-und-
 * Bildschirm.md, sections 4 (keyboard) and 5 (GDC):
 *
 *   GDC: A1h command, A0h parameter (write) / status (read), status bit 1
 *   FIFO full. 80 x 25 characters, one 16-bit word per cell: character in
 *   the low byte, attribute in the high byte (E8h is normal on both the
 *   colour and the mono board). CURS 49h, MASK 4Ah, FIGS 4Ch (direction 2
 *   = left to right), WDAT 20h, PRAM 70h. FIGS and its DC stay set across
 *   WDAT, so DC is set back to 0 after every block.
 *
 *   The text memory is a ring of 2000 words shown through two partitions:
 *   partition 1 from the top of the ring to its end, partition 2 from
 *   address 0 for the rows that are missing. Scrolling clears the oldest
 *   row and moves the top by 80 words; nothing is copied.
 *
 *   8741: 41h status / command, 40h data, status bit 0 = byte ready.
 *   Command 01h initialises the controller and answers with the country
 *   code (status bit 7 set); keys pressed before come first. The Enter key
 *   sends 88h, special keys 80h-9Fh.
 *
 * 2026, rfka01 and Claude (Anthropic). GPL-2.0, like ELKS.
 */

#include <linuxmt/config.h>
#include <linuxmt/types.h>
#include <arch/io.h>
#include <arch/ports.h>
#include "conio.h"

#define GDC_FIFO_FULL   0x02
#define GDC_CURS        0x49
#define GDC_MASK        0x4A
#define GDC_FIGS        0x4C
#define GDC_WDAT        0x20
#define GDC_PRAM        0x70
#define GDC_DIR_RIGHT   0x02

#define COLS            80
#define ROWS            25
#define RING            (COLS * ROWS)
#define ATTR_NORMAL     0xE8

static unsigned int top;            /* ring address of the first visible row */
static unsigned char row, col;
static unsigned char kbd_country = 0xFF;    /* 0xFF: answer to 01h still due */
static unsigned char ready;         /* screen set up; early printk comes first */

static void gdc_cmd(unsigned char c)
{
    while (inb(GDC_STATUS_PORT) & GDC_FIFO_FULL)
        ;
    outb(c, GDC_CMD_PORT);
}

static void gdc_par(unsigned char p)
{
    while (inb(GDC_STATUS_PORT) & GDC_FIFO_FULL)
        ;
    outb(p, GDC_STATUS_PORT);
}

/* ring address of a screen position */
static unsigned int cell(unsigned char r, unsigned char c)
{
    unsigned int a = top + r * COLS + c;

    return (a >= RING) ? a - RING : a;
}

/* CURS also loads MASK from the dot address; words need MASK = FFFFh */
static void gdc_at(unsigned int a)
{
    gdc_cmd(GDC_CURS);
    gdc_par((unsigned char)a);
    gdc_par((unsigned char)(a >> 8));
    gdc_par(0);
    gdc_cmd(GDC_MASK);
    gdc_par(0xFF);
    gdc_par(0xFF);
}

/* write n (>= 1) equal words from ring address a, n must not cross the end */
static void gdc_fill(unsigned int a, unsigned int n, unsigned char ch)
{
    gdc_at(a);
    gdc_cmd(GDC_FIGS);
    gdc_par(GDC_DIR_RIGHT);
    gdc_par((unsigned char)(n - 1));
    gdc_par((unsigned char)((n - 1) >> 8));
    gdc_cmd(GDC_WDAT);
    gdc_par(ch);
    gdc_par(ATTR_NORMAL);
    gdc_cmd(GDC_FIGS);              /* DC back to 0 */
    gdc_par(GDC_DIR_RIGHT);
    gdc_par(0);
    gdc_par(0);
}

/* show the ring from 'top': partition 1 to the end, partition 2 from 0 */
static void gdc_partitions(void)
{
    unsigned char first = top / COLS;

    gdc_cmd(GDC_PRAM);
    gdc_par((unsigned char)top);
    gdc_par((unsigned char)(top >> 8));
    gdc_par(0);
    gdc_par(ROWS - first);
    gdc_par(0);
    gdc_par(0);
    gdc_par(0);
    gdc_par(first);
}

static void cursor(void)
{
    gdc_at(cell(row, col));
}

static void newline(void)
{
    if (++row < ROWS)
        return;
    row = ROWS - 1;
    gdc_fill(top, COLS, ' ');       /* the oldest row becomes the new bottom */
    top += COLS;
    if (top >= RING)
        top = 0;
    gdc_partitions();
}

void conio_init(void)
{
    if (ready)
        return;
    ready = 1;
    top = 0;
    row = col = 0;
    gdc_fill(0, RING, ' ');
    gdc_partitions();
    cursor();
    outb(0x01, KBD_STATUS_PORT);    /* initialise the 8741, country code follows */
}

void conio_putc(byte_t c)
{
    if (!ready)
        conio_init();
    switch (c) {
    case '\r':
        col = 0;
        break;
    case '\n':
        newline();
        break;
    case '\b':
        if (col)
            col--;
        break;
    case '\t':
        do {
            conio_putc(' ');
        } while (col & 7);
        return;
    default:
        if (c < ' ')
            return;
        gdc_at(cell(row, col));
        gdc_cmd(GDC_WDAT);
        gdc_par(c);
        gdc_par(ATTR_NORMAL);
        if (++col >= COLS) {
            col = 0;
            newline();
        }
        break;
    }
    cursor();
}

int conio_poll(void)
{
    unsigned char st, k;

    st = inb(KBD_STATUS_PORT);
    if (!(st & 0x01))
        return 0;
    k = inb(KBD_DATA_PORT);
    if (kbd_country == 0xFF && (st & 0x80)) {
        kbd_country = k;            /* E8h + DIP switch setting */
        return 0;
    }
    if (k == 0x88)                  /* Enter */
        return '\r';
    if (k >= 0x80)                  /* other special keys: not yet */
        return 0;
    return k;
}
