# ELKS for the NCR Decision Mate V

Status (Nov 2026, tested in MAME only): boots to a shell on the 8088/V20
cards K235 (with 8259) and K230 (no interrupts), 512 KB (K208). The WD1001
(K012) is read through ata-cf (`/dev/hda`, read only), and `dmvhd` checks
a DMV disk: DMVGEOM1, master boot record, extended partitions, boot
sectors, FAT copies, cluster chains - or NCR's original MS-DOS 2.11
layout (one partition over 4 heads, or 2 x 5 MB on head pairs).

Tested: DR DOS 3.41 disks with 20 MB (type 04) and 41 MB (type 06), an
MS-DOS 3.3 disk with primary and extended partition, synthetic original
format disks (10 MB, 2 x 5 MB). `mount -t fat -o ro /dev/hda4 /mnt` works
on the DR partitions.

Purpose: an independent check of DMV hard disks - parameters, partition
tables, FAT file systems - written by people other than those who wrote
the DMV DOS ports (rfka01/dmv_os).

## Build

    cp dmv.config .config && make yesconfig && make
    nasm -f bin -o bootdmv.bin elks/arch/i86/boot/dmv/bootdmv.s
    python3 elks/arch/i86/boot/dmv/mkdmvimg.py bootdmv.bin \
        elks/arch/i86/boot/Image image/romfs.bin ELKSDMV.IMG

ELKSDMV.IMG is a 360 KB DMV floppy (40 x 2 x 9 x 512) that boots without DOS.
Alternatively `nasm -f bin -o ELKSDMV.COM elksdmv.asm` gives a loader that
starts ELKS from MS-DOS or DR DOS (files ELKS.IMG and ROMFS.BIN next to it).

## How it fits together

- There is no PC BIOS. ELKS is built as a ROM kernel (CONFIG_ROMCODE); the
  boot sector (`bootdmv.s`, from rfka01/gentleos) or `elksdmv.asm` puts the
  kernel image at CONFIG_ROM_BASE (7000:0) and the ROMFS at
  CONFIG_ROMFS_BASE (3B00:0), then enters setup like an option ROM.
- Linear 0F000h-0FFFFh stays free for the firmware (`system-dmv.c`):
  near heap ends below it, application memory starts at 64 KB.
- Console: `conio-dmv.c`, direct uPD7220 text output (ring of two
  partitions for scrolling), 8741 keyboard polled.
- Timer: 8253 channel 2 (500 kHz) on IR0 of the K235's 8259. On the K230
  the idle loop watches the counter and raises INT 08h itself
  (`timer-dmv.c`).
- Restart: back to the Z80 like REBOOT.COM, with bank 0 and disk unit 0
  (`reset-dmv.c`).

Hardware facts: rfka01/dmv_hardware, docs/.
