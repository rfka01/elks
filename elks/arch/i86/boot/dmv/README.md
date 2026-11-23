# ELKS for the NCR Decision Mate V

Status (Nov 2026, tested in MAME only): boots to a shell on the 8088/V20
cards K235 (with 8259) and K230 (no interrupts), 512 KB (K208). The WD1001
(K012) is read through ata-cf, units HD1-HD3 as `/dev/cfa`, `cfb`, `cfc`
(read only). `dmvhd` is a menu in 24 lines with a status line: a page of
devices (size, geometry, layout, findings), keys 1-3 for the structure of
a disk (DMVGEOM1, boot record, partitions and logical drives with CHS,
OEM and BPB - or NCR's original MS-DOS 2.11 layout: one partition over 4
heads, or 2 x 5 MB, DOS or CP/M halves), D for its file system check (FAT
copies, chains, directories, lost clusters; CP/M directory and blocks),
H for help, Q to quit. `dmvhd -n` and `dmvfs` (a link to dmvhd) give the
same as plain output for scripts.

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
        elks/arch/i86/boot/Image image/romfs.bin .

ELKSDMV_<version>.IMG is a 360 KB DMV floppy (40 x 2 x 9 x 512) that boots
without DOS. The version (`elks/include/arch/dmvver.h`) also shows on the boot
screen, in the line "NCR Decision Mate V v001, syscaps ..."; raise it with
every floppy given out.
Alternatively `nasm -f bin -o ELKSDMV.COM elksdmv.asm` gives a loader that
starts ELKS from MS-DOS or DR DOS (files ELKS.IMG and ROMFS.BIN next to it).

## How it fits together

- There is no PC BIOS. ELKS is built as a ROM kernel (CONFIG_ROMCODE); the
  boot sector (`bootdmv.s`, from rfka01/gentleos) or `elksdmv.asm` puts the
  kernel image at CONFIG_ROM_BASE (6E00:0) and the ROMFS at
  CONFIG_ROMFS_BASE (3800:0), then enters setup like an option ROM.
  Programs get the memory below the ROMFS (224 KB with 512 KB).
- Linear 0F000h-0FFFFh stays free for the firmware (`system-dmv.c`):
  near heap ends below it, application memory starts at 64 KB.
- Console: `conio-dmv.c`, direct uPD7220 text output (ring of two
  partitions for scrolling), 8741 keyboard polled. A small ANSI subset:
  cursor position, clear line/screen, highlight (yellow on colour,
  inverse on mono), and `ESC [ 1 ; 24 r` for 24 scrolling rows plus a
  status line, which the console keeps in a copy and writes back after
  every scroll, as NCR's ILF1 and the DMV DOS ports do.
- Boot logo: Tux in a bitmap partition above the text (the GDC runs in
  mixed mode), until the first scroll gives the two partitions back to the
  text ring. Colour board: white, yellow, black; mono board: the green
  plane. `tux-dmv.h` is made by `mktux-dmv.py` from the Linux kernel's
  80x80 logo. Tux is by Larry Ewing <lewing@isc.tamu.edu> and The GIMP.
- The kernel image may take 72 KB (6E00:0 up to the end of 512 KB,
  CONFIG_ROM_CHECKSUM_SIZE), the code itself at most 64 KB; the ROMFS
  216 KB (3800:0 up to the kernel). mkdmvimg.py checks the ROMFS. The
  bases are in dmv.config and as defaults in bootdmv.s and elksdmv.asm.
- The boot sector steps twice per track if a read fails, so the 40-track
  floppy also boots in an 80-track (QD) drive.
- Timer: 8253 channel 2 (500 kHz) on IR0 of the K235's 8259. On the K230
  the idle loop watches the counter and raises INT 08h itself
  (`timer-dmv.c`).
- Restart: back to the Z80 like REBOOT.COM, with bank 0 and disk unit 0
  (`reset-dmv.c`).

Hardware facts: rfka01/dmv_hardware, docs/.
