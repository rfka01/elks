#!/usr/bin/env python3
"""Make tux-dmv.h, the boot logo of conio-dmv.c, from the 80x80 Linux logo.

    python3 mktux-dmv.py logo_linux_clut224.ppm > tux-dmv.h

The input is drivers/video/logo/logo_linux_clut224.ppm from the Linux
kernel. Tux is by Larry Ewing <lewing@isc.tamu.edu>, drawn with The GIMP;
his terms: "Permission to use and/or modify this image is granted provided
you acknowledge me lewing@isc.tamu.edu and The GIMP if someone asks."

The DMV colour board has three planes, green, red, blue, one bit each, so
eight colours. Yellow and orange become yellow, light pixels white, the grey
of the glow around the body a white outline (Tux is black on a black
screen otherwise), the rest black. White, yellow and black have red equal
to green, so only two planes are stored: green (also red) and blue, each
80 lines of 5 words, bit 0 of a word is the leftmost pixel.
"""
import sys

def ppm(path):
    t = [x for line in open(path) for x in line.split('#')[0].split()]
    assert t[0] == 'P3'
    w, h = int(t[1]), int(t[2])
    v = list(map(int, t[4:]))
    return w, h, [tuple(v[i*3:i*3+3]) for i in range(w*h)]

def dmv(c):
    r, g, b = c
    l = (r*30 + g*59 + b*11) / 100
    if r > 150 and g > 100 and b < 110:
        return (1, 1, 0)                # r, g, b: yellow
    if l > 60:
        return (1, 1, 1)                # white, also the outline
    return (0, 0, 0)

w, h, px = ppm(sys.argv[1])
assert w == 80 and h == 80
assert all(dmv(c)[0] == dmv(c)[1] for c in px)     # red == green
print('/* Tux for conio-dmv.c, made by mktux-dmv.py - do not edit.')
print(' * Tux by Larry Ewing <lewing@isc.tamu.edu> and The GIMP. */')
print('#define TUX_W_WORDS %d' % (w // 16))
print('#define TUX_H %d' % h)
print('static const unsigned short tux_planes[2][%d] = {' % (h * w // 16))
for plane, name in ((1, 'green and red'), (2, 'blue')):
    print('    {   /* %s */' % name)
    for y in range(h):
        words = []
        for xw in range(w // 16):
            v = 0
            for bit in range(16):
                if dmv(px[y*w + xw*16 + bit])[plane]:
                    v |= 1 << bit
            words.append('0x%04x' % v)
        print('        ' + ', '.join(words) + ',')
    print('    },')
print('};')
