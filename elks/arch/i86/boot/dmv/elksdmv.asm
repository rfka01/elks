;=====================================================================
; ELKSDMV.COM - start ELKS on the NCR Decision Mate V from DOS
;
; The DMV has no PC BIOS, so ELKS is built as a ROM kernel
; (CONFIG_ROMCODE) and this program plays the ROM: it reads the
; kernel image (arch/i86/boot/Image, here ELKS.IMG) to CONFIG_ROM_BASE
; and the root filesystem (image/romfs.bin, here ROMFS.BIN) to
; CONFIG_ROMFS_BASE, then enters setup like a PC BIOS enters an option
; ROM: far call to ROM_BASE:0003, which sets the INT 19h vector, then
; INT 19h. DOS is gone after that; there is no way back except a
; restart.
;
; Reads both files from the current directory of the current drive
; with plain DOS calls, so it works from floppy and hard disk, under
; MS-DOS and DR DOS. Needs 512K (K208).
;
; Build: nasm -f bin -o ELKSDMV.COM elksdmv.asm
;        nasm -DROM_BASE=0x6800 -DROMFS_BASE=0x3C00 ... to override
;
; 2026, rfka01 and Claude (Anthropic). GPL-2.0, like ELKS.
;=====================================================================
cpu 8086
%ifndef ROM_BASE
%define ROM_BASE   0x6E00
%endif
%ifndef ROMFS_BASE
%define ROMFS_BASE 0x3800
%endif

        org     100h

start:
        mov     dx,m_hello
        call    print
        ; our own code and stack must stay below the ROMFS
        mov     ax,cs
        add     ax,0x1000
        cmp     ax,ROMFS_BASE
        jb      .lowok
        mov     dx,m_high
        jmp     fail
.lowok:
        mov     bx,0xFFFF               ; how much memory does DOS have?
        mov     ah,0x4A
        int     0x21                    ; fails, BX = largest size
        mov     ax,cs
        add     ax,bx
        cmp     ax,0x8000               ; up to 512K must be RAM
        jae     .memok
        mov     dx,m_mem
        jmp     fail
.memok:
        mov     dx,f_rom
        mov     ax,ROM_BASE
        call    load
        mov     dx,f_romfs
        mov     ax,ROMFS_BASE
        call    load

        mov     dx,m_go
        call    print
        mov     ah,0x0D                 ; DOS buffers, though we never come back
        int     0x21

        cli
        xor     ax,ax                   ; bank 0 for DMA and the Z80, as for
        out     0xE0,al                 ; any restart (dmv_hardware, Abschnitt 6)
        mov     ax,ROMFS_BASE-0x100     ; temporary stack just below the ROMFS,
        mov     ss,ax                   ; out of the way of setup and kernel data
        mov     sp,0x1000
        call    ROM_BASE:0x0003         ; "option ROM" init: sets INT 19h
        int     0x19                    ; and start
        jmp     $

;---------------------------------------------------------------------
; load: DS:DX = file name, AX = target segment. Whole file, any size.
load:
        push    ax
        call    print_name
        mov     ax,0x3D00               ; open, read only
        int     0x21
        jc      .nofile
        mov     bx,ax
        pop     ax
.next:
        push    ds
        mov     ds,ax
        xor     dx,dx
        mov     cx,0x8000               ; 32K at a time
        push    ax
        mov     ah,0x3F
        int     0x21
        pop     cx                      ; segment
        pop     ds
        jc      .readerr
        or      ax,ax
        jz      .eof
        push    ax
        mov     dl,'.'
        mov     ah,2
        int     0x21
        pop     ax
        add     ax,15
        shr     ax,1
        shr     ax,1
        shr     ax,1
        shr     ax,1
        add     ax,cx
        jmp     .next
.eof:
        mov     ah,0x3E
        int     0x21
        mov     dx,m_crlf
        jmp     print
.nofile:
        pop     ax
        mov     dx,m_nofile
        jmp     fail
.readerr:
        mov     dx,m_read
        ; fall through

fail:
        call    print
        mov     ax,0x4C01
        int     0x21

print:
        mov     ah,9
        int     0x21
        ret

print_name:                             ; DS:DX = ASCIIZ, keep DX
        push    dx
        mov     si,dx
.l:     lodsb
        or      al,al
        jz      .e
        mov     dl,al
        mov     ah,2
        int     0x21
        jmp     .l
.e:     mov     dl,' '
        mov     ah,2
        int     0x21
        pop     dx
        ret

m_hello  db 'ELKSDMV - ELKS fuer die NCR DMV',13,10,'$'
m_high   db 'ELKSDMV liegt zu hoch im Speicher',13,10,'$'
m_mem    db 'ELKS braucht 512 KB (K208)',13,10,'$'
m_nofile db 13,10,'Datei nicht gefunden',13,10,'$'
m_read   db 13,10,'Lesefehler',13,10,'$'
m_go     db 'Start...',13,10,'$'
m_crlf   db 13,10,'$'
f_rom    db 'ELKS.IMG',0
f_romfs  db 'ROMFS.BIN',0
