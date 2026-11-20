/*
 * ELKS hard disk backend for the NCR Decision Mate V
 *
 * The K012 (or 3282) carries a WD1001 (WD1010 type) MFM controller at
 * C0h-C7h. It is driven here behind the ata-cf.c block driver: this file
 * provides ata_reset/ata_init/ata_read/ata_write for it, so the disks show
 * up as /dev/cfa, /dev/cfa1 ... with the normal MBR partition handling.
 *
 * READ ONLY. The purpose is an independent look at DMV disks set up by the
 * DMV DOS ports: parameters, partitions, file systems. ata_write refuses.
 *
 * Hardware facts (rfka01/dmv_hardware, docs/DMV-Hardware-Boot-und-Bildschirm.md
 * section 6, docs/DMV-Module-und-Anschluesse.md section 5; rfka01/dmv_os
 * docs/DMV-Datentraeger.md section 5):
 *   C0h data (8 bit), C2h sector count, C3h sector number, C4h/C5h cylinder,
 *   C6h SDH, C7h status/command. Status 80h BUSY, 40h READY, 08h DRQ,
 *   01h ERROR. SDH: 101uuhhh, 512-byte sectors, unit in bits 4-3, head in
 *   bits 2-0 (at most 8 heads). Sector IDs count from 0.
 *   No controller: the bus reads FFh, BUSY included - so find it first by a
 *   write/read test (55h to C4h, AAh to C3h) and bound every wait.
 *   The step rate of RESTORE (low nibble) is kept for the implied seeks of
 *   READ and WRITE; RESTORE with 0 = buffered seek.
 *   Geometry comes from the DMVGEOM1 block on cylinder 0, head 0, sector
 *   ID 1 (copy on ID 2); the DOS ports treat a unit without a valid block
 *   as absent. Here such a unit is still read, as 4 heads/17 sectors, so
 *   dmvhd can look at disks in NCR's original MS-DOS 2.11 format.
 *   Units 0-2 are HD1-HD3; ELKS's ata-cf handles two.
 *   Before handing the machine back to the Z80 unit 0 must be selected
 *   (reset-dmv.c does), so every access here ends with unit 0 selected.
 *
 * 2026, rfka01 and Claude (Anthropic). GPL-2.0, like ELKS.
 */

#include <linuxmt/config.h>
#include <linuxmt/kernel.h>
#include <linuxmt/errno.h>
#include <linuxmt/genhd.h>
#include <linuxmt/ata.h>
#include <linuxmt/heap.h>
#include <linuxmt/string.h>
#include <arch/io.h>
#include <arch/ports.h>

#define HD_DATA     (DMV_HD_BASE + 0)
#define HD_COUNT    (DMV_HD_BASE + 2)
#define HD_SECTOR   (DMV_HD_BASE + 3)
#define HD_CYL_LO   (DMV_HD_BASE + 4)
#define HD_CYL_HI   (DMV_HD_BASE + 5)
#define HD_SDH      (DMV_HD_BASE + 6)
#define HD_STATUS   (DMV_HD_BASE + 7)
#define HD_CMD      (DMV_HD_BASE + 7)

#define ST_BUSY     0x80
#define ST_READY    0x40
#define ST_DRQ      0x08
#define ST_ERROR    0x01

#define CMD_RESTORE 0x10            /* step rate 0: buffered seek */
#define CMD_READ    0x20

#define SDH_512     0xA0
#define WAIT_LONG   60000U          /* loop passes, several seconds on an 8088 */
#define WAIT_READY  20000U

#define DMV_SECTOR_SIZE 512

static char present;                /* controller found */
static struct {
    unsigned int cyls;
    unsigned char heads, spt;
} geo[2];

static unsigned char sdh(unsigned int unit, unsigned int head)
{
    return SDH_512 | (unit << 3) | (head & 7);
}

/* wait until BUSY is gone; then the status, or -1 on timeout */
static int wait_notbusy(unsigned int limit)
{
    unsigned char st;

    do {
        st = inb(HD_STATUS);
        if (!(st & ST_BUSY))
            return st;
    } while (--limit);
    return -1;
}

static int wait_drq(void)
{
    unsigned int limit = WAIT_LONG;
    unsigned char st;

    do {
        st = inb(HD_STATUS);
        if (!(st & ST_BUSY)) {
            if (st & ST_ERROR)
                return -EIO;
            if (st & ST_DRQ)
                return 0;
        }
    } while (--limit);
    return -ETIMEDOUT;
}

static int unit_ready(unsigned int unit)
{
    unsigned int limit = WAIT_READY;

    outb(sdh(unit, 0), HD_SDH);
    do {
        if ((inb(HD_STATUS) & (ST_BUSY | ST_READY)) == ST_READY)
            return 1;
    } while (--limit);
    return 0;
}

static void unit0(void)
{
    outb(sdh(0, 0), HD_SDH);
}

/* read one sector by CHS (sector ID from 0) into a far buffer */
static int read_chs(unsigned int unit, unsigned int cyl, unsigned int head,
    unsigned int sect, unsigned char __far *buf)
{
    int i, err;

    if (wait_notbusy(WAIT_LONG) < 0)
        return -ETIMEDOUT;
    outb(sdh(unit, head), HD_SDH);
    outb(1, HD_COUNT);
    outb(sect, HD_SECTOR);
    outb(cyl, HD_CYL_LO);
    outb(cyl >> 8, HD_CYL_HI);
    outb(CMD_READ, HD_CMD);
    err = wait_drq();
    if (!err) {
        for (i = 0; i < DMV_SECTOR_SIZE; i++)
            buf[i] = inb(HD_DATA);
    }
    unit0();
    return err;
}

int ATPROC ata_reset(void)
{
    int i;

    present = 1;
    for (i = 0; i < 3; i++) {
        outb(0x55, HD_CYL_LO);
        outb(0xAA, HD_SECTOR);
        if (inb(HD_CYL_LO) != 0x55 || inb(HD_SECTOR) != 0xAA)
            present = 0;
    }
    if (!present)
        printk("cf: no WD1001 at %x\n", DMV_HD_BASE);
    return present ? 0 : -ENODEV;
}

/* DMVGEOM1: 'DMVGEOM1', cyls (word), heads, spt, bytes/sector, precomp,
   landing zone, step rate, interleave, unit, -, checksum of bytes 0..21 */
static int geom_ok(unsigned char *b)
{
    unsigned int sum = 0;
    int i;

    if (memcmp(b, "DMVGEOM1", 8))
        return 0;
    for (i = 0; i < 22; i++)
        sum += b[i];
    return sum == (b[22] | (b[23] << 8));
}

int ATPROC ata_init(int drive, struct drive_infot *drivep)
{
    unsigned char *b;
    int ok = 0, id;

    drivep->cylinders = 0;
    geo[drive].cyls = 0;
    if (!present || drive > 1)
        return 0;
    if (!unit_ready(drive)) {
        unit0();
        return 0;
    }
    outb(CMD_RESTORE, HD_CMD);          /* track 0, buffered seek from now on */
    if (wait_notbusy(WAIT_LONG) < 0) {
        unit0();
        return 0;
    }

    b = heap_alloc(DMV_SECTOR_SIZE, HEAP_TAG_DRVR | HEAP_TAG_CLEAR);
    if (!b)
        return 0;
    for (id = 1; id <= 2 && !ok; id++) {
        if (read_chs(drive, 0, 0, id, (unsigned char __far *)b) == 0)
            ok = geom_ok(b);
    }
    if (ok) {
        geo[drive].cyls = b[8] | (b[9] << 8);
        geo[drive].heads = b[10];
        geo[drive].spt = b[11];
        if (!geo[drive].cyls || !geo[drive].heads || geo[drive].heads > 8 ||
            !geo[drive].spt || (b[12] | (b[13] << 8)) != DMV_SECTOR_SIZE) {
            printk("cf%c: DMVGEOM1 not usable\n", 'a' + drive);
            geo[drive].cyls = 0;
        } else {
            drivep->cylinders = geo[drive].cyls;
            drivep->heads = geo[drive].heads;
            drivep->sectors = geo[drive].spt;
            drivep->sector_size = DMV_SECTOR_SIZE;
            drivep->fdtype = HARDDISK;
            show_drive_info(drivep, "cf", drive, 1, " ");
            printk("WD1001 unit %d, DMVGEOM1 precomp %u, step %u, interleave %u\n",
                drive, b[14] | (b[15] << 8), b[18], b[19]);
        }
    } else if (read_chs(drive, 0, 0, 0, (unsigned char __far *)b) == 0) {
        /* no DMVGEOM1: maybe a disk in NCR's original MS-DOS 2.11 format
           (4 heads, 17 sectors, no MBR). The DOS ports treat such a unit
           as absent; here it stays readable for dmvhd, with the physical
           layout of those disks and the largest cylinder count. */
        geo[drive].cyls = 1024;
        geo[drive].heads = 4;
        geo[drive].spt = 17;
        drivep->cylinders = 1024;
        drivep->heads = 4;
        drivep->sectors = 17;
        drivep->sector_size = DMV_SECTOR_SIZE;
        drivep->fdtype = HARDDISK;
        printk("cf%c: WD1001 unit %d without DMVGEOM1, read as 4 heads/17 sectors\n",
            'a' + drive, drive);
    } else
        printk("cf%c: unit %d ready, but no valid DMVGEOM1 block\n", 'a' + drive, drive);
    heap_free(b);
    return geo[drive].cyls;
}

int ATPROC ata_read(unsigned int drive, sector_t sector, char *buf, ramdesc_t seg)
{
    unsigned int track, head, sect, cyl;

    if (drive > 1 || !geo[drive].cyls)
        return -ENODEV;
    sect = (unsigned int)(sector % geo[drive].spt);
    track = (unsigned int)(sector / geo[drive].spt);
    head = track % geo[drive].heads;
    cyl = track / geo[drive].heads;
    if (cyl >= geo[drive].cyls)
        return -EIO;
    return read_chs(drive, cyl, head, sect, _MK_FP(seg, buf));
}

int ATPROC ata_write(unsigned int drive, sector_t sector, char *buf, ramdesc_t seg)
{
    return -EROFS;                      /* diagnosis only */
}
