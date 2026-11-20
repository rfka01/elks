#!/usr/bin/env python3
"""Build the DMV-native ELKS floppy (360 KB, 40 x 2 x 9 x 512).

  sector 0     bootdmv.bin (16BIT signature)
  sectors 1..  arch/i86/boot/Image  -> ROM_BASE:0000
  then         image/romfs.bin      -> ROMFS_BASE:0000

The sector counts are patched in after the "SECC" and "INIT" markers.
Usage: mkdmvimg.py bootdmv.bin Image romfs.bin out.img
"""
import sys

def pad(b, n):
    if len(b) > n:
        sys.exit('mkdmvimg: %d bytes do not fit into %d' % (len(b), n))
    return b + bytes(n - len(b))

def patch(boot, marker, value):
    at = boot.find(marker)
    if at < 0:
        sys.exit('mkdmvimg: marker %r missing in boot sector' % marker)
    boot[at + 4:at + 6] = value.to_bytes(2, 'little')

boot, image, romfs, out = sys.argv[1:]
boot = bytearray(pad(open(boot, 'rb').read(), 512))
k = open(image, 'rb').read()
r = open(romfs, 'rb').read()
ks, rs = (len(k) + 511) // 512, (len(r) + 511) // 512
patch(boot, b'SECC', ks)
patch(boot, b'INIT', rs)
img = pad(bytes(boot) + pad(k, ks * 512) + pad(r, rs * 512), 368640)
open(out, 'wb').write(img)
print('mkdmvimg: kernel %d, romfs %d sectors, %d of 720 used' % (ks, rs, 1 + ks + rs))
