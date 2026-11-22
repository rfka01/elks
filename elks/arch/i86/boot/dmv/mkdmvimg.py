#!/usr/bin/env python3
"""Build the DMV-native ELKS floppy (360 KB, 40 x 2 x 9 x 512).

  sector 0     bootdmv.bin (16BIT signature)
  sectors 1..  arch/i86/boot/Image  -> ROM_BASE:0000
  then         image/romfs.bin      -> ROMFS_BASE:0000

The sector counts are patched in after the "SECC" and "INIT" markers.
Usage: mkdmvimg.py bootdmv.bin Image romfs.bin out.img|directory
(a directory gets ELKSDMV_<version>.IMG, version from include/arch/dmvver.h)
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

boot, image, romfs, out = sys.argv[1:5]

# A directory as output: the image gets the version from dmvver.h,
# ELKSDMV_<version>.IMG, the same version the boot screen shows.
import os, re
here = os.path.dirname(os.path.abspath(__file__))
ver = re.search(r'#define DMV_VERSION "([^"]+)"',
                open(os.path.join(here, '../../../../include/arch/dmvver.h')).read()).group(1)
if os.path.isdir(out):
    out = os.path.join(out, 'ELKSDMV_%s.IMG' % ver)

# The ROMFS lies below the kernel image; check it does not run into it
# (CONFIG_ROMFS_BASE and CONFIG_ROM_BASE from .config, if at hand).
try:
    cfg = dict(l.strip().split('=', 1) for l in open('.config') if l.startswith('CONFIG_ROM'))
    rb, fb = int(cfg['CONFIG_ROM_BASE'], 16), int(cfg['CONFIG_ROMFS_BASE'], 16)
    import os
    need = (os.path.getsize(romfs) + 15) // 16
    if fb + need > rb:
        sys.exit('mkdmvimg: ROMFS (%d paragraphs from %04X) runs into the kernel at %04X; '
                 'lower CONFIG_ROMFS_BASE' % (need, fb, rb))
except (OSError, KeyError):
    pass
boot = bytearray(pad(open(boot, 'rb').read(), 512))
k = open(image, 'rb').read()
r = open(romfs, 'rb').read()
ks, rs = (len(k) + 511) // 512, (len(r) + 511) // 512
patch(boot, b'SECC', ks)
patch(boot, b'INIT', rs)
img = pad(bytes(boot) + pad(k, ks * 512) + pad(r, rs * 512), 368640)
open(out, 'wb').write(img)
print('mkdmvimg: %s %s: kernel %d, romfs %d sectors, %d of 720 used'
      % (out, ver, ks, rs, 1 + ks + rs))
