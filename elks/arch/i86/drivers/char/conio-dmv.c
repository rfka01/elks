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
 *   The GDC runs in mixed mode: each of the two partitions is text or a
 *   bitmap (PRAM byte 3 bit 6, IM). A bitmap line is 40 words, 16 pixels
 *   each, bit 0 leftmost; the colour board has the planes green, red, blue
 *   at word 0000h, 4000h, 8000h, the mono board only green (32 KB, so the
 *   other planes fold onto it). The DMV restarts the character line counter
 *   at every partition, so text below a bitmap shows whole rows.
 *
 *   Boot logo: Tux in partition 1 (TUX_ROWS text rows high, bitmap at
 *   TUX_SAD behind the text ring), the text in partition 2 below. On the
 *   first scroll the console goes back to the two text partitions of the
 *   ring and Tux is gone, as the Linux logo scrolls away.
 *
 *   Attribute byte (DMV-Hardware-Boot-und-Bildschirm.md, "Das Attributbyte
 *   bedeutet auf den beiden Karten NICHT dasselbe"): E8h normal on both
 *   boards; bit 0 inverse on mono, no effect on colour; bit 2 red on colour,
 *   half brightness on mono. Highlight is EDh: yellow on colour, inverse on
 *   mono - there is no "bright" on mono.
 *
 *   Escape sequences (a small ANSI subset, enough for full screen tools):
 *     ESC [ r ; c H   cursor to row r, column c (from 1; row 25 reaches
 *                     the status line)
 *     ESC [ K         clear to end of line      ESC [ J  clear the screen
 *     ESC [ n m       0 normal, 1 or 33 highlight, 7 inverse
 *     ESC [ 1 ; 24 r  rows 1-24 scroll, row 25 is a status line;
 *                     ESC [ r goes back to 25 rows and clears it
 *   Form feed clears the screen and homes the cursor. With a status line,
 *   scrolling keeps it the way NCR's ILF1 and the DOS ports do: the line
 *   is kept in a copy here and written back after the ring moved; clearing
 *   leaves it, a line feed in it does not scroll.
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
#include "tux-dmv.h"

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
#define ATTR_HELL       0xED        /* yellow on colour, inverse on mono */
#define ATTR_INVERS     0xE9

#define PLANE_WORDS     0x4000U     /* green, red, blue */
#define BMP_PITCH       40          /* words per bitmap line */
#define TUX_ROWS        6           /* text rows the logo takes */
#define TUX_LINES       (TUX_ROWS * 16)
#define TUX_SAD         0x0800U     /* behind the text ring */
#define TUX_TOP         8           /* lines above Tux */

static unsigned int top;            /* ring address of the first visible row */
static unsigned char row, col;
static unsigned char kbd_country = 0xFF;    /* 0xFF: answer to 01h still due */
static unsigned char ready;         /* screen set up; early printk comes first */
static unsigned char tux_rows;      /* rows taken by the logo, 0 = gone */
static unsigned char attr = ATTR_NORMAL;
static unsigned char statl;         /* 1: row 25 is a status line */
static unsigned short sline[COLS];  /* its copy, char | attr << 8 */
static unsigned char esc, np;       /* escape state: 1 ESC, 2 ESC [ */
static unsigned char par[2];

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

/* write n (>= 1) equal words from address a */
static void gdc_fillw(unsigned int a, unsigned int n, unsigned int w)
{
    gdc_at(a);
    gdc_cmd(GDC_FIGS);
    gdc_par(GDC_DIR_RIGHT);
    gdc_par((unsigned char)(n - 1));
    gdc_par((unsigned char)((n - 1) >> 8));
    gdc_cmd(GDC_WDAT);
    gdc_par((unsigned char)w);
    gdc_par((unsigned char)(w >> 8));
    gdc_cmd(GDC_FIGS);              /* DC back to 0 */
    gdc_par(GDC_DIR_RIGHT);
    gdc_par(0);
    gdc_par(0);
}

/* write n (>= 1, DC 0) different words from address a */
static void gdc_words(unsigned int a, const unsigned short *w, unsigned int n)
{
    gdc_at(a);
    gdc_cmd(GDC_WDAT);
    do {
        gdc_par((unsigned char)*w);
        gdc_par((unsigned char)(*w++ >> 8));
    } while (--n);
}

/* n equal text cells from ring address a, n must not cross the end */
static void gdc_fill(unsigned int a, unsigned int n, unsigned char ch)
{
    gdc_fillw(a, n, ch | (ATTR_NORMAL << 8));
}

/* Tux into the bitmap area. Green last: on the mono board the red and blue
   planes fold onto green, and green is what should stay. */
static void tux_draw(void)
{
    static const unsigned char plane[3] = { 1, 2, 0 };  /* red, blue, green */
    unsigned short line[TUX_W_WORDS];
    const unsigned char *p;
    unsigned char n = 0, zero = 0;
    unsigned int base, y, x;
    int i;

    for (i = 0; i < 3; i++) {
        base = plane[i] * PLANE_WORDS + TUX_SAD;
        gdc_fillw(base, TUX_LINES * BMP_PITCH, 0);
        p = plane[i] == 2 ? tux_blue : tux_green;   /* tux-dmv.h: G = R, B */
        for (y = 0; y < TUX_H; y++) {
            for (x = 0; x < TUX_W_WORDS; x++) {     /* unpack, see mktux-dmv.py */
                if (!n) {
                    zero = *p & 0x80;
                    n = (*p++ & 0x7F) + 1;
                }
                n--;
                if (zero)
                    line[x] = 0;
                else {
                    line[x] = p[0] | (p[1] << 8);
                    p += 2;
                }
            }
            gdc_words(base + (TUX_TOP + y) * BMP_PITCH, line, TUX_W_WORDS);
        }
    }
}

/* show the ring from 'top': partition 1 to the end, partition 2 from 0;
   while the logo is up: partition 1 the logo, partition 2 the text rows
   from ring address 0 (no scrolling has happened yet, top is 0) */
static void gdc_partitions(void)
{
    unsigned char first = top / COLS;

    gdc_cmd(GDC_PRAM);
    if (tux_rows) {
        gdc_par((unsigned char)TUX_SAD);
        gdc_par((unsigned char)(TUX_SAD >> 8));
        gdc_par(0);
        gdc_par(0x40 | tux_rows);   /* IM, length rows * 16 lines */
        gdc_par(0);
        gdc_par(0);
        gdc_par(0);
        gdc_par(ROWS - tux_rows);
        return;
    }
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

/* status line from its copy back to row 25 */
static void sline_put(void)
{
    gdc_words(cell(ROWS - 1, 0), sline, COLS);
}

static void sline_clear(void)
{
    int i;

    for (i = 0; i < COLS; i++)
        sline[i] = ' ' | (ATTR_NORMAL << 8);
    sline_put();
}

/* clear rows from r to the end of the scrolling area */
static void clear_rows(unsigned char r)
{
    for (; r < ROWS - statl; r++)
        gdc_fill(cell(r, 0), COLS, ' ');
}

static void newline(void)
{
    if (statl && row == ROWS - 1)   /* in the status line: no scrolling */
        return;
    if (++row < ROWS - statl - tux_rows)
        return;
    if (tux_rows) {                 /* first scroll: the logo makes room */
        tux_rows = 0;
        gdc_partitions();
        return;
    }
    row = ROWS - 1 - statl;
    gdc_fill(top, COLS, ' ');       /* the oldest row becomes the new bottom */
    top += COLS;
    if (top >= RING)
        top = 0;
    gdc_partitions();
    if (statl) {                    /* old status row is now the last text row */
        gdc_fill(cell(ROWS - 2, 0), COLS, ' ');
        sline_put();
    }
}

static void csi(byte_t c)
{
    unsigned char i;

    switch (c) {
    case 'H':
    case 'f':
        row = par[0] ? par[0] - 1 : 0;
        col = par[1] ? par[1] - 1 : 0;
        if (row >= ROWS)
            row = ROWS - 1;
        if (col >= COLS)
            col = COLS - 1;
        break;
    case 'K':
        gdc_fill(cell(row, col), COLS - col, ' ');
        if (statl && row == ROWS - 1)
            for (i = col; i < COLS; i++)
                sline[i] = ' ' | (ATTR_NORMAL << 8);
        break;
    case 'J':
        clear_rows(0);
        break;
    case 'm':
        for (i = 0; i <= np; i++)
            attr = par[i] == 1 || par[i] == 33 ? ATTR_HELL :
                   par[i] == 7 ? ATTR_INVERS : ATTR_NORMAL;
        break;
    case 'r':
        i = par[1] == ROWS - 1;
        if (i != statl) {
            statl = i;
            if (!statl)
                tux_rows = 0;
            sline_clear();          /* both ways: the line starts empty */
            gdc_partitions();
            if (statl && row == ROWS - 1)
                row--;
        }
        break;
    }
}

void conio_init(void)
{
    if (ready)
        return;
    ready = 1;
    top = 0;
    row = col = 0;
    gdc_fill(0, RING, ' ');
    tux_draw();
    tux_rows = TUX_ROWS;
    gdc_partitions();
    cursor();
    outb(0x01, KBD_STATUS_PORT);    /* initialise the 8741, country code follows */
}

void conio_putc(byte_t c)
{
    if (!ready)
        conio_init();
    if (esc == 1) {
        esc = c == '[' ? 2 : 0;
        np = par[0] = par[1] = 0;
        return;
    }
    if (esc == 2) {
        if (c >= '0' && c <= '9')
            par[np] = par[np] * 10 + c - '0';
        else if (c == ';')
            np = 1;
        else {
            esc = 0;
            csi(c);
            cursor();
        }
        return;
    }
    switch (c) {
    case 27:
        esc = 1;
        return;
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
    case '\f':                      /* form feed: clear the screen */
        tux_rows = 0;
        top = 0;
        row = col = 0;
        gdc_fill(0, RING, ' ');
        gdc_partitions();
        if (statl)
            sline_put();
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
        gdc_par(attr);
        if (statl && row == ROWS - 1)
            sline[col] = c | (attr << 8);
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
