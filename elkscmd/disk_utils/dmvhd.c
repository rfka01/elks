/*
 * dmvhd - Festplattendiagnose fuer die NCR Decision Mate V (nur lesend)
 *
 * Eine unabhaengige Pruefung von DMV-Platten, die mit den DOS-Ports aus
 * rfka01/dmv_os eingerichtet wurden (DMVHDFMT, FDISK, FORMAT/SYS):
 *
 *   1. DMVGEOM1-Block (Sektor-ID 1, Kopie auf ID 2): Geometrie, Pruefsumme
 *   2. Haupt-Bootsatz: Kennung 16BIT, 55AA, die vier Eintraege mit CHS
 *      und LBA, beide gegen die Geometrie gerechnet, Ueberlappungen
 *   3. je FAT-Partition (Typ 01, 04, 06): Bootsektor (OEM, BPB, DMV-Bytes
 *      24h/25h), BPB gegen den Partitionseintrag
 *   4. je FAT-Partition: beide FAT-Kopien sektorweise verglichen, Cluster
 *      gezaehlt (frei/belegt/defekt), Zeiger ausserhalb, Querverweise,
 *      Verzeichnisbaum durchlaufen, verlorene Ketten
 *
 * Gelesen wird die ganze Platte (/dev/hda); die Partitionen werden aus
 * dem eigenen Lesen des Haupt-Bootsatzes gefunden, nicht ueber den Kern.
 * Die FAT wird nicht im Ganzen gehalten (16-Bit-Datensegment), sondern
 * sektorweise gelesen; fuer die Kettenpruefung reichen zwei Bitfelder.
 *
 * Aufruf: dmvhd [-v] [-n] [Geraet]   Vorgabe /dev/hda
 *   -v jede Datei zeigen, -n ohne Halt nach jeder Bildschirmseite
 *
 * Grundlagen: rfka01/dmv_os docs/DMV-Datentraeger.md (DMVGEOM1, OEM-
 * Kennungen), docs/DMV-Schnittstellen.md (Abschnitt 4, Bytes 24h/25h).
 *
 * 2026, rfka01 und Claude (Anthropic). GPL-2.0 wie ELKS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

#define SECT    512

typedef unsigned long ulong;
typedef unsigned short ushort;
typedef unsigned char uchar;

static int fd, verbose, fehler, warnungen;

/* Seitenweise Ausgabe: der Bildschirm hat 25 Zeilen und rollt nicht
   zurueck. Nach 23 Zeilen auf die Eingabetaste warten, wenn die Ausgabe
   auf den Bildschirm geht (-n schaltet es ab). */
static int zeilen, seitenweise = 1;

static void seite(const char *s)
{
    char c[2];

    for (; *s; s++)
        if (*s == '\n' && seitenweise && ++zeilen >= 23) {
            fflush(stdout);
            fputs("-- weiter mit Eingabetaste --", stderr);
            read(0, c, 1);
            while (c[0] != '\n' && c[0] != '\r' && read(0, c, 1) == 1)
                ;
            fputs("\r                             \r", stderr);
            zeilen = 0;
        }
}

static int out(const char *fmt, ...)
{
    char z[200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(z, sizeof(z), fmt, ap);
    va_end(ap);
    fputs(z, stdout);
    seite(z);
    return n;
}
#define printf out
static uchar buf[SECT];
static unsigned int cyls, heads, spt;   /* aus DMVGEOM1 */
static ulong disksects;

static ushort w(uchar *p) { return p[0] | (p[1] << 8); }
static ulong dw(uchar *p) { return w(p) | ((ulong)w(p + 2) << 16); }

static void err(const char *fmt, ...)
{
    char z[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z, sizeof(z), fmt, ap);
    va_end(ap);
    printf("  FEHLER: %s", z);
    fehler++;
}

static void warn(const char *fmt, ...)
{
    char z[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z, sizeof(z), fmt, ap);
    va_end(ap);
    printf("  Warnung: %s", z);
    warnungen++;
}

/* NCR-Originalformat 2 x 5 MB: eine Haelfte liegt auf einem Kopfpaar.
   map_hp = Koepfe der Haelfte (0: keine Umrechnung), map_fh = erster Kopf */
static unsigned int map_hp, map_fh;
static int original;                    /* NCR-Originalformat: 16BIT im Bootsektor ist richtig */

static int rd(ulong lba, uchar *b)
{
    if (map_hp) {
        ulong spur = lba / 17;
        lba = ((spur / map_hp) * 4 + map_fh + spur % map_hp) * 17 + lba % 17;
    }
    if (lseek(fd, (long)lba * SECT, SEEK_SET) < 0 || read(fd, b, SECT) != SECT) {
        err("Sektor %lu nicht lesbar\n", lba);
        return -1;
    }
    return 0;
}

/* LBA aus CHS (Sektor 1-basiert, wie im Partitionseintrag) */
static ulong chs2lba(unsigned int c, unsigned int h, unsigned int s)
{
    return ((ulong)c * heads + h) * spt + (s - 1);
}

/* ---------------------------------------------------------------- 1 -- */
static int geometrie(void)
{
    int id, ok = 0;
    unsigned int sum, i;

    printf("DMVGEOM1-Block\n");
    for (id = 1; id <= 2; id++) {
        if (rd(id, buf) < 0)
            continue;
        if (memcmp(buf, "DMVGEOM1", 8)) {
            warn("Sektor-ID %d: keine Kennung DMVGEOM1\n", id);
            continue;
        }
        for (sum = 0, i = 0; i < 22; i++)
            sum += buf[i];
        if (sum != w(buf + 22)) {
            err("Sektor-ID %d: Pruefsumme %04x, gespeichert %04x\n", id, sum, w(buf + 22));
            continue;
        }
        if (!ok) {
            cyls = w(buf + 8);
            heads = buf[10];
            spt = buf[11];
            printf("  Sektor-ID %d: %u Zylinder, %u Koepfe, %u Sektoren, %u Byte\n",
                id, cyls, heads, spt, w(buf + 12));
            printf("  Precomp %u, Landezone %u, Schrittweite %u, Interleave %u, Laufwerk %u\n",
                w(buf + 14), w(buf + 16), buf[18], buf[19], buf[20]);
            if (w(buf + 12) != SECT)
                err("Sektorgroesse %u statt 512\n", w(buf + 12));
            if (heads == 0 || heads > 8)
                err("%u Koepfe: die DMV kann 1 bis 8\n", heads);
            if (cyls == 0 || cyls > 1024)
                err("%u Zylinder: erlaubt 1 bis 1024\n", cyls);
            ok = 1;
        } else if (w(buf + 8) != cyls || buf[10] != heads || buf[11] != spt)
            err("Kopie auf ID 2 weicht von ID 1 ab\n");
        else
            printf("  Kopie auf Sektor-ID 2 gleich\n");
    }
    if (!ok) {
        warn("kein gueltiger DMVGEOM1-Block - fuer die DOS-Ports gilt die Einheit als\n"
             "  nicht vorhanden. Weiter mit 4 Koepfen, 17 Sektoren (NCR-Originalformat?)\n");
        cyls = 0;
        heads = 4;
        spt = 17;
        disksects = 0xFFFFFFFFL;
        return 0;
    }
    disksects = (ulong)cyls * heads * spt;
    printf("  %lu Sektoren, %lu KB\n", disksects, disksects / 2);
    return 0;
}

/* ---------------------------------------------------------------- 2 -- */
struct part {
    uchar boot, typ;
    ulong start, size;
    ulong hidden;           /* erwartete versteckte Sektoren im BPB */
};
static struct part pt[4];

static const char *typname(int t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 <32M";
    case 0x05: return "erweitert";
    case 0x06: return "FAT16 >32M";
    case 0x00: return "leer";
    }
    return "?";
}

static int mbr(void)
{
    int i, j;
    uchar *e;

    printf("\nHaupt-Bootsatz (Sektor 0)\n");
    if (rd(0, buf) < 0)
        return -1;
    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        printf("  keine Kennung 55AA - kein Partitionsschema\n");
        if (memcmp(buf + 3, "16BIT", 5))
            printf("  Sektor 0 ohne 16BIT: von dieser Platte startet die DMV nicht\n");
        return 1;                       /* Originalformat pruefen */
    }
    if (memcmp(buf + 3, "16BIT  ", 7))
        err("Kennung ab Byte 3 ist nicht \"16BIT  \" - das Z80-ROM startet davon nicht\n");
    else
        printf("  Kennung 16BIT vorhanden\n");
    printf("  Nr Boot Typ          Start-CHS     Ende-CHS          LBA      Sektoren\n");
    for (i = 0; i < 4; i++) {
        unsigned int sc, sh, ss, ec, eh, es;
        e = buf + 0x1BE + 16 * i;
        pt[i].boot = e[0];
        pt[i].typ = e[4];
        pt[i].start = dw(e + 8);
        pt[i].size = dw(e + 12);
        sh = e[1]; ss = e[2] & 63; sc = e[3] | ((e[2] & 0xC0) << 2);
        eh = e[5]; es = e[6] & 63; ec = e[7] | ((e[6] & 0xC0) << 2);
        if (!pt[i].typ) {
            printf("  %d   -   leer\n", i + 1);
            continue;
        }
        printf("  %d   %c   %02x %-10s %4u/%u/%-2u    %4u/%u/%-2u    %8lu %9lu\n",
            i + 1, e[0] == 0x80 ? '*' : ' ', pt[i].typ, typname(pt[i].typ),
            sc, sh, ss, ec, eh, es, pt[i].start, pt[i].size);
        if (e[0] != 0 && e[0] != 0x80)
            err("Eintrag %d: Bootkennung %02x (erlaubt 00/80)\n", i + 1, e[0]);
        if (pt[i].typ != 0x01 && pt[i].typ != 0x04 && pt[i].typ != 0x05 && pt[i].typ != 0x06)
            warn("Eintrag %d: Typ %02x kennt der DMV-MBR nicht (01/04/05/06)\n", i + 1, pt[i].typ);
        if (ss == 0 || es == 0)
            err("Eintrag %d: Sektor 0 in CHS (Partitionseintraege zaehlen ab 1)\n", i + 1);
        else {
            if (chs2lba(sc, sh, ss) != pt[i].start)
                err("Eintrag %d: Start-CHS ergibt LBA %lu, eingetragen %lu\n",
                    i + 1, chs2lba(sc, sh, ss), pt[i].start);
            if (chs2lba(ec, eh, es) != pt[i].start + pt[i].size - 1)
                err("Eintrag %d: Ende-CHS ergibt LBA %lu, erwartet %lu\n",
                    i + 1, chs2lba(ec, eh, es), pt[i].start + pt[i].size - 1);
        }
        if (pt[i].start < 3)
            err("Eintrag %d beginnt vor Sektor 3 und ueberschreibt DMVGEOM1\n", i + 1);
        if (pt[i].start + pt[i].size > disksects)
            err("Eintrag %d reicht ueber das Plattenende (%lu Sektoren)\n", i + 1, disksects);
        for (j = 0; j < i; j++)
            if (pt[j].typ && pt[i].start < pt[j].start + pt[j].size &&
                pt[j].start < pt[i].start + pt[i].size)
                err("Eintrag %d ueberlappt Eintrag %d\n", i + 1, j + 1);
        if (pt[i].typ == 0x04 && pt[i].size > 65535L)
            err("Eintrag %d: Typ 04 mit mehr als 65535 Sektoren\n", i + 1);
        if (pt[i].typ == 0x06 && pt[i].size <= 65535L)
            warn("Eintrag %d: Typ 06, waere als 04 klein genug\n", i + 1);
        if (pt[i].typ == 0x06)
            printf("     (ueber 32 MB: MS-DOS 3.3 sieht diese Partition nicht)\n");
    }
    for (i = j = 0; i < 4; i++)
        if (pt[i].boot == 0x80)
            j++;
    if (j != 1)
        warn("%d Eintraege als startbar markiert (erwartet 1)\n", j);
    return 0;
}

/* ------------------------------------------------------------- 3, 4 -- */
static ulong p0;                        /* Partitionsbeginn */
static unsigned int bps, spc, nfat, rootents, fatsz, fat16;
static ulong reserved, totsec, fatlba, rootlba, datalba, ncl;

static uchar *seen, *refd;              /* Bitfelder je Cluster */
static uchar fatbuf[SECT];
static ulong fatbufsec = 0xFFFFFFFFL;

#define BIT(a, n)   ((a)[(n) >> 3] & (1 << ((n) & 7)))
#define SETB(a, n)  ((a)[(n) >> 3] |= (1 << ((n) & 7)))

/* FAT-Eintrag n (erste Kopie), sektorweise gelesen */
static ulong fatget(ulong n)
{
    ulong off = fat16 ? n * 2 : n + n / 2;
    ulong sec = fatlba + off / SECT;
    unsigned int o = (unsigned int)(off % SECT), v;

    if (sec != fatbufsec) {
        if (rd(sec, fatbuf) < 0)
            return 0;
        fatbufsec = sec;
    }
    if (fat16)
        return w(fatbuf + o);
    /* FAT12: der Eintrag kann ueber die Sektorgrenze gehen */
    v = fatbuf[o];
    if (o == SECT - 1) {
        if (rd(sec + 1, fatbuf) < 0)
            return 0;
        fatbufsec = sec + 1;
        v |= fatbuf[0] << 8;
    } else
        v |= fatbuf[o + 1] << 8;
    return (n & 1) ? v >> 4 : v & 0xFFF;
}

static int is_eoc(ulong v) { return fat16 ? v >= 0xFFF8L : v >= 0xFF8; }
static int is_bad(ulong v) { return fat16 ? v == 0xFFF7L : v == 0xFF7; }

static ulong nfiles, ndirs, querverweise;

/* Kette ab c durchgehen, als gesehen markieren; Laenge in Clustern */
static ulong kette(ulong c, const char *name)
{
    ulong len = 0, v;

    while (c >= 2 && c < ncl + 2) {
        if (BIT(seen, c)) {
            err("%s: Cluster %lu doppelt belegt (Querverweis)\n", name, c);
            querverweise++;
            break;
        }
        SETB(seen, c);
        len++;
        v = fatget(c);
        if (is_eoc(v))
            break;
        if (v < 2 || v >= ncl + 2 || is_bad(v)) {
            err("%s: Kette bricht bei Cluster %lu ab (Eintrag %lx)\n", name, c, v);
            break;
        }
        c = v;
    }
    return len;
}

static void verzeichnis(ulong first, int root, const char *pfad, int tiefe);

static void eintraege(uchar *b, int n, const char *pfad, int tiefe, int *ende)
{
    int i;
    char name[13], voll[80];
    ulong c, len, groesse, need;

    for (i = 0; i < n && !*ende; i++, b += 32) {
        if (b[0] == 0) {
            *ende = 1;
            return;
        }
        if (b[0] == 0xE5 || b[0] == '.' || (b[11] & 0x08))
            continue;               /* geloescht, . und .., Volumenname */
        memcpy(name, b, 8);
        name[8] = 0;
        while (name[0] && name[strlen(name) - 1] == ' ')
            name[strlen(name) - 1] = 0;
        if (b[8] != ' ') {
            int k = strlen(name);
            name[k++] = '.';
            memcpy(name + k, b + 8, 3);
            name[k + 3] = 0;
            while (name[strlen(name) - 1] == ' ')
                name[strlen(name) - 1] = 0;
        }
        snprintf(voll, sizeof(voll), "%s/%s", pfad, name);
        c = w(b + 26);
        groesse = dw(b + 28);
        if (b[11] & 0x10) {
            ndirs++;
            if (verbose)
                printf("    %s/\n", voll);
            if (c < 2 || c >= ncl + 2)
                err("%s: Verzeichnis ohne gueltigen Startcluster %lu\n", voll, c);
            else if (BIT(seen, c))
                err("%s: Startcluster %lu schon belegt\n", voll, c);
            else if (tiefe > 16)
                err("%s: Verzeichnisse zu tief verschachtelt\n", voll);
            else
                verzeichnis(c, 0, voll, tiefe + 1);
        } else {
            nfiles++;
            if (verbose)
                printf("    %s %lu\n", voll, groesse);
            if (groesse == 0) {
                if (c)
                    warn("%s: Laenge 0, aber Startcluster %lu\n", voll, c);
                continue;
            }
            if (c < 2 || c >= ncl + 2) {
                err("%s: ungueltiger Startcluster %lu\n", voll, c);
                continue;
            }
            len = kette(c, voll);
            need = (groesse + (ulong)spc * SECT - 1) / ((ulong)spc * SECT);
            if (len != need)
                err("%s: %lu Byte brauchen %lu Cluster, die Kette hat %lu\n",
                    voll, groesse, need, len);
        }
    }
}

static void verzeichnis(ulong first, int root, const char *pfad, int tiefe)
{
    uchar b[SECT];
    int ende = 0;
    unsigned int s;
    ulong c, v;

    if (root) {
        for (s = 0; s < (rootents * 32 + SECT - 1) / SECT && !ende; s++) {
            if (rd(rootlba + s, b) < 0)
                return;
            eintraege(b, SECT / 32, pfad, tiefe, &ende);
        }
        return;
    }
    for (c = first; c >= 2 && c < ncl + 2 && !ende; ) {
        if (BIT(seen, c)) {
            err("%s: Verzeichniskette laeuft in belegten Cluster %lu\n", pfad, c);
            return;
        }
        SETB(seen, c);
        for (s = 0; s < spc && !ende; s++) {
            if (rd(datalba + (c - 2) * spc + s, b) < 0)
                return;
            eintraege(b, SECT / 32, pfad, tiefe, &ende);
        }
        v = fatget(c);
        if (is_eoc(v))
            break;
        if (v < 2 || v >= ncl + 2) {
            err("%s: Verzeichniskette bricht bei %lu ab\n", pfad, c);
            break;
        }
        c = v;
    }
}

static void fatpruefung(int nr)
{
    uchar a[SECT], b[SECT];
    ulong s, n, v, frei = 0, belegt = 0, defekt = 0, aussen = 0, verloren = 0,
          ketten = 0, diff = 0;
    unsigned int k;

    printf("  FAT-Kopien vergleichen ...\n");
    for (k = 1; k < nfat; k++)
        for (s = 0; s < fatsz; s++) {
            if (rd(fatlba + s, a) < 0 || rd(fatlba + (ulong)k * fatsz + s, b) < 0)
                break;
            if (memcmp(a, b, SECT)) {
                if (!diff)
                    err("FAT %u weicht ab, zuerst in FAT-Sektor %lu\n", k + 1, s);
                diff++;
            }
        }
    if (nfat > 1 && !diff)
        printf("  %u FAT-Kopien gleich\n", nfat);
    else if (diff)
        printf("  %lu FAT-Sektoren verschieden\n", diff);

    seen = calloc(1, (unsigned int)((ncl + 2 + 7) / 8));
    refd = calloc(1, (unsigned int)((ncl + 2 + 7) / 8));
    if (!seen || !refd) {
        err("zu wenig Speicher fuer %lu Cluster\n", ncl);
        free(seen);
        free(refd);
        return;
    }
    for (n = 2; n < ncl + 2; n++) {
        v = fatget(n);
        if (v == 0)
            frei++;
        else if (is_bad(v))
            defekt++;
        else {
            belegt++;
            if (!is_eoc(v)) {
                if (v < 2 || v >= ncl + 2) {
                    if (aussen < 5)
                        err("Cluster %lu zeigt nach %lx, ausserhalb\n", n, v);
                    aussen++;
                } else {
                    if (BIT(refd, v)) {
                        if (querverweise < 5)
                            err("Cluster %lu wird von zwei Clustern angezeigt\n", v);
                        querverweise++;
                    }
                    SETB(refd, v);
                }
            }
        }
    }
    printf("  %lu Cluster zu %u Sektoren: %lu frei, %lu belegt, %lu defekt\n",
        ncl, spc, frei, belegt, defekt);
    printf("  frei %lu KB\n", frei * spc / 2);

    printf("  Verzeichnisbaum durchlaufen ...\n");
    nfiles = ndirs = 0;
    verzeichnis(0, 1, "", 0);
    printf("  %lu Dateien, %lu Verzeichnisse\n", nfiles, ndirs);

    for (n = 2; n < ncl + 2; n++) {
        v = fatget(n);
        if (v == 0 || is_bad(v) || BIT(seen, n))
            continue;
        verloren++;
        if (!BIT(refd, n))
            ketten++;
    }
    if (verloren)
        err("%lu verlorene Cluster in %lu Ketten (belegt, aber von keiner Datei)\n",
            verloren, ketten);
    else
        printf("  keine verlorenen Cluster\n");
    free(seen);
    free(refd);
    (void)nr;
}

static void partition(struct part *pp, const char *was)
{
    uchar *b = buf;
    ulong dsec;

    p0 = pp->start;
    printf("\n%s (Typ %02x, ab Sektor %lu)\n", was, pp->typ, p0);
    if (rd(p0, b) < 0)
        return;
    printf("  OEM \"%.8s\"", b + 3);
    if (original)
        printf(" - Originalformat: %s\n", b[3] == '1' ? "bootfaehig" : "nicht bootfaehig");
    else if (!memcmp(b + 3, "IBM  3.3", 8))
        printf(" - wie von den DMV-Ports verlangt\n");
    else if (!memcmp(b + 3, "16BIT", 5)) {
        printf("\n");
        err("Bootsektor einer Plattenpartition darf nicht 16BIT tragen (MS-DOS 3.3 verwirft dann den BPB)\n");
    } else {
        printf("\n");
        warn("OEM nicht \"IBM  3.3\" - MS-DOS 3.3 nimmt den BPB dann nicht\n");
    }
    if (!original && (b[510] != 0x55 || b[511] != 0xAA))
        warn("Bootsektor ohne 55AA\n");
    bps = w(b + 11);
    spc = b[13];
    reserved = w(b + 14);
    nfat = b[16];
    rootents = w(b + 17);
    totsec = w(b + 19) ? w(b + 19) : dw(b + 32);
    fatsz = w(b + 22);
    printf("  BPB: %u Byte/Sektor, %u Sektoren/Cluster, %lu reserviert, %u FATs zu %u\n",
        bps, spc, reserved, nfat, fatsz);
    /* DOS 2.x: versteckte Sektoren nur 16 Bit, dahinter beginnt der Code */
    printf("       %u Wurzeleintraege, %lu Sektoren, Medium %02x, %u Sektoren/Spur, %u Koepfe, %lu versteckt\n",
        rootents, totsec, b[21], w(b + 24), w(b + 26), original ? (ulong)w(b + 28) : dw(b + 28));
    if (!original)
        printf("  DMV: Byte 24h (Einheit, SDH-Bits) %02x, Byte 25h (Laufwerk) %02x\n", b[0x24], b[0x25]);
    if (bps != SECT || !spc || (spc & (spc - 1)) || !nfat || !fatsz || !rootents) {
        err("BPB unbrauchbar\n");
        return;
    }
    if (totsec != pp->size)
        err("BPB nennt %lu Sektoren, der Partitionseintrag %lu\n", totsec, pp->size);
    if (!original && dw(b + 28) != pp->hidden)
        err("versteckte Sektoren %lu, erwartet %lu\n", dw(b + 28), pp->hidden);
    if (!original && (w(b + 24) != spt || w(b + 26) != heads))
        warn("BPB-Geometrie %u/%u, DMVGEOM1 %u/%u\n", w(b + 24), w(b + 26), spt, heads);
    if (!original && b[21] != 0xF8)
        warn("Medienbyte %02x (Platte: F8)\n", b[21]);
    if (!original && (b[0x24] & ~0x18) != 0)
        warn("Byte 24h hat Bits ausser der Einheit (18h)\n");

    fatlba = p0 + reserved;
    rootlba = fatlba + (ulong)nfat * fatsz;
    datalba = rootlba + ((ulong)rootents * 32 + SECT - 1) / SECT;
    dsec = totsec - (datalba - p0);
    ncl = dsec / spc;
    fat16 = ncl >= 4085;
    printf("  %s, %lu Cluster, Daten ab Sektor %lu\n", fat16 ? "FAT16" : "FAT12", ncl, datalba);
    if (pp->typ == 0x01 && fat16)
        err("Typ 01 (FAT12), aber %lu Cluster ergeben FAT16\n", ncl);
    if (pp->typ != 0x01 && !fat16)
        warn("Typ %02x, aber nur %lu Cluster: FAT12\n", pp->typ, ncl);
    if ((ulong)fatsz * SECT < (fat16 ? (ncl + 2) * 2 : (ncl + 2) * 3 / 2))
        err("FAT zu klein fuer %lu Cluster\n", ncl);
    fatbufsec = 0xFFFFFFFFL;
    if (rd(fatlba, fatbuf) == 0) {
        fatbufsec = fatlba;
        if (fatbuf[0] != b[21])
            err("FAT beginnt mit %02x, Medienbyte %02x\n", fatbuf[0], b[21]);
    }
    fatpruefung(0);
}

static int bpb_ok(uchar *b, unsigned int koepfe)
{
    unsigned int spc = b[13];

    return w(b + 11) == SECT && spc && !(spc & (spc - 1)) && b[16] && w(b + 17) &&
        w(b + 19) && w(b + 22) && w(b + 24) == 17 && w(b + 26) == koepfe;
}

/* wie HDIMPORT: sieht der Sektor wie ein CP/M-Verzeichnis aus? */
static int cpm_dir(uchar *b)
{
    int i, k;

    for (i = 0; i < 16; i++, b += 32) {
        if (b[0] == 0xE5)
            continue;
        if (b[0] > 15 || b[12] > 31 || b[15] > 128)
            return 0;
        for (k = 1; k < 12; k++)
            if ((b[k] & 0x7F) < 0x20 || (b[k] & 0x7F) > 0x7E)
                return 0;
    }
    return 1;
}

/* CP/M-Haelfte im Originalformat (Werte wie HDIMPORT): 305 Zylinder x 2
   Koepfe x 17 = 10370 Sektoren, Bloecke zu 8 KB (16 Sektoren), DSM 647,
   Verzeichnis in den Bloecken 0-1 (32 Sektoren, 512 Eintraege), 16-Bit-
   Blocknummern. Geprueft: Eintraege, Blocknummern, doppelt belegte Bloecke. */
#define CPM_DSM     647
#define CPM_DIRSEC  32

static void cpm_half(void)
{
    uchar b[SECT], map[(CPM_DSM + 8) / 8];
    unsigned int s, e, k, blk, users = 0;
    ulong eintraege = 0, dateien = 0, bloecke = 2, doppelt = 0, aussen = 0;

    memset(map, 0, sizeof(map));
    SETB(map, 0);
    SETB(map, 1);
    for (s = 0; s < CPM_DIRSEC; s++) {
        if (rd(s, b) < 0)
            return;
        for (e = 0; e < SECT / 32; e++) {
            uchar *d = b + e * 32;
            if (d[0] == 0xE5)
                continue;
            if (d[0] > 15) {
                if (d[0] != 0x20 && d[0] != 0x21)    /* Label/Zeitstempel */
                    warn("CP/M-Eintrag %u: User %u\n", s * 16 + e, d[0]);
                continue;
            }
            eintraege++;
            users |= 1 << d[0];
            if (d[14] == 0 && d[12] <= 3)
                dateien++;
            for (k = 0; k < 8; k++) {
                blk = w(d + 16 + 2 * k);
                if (!blk)
                    continue;
                if (blk > CPM_DSM) {
                    if (aussen++ < 5)
                        err("CP/M-Datei %.8s.%.3s: Block %u ueber DSM %u\n",
                            d + 1, d + 9, blk, CPM_DSM);
                    continue;
                }
                if (BIT(map, blk)) {
                    if (doppelt++ < 5)
                        err("CP/M-Datei %.8s.%.3s: Block %u doppelt belegt\n", d + 1, d + 9, blk);
                    continue;
                }
                SETB(map, blk);
                bloecke++;
            }
            if (verbose && d[14] == 0 && d[12] == 0)
                printf("    %u: %.8s.%.3s\n", d[0], d + 1, d + 9);
        }
    }
    printf("  CP/M: %lu Verzeichniseintraege von 512, %lu Dateien, User", eintraege, dateien);
    for (k = 0; k < 16; k++)
        if (users & (1 << k))
            printf(" %u", k);
    printf("\n  %lu Bloecke zu 8 KB belegt (mit Verzeichnis), %lu KB frei\n",
        bloecke, (ulong)(CPM_DSM + 1 - bloecke) * 8);
}

/* NCR MS-DOS 2.11 Originalformat (CONFIG.COM / FMT20): kein MBR, kein
   DMVGEOM1; physisch 4 Koepfe, 17 Sektoren. 10 oder 20 MB: eine FAT12-
   Partition ueber alle Koepfe. 2 x 5 MB: Haelfte A auf Kopf 0-1, B auf
   Kopf 2-3, jede DOS (BPB mit 2 Koepfen) oder CP/M. */
static void originalformat(void)
{
    struct part p;
    int h;

    printf("\nNCR-Originalformat (MS-DOS 2.11)?\n");
    original = 1;
    if (heads != 4 || spt != 17)
        warn("DMVGEOM1 nennt %u Koepfe/%u Sektoren, das Originalformat hat 4/17\n", heads, spt);
    if (rd(0, buf) < 0)
        return;
    if (bpb_ok(buf, 4)) {
        printf("  eine Partition ueber 4 Koepfe (10 oder 20 MB)\n");
        p.typ = 0x01;
        p.start = 0;
        p.size = w(buf + 19);
        p.hidden = dw(buf + 28);
        partition(&p, "Originalformat, eine Partition");
        return;
    }
    for (h = 0; h < 2; h++) {
        map_hp = 2;
        map_fh = 2 * h;
        if (rd(0, buf) < 0)
            break;
        if (bpb_ok(buf, 2)) {
            printf("\n  Haelfte %c (Kopf %d-%d): DOS\n", 'A' + h, 2 * h, 2 * h + 1);
            p.typ = 0x01;
            p.start = 0;
            p.size = w(buf + 19);
            p.hidden = dw(buf + 28);
            partition(&p, h ? "Originalformat, Haelfte B" : "Originalformat, Haelfte A");
        } else if (cpm_dir(buf)) {
            printf("\n  Haelfte %c (Kopf %d-%d): CP/M\n", 'A' + h, 2 * h, 2 * h + 1);
            cpm_half();
        }
        else
            printf("\n  Haelfte %c (Kopf %d-%d): weder DOS noch CP/M erkannt\n",
                'A' + h, 2 * h, 2 * h + 1);
    }
    map_hp = 0;
}

static int fattyp(int t)
{
    return t == 0x01 || t == 0x04 || t == 0x06;
}

/* erweiterte Partition: Kette der EBRs; Eintrag 1 ist das logische
   Laufwerk (Start relativ zum EBR), Eintrag 2 der naechste EBR (Start
   relativ zum Beginn der erweiterten Partition) */
static void erweitert(int i)
{
    ulong ext = pt[i].start, ebr = ext, end = pt[i].start + pt[i].size;
    struct part lp;
    uchar *e;
    int n = 0;
    char was[40];

    printf("\nErweiterte Partition %d (Sektor %lu bis %lu)\n", i + 1, ext, end - 1);
    while (n < 24) {
        uchar t2;
        ulong next;

        if (rd(ebr, buf) < 0)
            return;
        if (buf[510] != 0x55 || buf[511] != 0xAA) {
            err("EBR in Sektor %lu ohne 55AA\n", ebr);
            return;
        }
        e = buf + 0x1BE;
        lp.boot = e[0];
        lp.typ = e[4];
        lp.start = ebr + dw(e + 8);
        lp.size = dw(e + 12);
        lp.hidden = dw(e + 8);          /* bei DOS relativ zum EBR */
        t2 = e[16 + 4];
        next = dw(e + 16 + 8);
        n++;
        printf("  Logisches Laufwerk %d: Typ %02x %s, Sektor %lu, %lu Sektoren (EBR %lu)\n",
            n, lp.typ, typname(lp.typ), lp.start, lp.size, ebr);
        if (lp.typ && (lp.start < ebr + 1 || lp.start + lp.size > end))
            err("logisches Laufwerk %d liegt ausserhalb der erweiterten Partition\n", n);
        if (lp.typ && fattyp(lp.typ)) {
            snprintf(was, sizeof(was), "Logisches Laufwerk %d", n);
            partition(&lp, was);
        }
        if (t2 != 0x05 || !next)
            break;
        if (ext + next <= ebr || ext + next >= end) {
            err("naechster EBR %lu ausserhalb oder rueckwaerts\n", ext + next);
            break;
        }
        ebr = ext + next;
    }
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/hda";
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-n"))
            seitenweise = 0;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Aufruf: dmvhd [-v] [-n] [Geraet]\n  -v Dateien zeigen, -n nicht seitenweise\n");
            return 2;
        } else
            dev = argv[i];
    }
    if (!isatty(1))
        seitenweise = 0;
    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        perror(dev);
        return 2;
    }
    printf("dmvhd - Plattendiagnose NCR DMV, nur lesend - %s\n\n", dev);
    i = -1;
    if (geometrie() == 0 && (i = mbr()) == 1)
        originalformat();
    else if (i == 0)
        for (i = 0; i < 4; i++) {
            char was[20];
            struct part p = pt[i];

            p.hidden = p.start;

            snprintf(was, sizeof(was), "Partition %d", i + 1);
            if (fattyp(p.typ))
                partition(&p, was);
            else if (p.typ == 0x05)
                erweitert(i);
        }
    printf("\n%d Fehler, %d Warnungen\n", fehler, warnungen);
    close(fd);
    return fehler ? 1 : 0;
}
