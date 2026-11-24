/*
 * dmvhd, dmvfs - Festplattendiagnose fuer die NCR Decision Mate V (nur lesend)
 *
 * dmvhd: Uebersicht je Platte (/dev/cfa, /dev/cfb): DMVGEOM1-Block
 *   (Sektor-ID 1, Kopie auf ID 2), Haupt-Bootsatz (16BIT, 55AA, Eintraege
 *   mit CHS und LBA gegen die Geometrie), logische Laufwerke der
 *   erweiterten Partition, NCR-Originalformat (MS-DOS 2.11, ohne MBR, auch
 *   2 x 5 MB mit DOS- oder CP/M-Haelften); je Teil die Bootsektor-Angaben
 *   (OEM, BPB, DMV-Bytes 24h/25h) gegen den Eintrag.
 *   -v zeigt CHS und BPB je Teil, -e die Erklaerungsseite.
 * dmvfs (Verweis auf dieses Programm, oder dmvhd -f): Dateisysteme pruefen.
 *   FAT: Kopien sektorweise verglichen, Cluster gezaehlt, Ketten,
 *   Querverweise, Verzeichnisbaum, verlorene Cluster. CP/M (Originalformat):
 *   Verzeichnis, Blocknummern, doppelt belegte Bloecke. -v zeigt die Dateien.
 *
 * Gelesen wird die ganze Platte; die Teile werden aus dem eigenen Lesen von
 * MBR und EBRs gefunden, nicht ueber den Kern. Die FAT wird nicht im Ganzen
 * gehalten (16-Bit-Datensegment), sondern sektorweise gelesen; fuer die
 * Kettenpruefung reichen zwei Bitfelder.
 *
 * Am Bildschirm laeuft dmvhd als Menue (24 Zeilen, Zeile 25 Statuszeile):
 *   Geraeteseite, 1 2 3 Struktur einer Platte, D Dateisysteme, L Schritt-
 *   test der Diskettenlaufwerke, H Hilfe, Q Ende. Jede Seite beginnt mit
 *   einem Seitenvorschub (die DMV-Konsole loescht dabei den Bildschirm).
 * Einmal ausgeben und enden (auch wenn die Ausgabe umgeleitet ist):
 *         dmvhd -n [-v] [-e] [Platte ...]  Vorgabe /dev/cfa /dev/cfb /dev/cfc
 *         dmvfs [-v] [-n] [Platte [Nr]]    Nr wie in der Tabelle von dmvhd
 *         dmvhd -s                         Startbildschirm (aus /etc/rc.sys)
 *   -n ohne Menue und ohne Halt nach jeder Bildschirmseite
 *
 * Texte sind UTF-8; die DMV-Konsole setzt die Umlaute in die Codes des
 * Zeichengenerators um. Spalten werden nach Zeichen gezaehlt, nicht nach
 * Byte (utf_len).
 *
 * CP/M auf Partitionen (Festlegung 24.11.2026, dmv_os docs/DMV-
 * Bootmanager.md Stufe 3): 2Eh RomWBW-Behaelter mit Scheiben zu 8 MB
 * (hd1k), DBh nach DRs PC-Muster mit Kopfblock.
 *
 * Grundlagen: rfka01/dmv_os docs/DMV-Datentraeger.md (DMVGEOM1, OEM-
 * Kennungen, Originalformat), docs/DMV-Schnittstellen.md (Abschnitt 4,
 * Bytes 24h/25h).
 *
 * 2026, rfka01 und Claude (Anthropic). GPL-2.0 wie ELKS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <termios.h>
#ifdef __ia16__
#include <arch/io.h>
#include <arch/dmvver.h>
#else                           /* Pruefung auf dem PC mit Abbilddateien */
#define inb(p)      0xFF
#define outb(v, p)  ((void)(v))
#define DMV_VERSION "v000"
#endif

#define SECT    512

typedef unsigned long ulong;
typedef unsigned short ushort;
typedef unsigned char uchar;

static int fd, verbose, fehler, warnungen, still;

/* Seitenweise Ausgabe: der Bildschirm hat 25 Zeilen zu 80 Zeichen und
   rollt nicht zurueck. Ist eine Seite (24 Zeilen) voll und kommt noch
   etwas, wird auf die Eingabetaste gewartet, wenn die Ausgabe auf den
   Bildschirm geht (-n schaltet es ab). Zeilen ueber 80 Zeichen bricht die
   Konsole um; sie zaehlen doppelt. Die Ausgabe selbst bleibt unter 80. */
#define SEITE   24
static int zeilen, spalte, seitenweise = 1, abbruch, stumm, menue;

/* eine Taste ohne Echo und ohne Eingabetaste; 0 wenn nicht moeglich */
static int taste(void)
{
    struct termios alt, roh;
    char c = 0;
    int t = tcgetattr(0, &alt) == 0;

    if (t) {
        roh = alt;
        roh.c_lflag &= ~(ECHO | ICANON);
        roh.c_cc[VMIN] = 1;
        roh.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &roh);
    }
    if (read(0, &c, 1) != 1)
        c = 'q';
    if (t)
        tcsetattr(0, TCSANOW, &alt);
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static void seite(const char *s)
{
    int c[1];

    if (stumm)
        return;
    for (; *s && !abbruch; s++) {
        if (*s == '\f') {      /* neue Seite: Bildschirm loeschen */
            putchar(*s);
            zeilen = spalte = 0;
            continue;
        }
        if (seitenweise && zeilen >= SEITE - menue) {
            fflush(stdout);
            if (menue)          /* Hinweis in der Statuszeile, dann zurueck */
                fputs("\033[25;1H\033[K-- weiter mit Eingabetaste, \033[1mQ\033[0m bricht ab --",
                    stdout);
            else
                fputs("-- weiter mit Eingabetaste, q bricht ab --", stderr);
            fflush(stdout);
            do                  /* ohne Echo: die Zeile wird wieder geloescht */
                c[0] = taste();
            while (c[0] != '\n' && c[0] != '\r' && c[0] != 'q');
            if (menue)
                fputs("\033[25;1H\033[K\033[24;1H", stdout);
            else
                fputs("\r                                          \r", stderr);
            if (c[0] == 'q') {
                abbruch = 1;    /* Rest der Seite verwerfen */
                return;
            }
            zeilen = 0;
        }
        putchar(*s);
        if ((*s & 0xC0) == 0x80)    /* UTF-8-Folgebyte: keine neue Spalte */
            continue;
        if (*s == '\n' || ++spalte >= 80) {
            if (*s != '\n')
                fflush(stdout);
            zeilen++;
            spalte = 0;
        }
    }
}

/* Fortschritt auf stderr, verschwindet wieder (nur am Bildschirm) */
static void fortschritt(const char *s)
{
    if (!seitenweise)
        return;
    fflush(stdout);
    fputs(s, stderr);
    fputs("\r", stderr);
}

static void fortschritt_weg(void)
{
    if (seitenweise)
        fputs("                                        \r", stderr);
}

static int out(const char *fmt, ...)
{
    char z[200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(z, sizeof(z), fmt, ap);
    va_end(ap);
    seite(z);
    return n;
}
#define printf out

static uchar buf[SECT];
static unsigned int cyls, heads, spt;   /* aus DMVGEOM1 */
static ulong disksects;

/* Einzahl oder Mehrzahl */
static const char *pl(ulong n, const char *ein, const char *mehr)
{
    return n == 1 ? ein : mehr;
}

static ushort w(uchar *p) { return p[0] | (p[1] << 8); }
static ulong dw(uchar *p) { return w(p) | ((ulong)w(p + 2) << 16); }

/* Befunde; 'still' unterdrueckt sie (dmvfs zeigt nur die Dateisysteme) */
static void err(const char *fmt, ...)
{
    char z[200];
    va_list ap;

    if (still)
        return;
    va_start(ap, fmt);
    vsnprintf(z, sizeof(z), fmt, ap);
    va_end(ap);
    printf("    FEHLER: %s", z);
    fehler++;
}

static void warn(const char *fmt, ...)
{
    char z[200];
    va_list ap;

    if (still)
        return;
    va_start(ap, fmt);
    vsnprintf(z, sizeof(z), fmt, ap);
    va_end(ap);
    printf("    Warnung: %s", z);
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

/* ---------------------------------------------------------- Teile -- */
/* Ein Teil ist eine Zeile der Tabelle: Partition 1-4, logisches Laufwerk
   ab 5 (Zaehlung wie ELKS, cfa5 ...), Originalformat "-" (eine Partition)
   oder Haelfte "A"/"B" (2 x 5 MB, Kopf 0-1 und 2-3). */
#define ART_LEER    0
#define ART_FAT     1
#define ART_CPM     2
#define ART_ERW     3
#define ART_ANDERS  4
#define ART_RWBW    5           /* 2Eh: RomWBW-Behaelter, Scheiben zu 8 MB */
#define ART_DRCPM   6           /* DBh: CP/M-86 nach DRs PC-Muster */
#define MAXTEIL     16

struct teil {
    char nr[3];
    uchar boot, typ, art;
    uchar halb;                 /* 0, 1 = Kopf 0-1, 2 = Kopf 2-3 */
    ulong start, size;
    ulong hidden;               /* erwartete versteckte Sektoren im BPB */
    unsigned int sc, sh, ss, ec, eh, es;    /* CHS aus dem Eintrag */
    /* CP/M (DBh): aus dem DR-Kopfblock; ok = Kopfblock brauchbar */
    uchar ok;
    ulong dir0;                 /* erster Sektor des Verzeichnisses */
    unsigned int dsm, drm, spb; /* hoechster Block, Eintrag, Sektoren/Block */
    unsigned int off;           /* OFF aus dem Kopfblock (Spuren) */
};

/* RomWBW hd1k: Scheibe 8 MB = 16384 Sektoren, davon 32 Startsektoren,
   Bloecke zu 4 KB (8 Sektoren), 2044 Bloecke, 1024 Verzeichniseintraege */
#define RW_SLICE    16384L
#define RW_SYS      32
#define RW_DSM      2043
#define RW_DRM      1023
static struct teil teile[MAXTEIL];
static int nteile;
static char leer[12];           /* leere Eintraege im MBR, "1, 2, 3" */

static struct teil *neu(const char *nr)
{
    struct teil *t;

    if (nteile >= MAXTEIL)
        return 0;
    t = &teile[nteile++];
    memset(t, 0, sizeof(*t));
    strncpy(t->nr, nr, 2);
    return t;
}

static void halbe(struct teil *t)
{
    map_hp = t->halb ? 2 : 0;
    map_fh = t->halb ? 2 * (t->halb - 1) : 0;
}


static int fattyp(int t)
{
    return t == 0x01 || t == 0x04 || t == 0x06;
}

/* ----------------------------------------------------- DMVGEOM1 -- */
static ulong geo_pre, geo_land;
static unsigned int geo_schritt, geo_il, geo_lw;
static int geo_ok;

static void geometrie(void)
{
    int id, fehlt = 0, ok = 0;
    unsigned int sum, i;

    geo_ok = 0;
    for (id = 1; id <= 2; id++) {
        if (rd(id, buf) < 0)
            continue;
        if (memcmp(buf, "DMVGEOM1", 8)) {
            fehlt |= id;
            continue;
        }
        for (sum = 0, i = 0; i < 22; i++)
            sum += buf[i];
        if (sum != w(buf + 22)) {
            err("DMVGEOM1 auf ID %d: Prüfsumme %04x, gespeichert %04x\n", id, sum, w(buf + 22));
            continue;
        }
        if (!ok) {
            cyls = w(buf + 8);
            heads = buf[10];
            spt = buf[11];
            geo_pre = w(buf + 14);
            geo_land = w(buf + 16);
            geo_schritt = buf[18];
            geo_il = buf[19];
            geo_lw = buf[20];
            ok = id;
            if (w(buf + 12) != SECT)
                err("Sektorgröße %u statt 512\n", w(buf + 12));
            if (heads == 0 || heads > 8)
                err("%u Köpfe: die DMV kann 1 bis 8\n", heads);
            if (cyls == 0 || cyls > 1024)
                err("%u Zylinder: erlaubt 1 bis 1024\n", cyls);
        } else if (w(buf + 8) != cyls || buf[10] != heads || buf[11] != spt)
            err("Kopie des DMVGEOM1 auf ID 2 weicht von ID 1 ab\n");
    }
    if (ok && (fehlt & 2))
        warn("keine Kopie des DMVGEOM1 auf Sektor-ID 2\n");
    if (!ok) {
        cyls = 0;
        heads = 4;
        spt = 17;
        disksects = 0xFFFFFFFFL;
        return;
    }
    geo_ok = 1;
    disksects = (ulong)cyls * heads * spt;
}

static void geo_zeilen(int nr, const char *dev)
{
    if (!geo_ok) {
        printf("Platte %d (%s): kein DMVGEOM1, gelesen mit 4 Köpfen, 17 Sektoren\n",
            nr, dev);
        warn("ohne DMVGEOM1 fehlt die Einheit für die DOS-Ports\n");
        return;
    }
    printf("Platte %d (%s): %lu KB, %u Zylinder, %u Köpfe, %u Sektoren je Spur\n",
        nr, dev, disksects / 2, cyls, heads, spt);
    printf("  DMVGEOM1: Precomp %lu, Landezone %lu, Schritt %u, Interleave %u\n",
        geo_pre, geo_land, geo_schritt, geo_il);
}

/* --------------------------------------------------- Bootsaetze -- */
/* Teile aus dem MBR; 1: kein 55AA (Originalformat pruefen), -1 Fehler */
static int mbr(void)
{
    int i;
    uchar *e;
    struct teil *t;
    char nr[3];

    leer[0] = 0;
    if (rd(0, buf) < 0)
        return -1;
    if (buf[510] != 0x55 || buf[511] != 0xAA)
        return 1;
    for (i = 0; i < 4; i++) {
        e = buf + 0x1BE + 16 * i;
        if (!e[4]) {
            sprintf(leer + strlen(leer), "%s%d", leer[0] ? ", " : "", i + 1);
            continue;
        }
        sprintf(nr, "%d", i + 1);
        if (!(t = neu(nr)))
            break;
        t->boot = e[0];
        t->typ = e[4];
        t->start = dw(e + 8);
        t->size = dw(e + 12);
        t->hidden = t->start;
        t->art = fattyp(t->typ) ? ART_FAT : t->typ == 0x05 ? ART_ERW :
            t->typ == 0x2E ? ART_RWBW : t->typ == 0xDB ? ART_DRCPM : ART_ANDERS;
        t->sh = e[1]; t->ss = e[2] & 63; t->sc = e[3] | ((e[2] & 0xC0) << 2);
        t->eh = e[5]; t->es = e[6] & 63; t->ec = e[7] | ((e[6] & 0xC0) << 2);
    }
    return 0;
}

/* Pruefungen eines MBR-Eintrags (als Befunde unter seiner Zeile) */
static void eintrag_pruefen(struct teil *t)
{
    int j;
    ulong a, b;

    if (t->boot != 0 && t->boot != 0x80)
        err("Bootkennung %02x (erlaubt 00/80)\n", t->boot);
    if (t->art == ART_ANDERS)
        warn("Typ %02x: weder DOS noch CP/M (2Eh, DBh)\n", t->typ);
    if (t->art == ART_RWBW && t->size < RW_SLICE)
        err("kleiner als eine Scheibe (8 MB = %lu Sektoren)\n", RW_SLICE);
    if (t->ss == 0 || t->es == 0)
        err("Sektor 0 in CHS (Partitionseinträge zählen ab 1)\n");
    else if (geo_ok) {
        a = chs2lba(t->sc, t->sh, t->ss);
        b = chs2lba(t->ec, t->eh, t->es);
        if (a != t->start)
            err("Start-CHS ergibt LBA %lu, eingetragen %lu\n", a, t->start);
        if (b != t->start + t->size - 1)
            err("Ende-CHS ergibt LBA %lu, erwartet %lu\n", b, t->start + t->size - 1);
    }
    if (t->start < 3)
        err("beginnt vor Sektor 3 und überschreibt DMVGEOM1\n");
    if (t->start + t->size > disksects)
        err("reicht über das Plattenende (%lu Sektoren)\n", disksects);
    for (j = 0; j < nteile && &teile[j] != t; j++)
        if (teile[j].nr[0] <= '4' && t->start < teile[j].start + teile[j].size &&
            teile[j].start < t->start + t->size)
            err("überlappt Eintrag %s\n", teile[j].nr);
    if (t->typ == 0x04 && t->size > 65535L)
        err("Typ 04 mit mehr als 65535 Sektoren\n");
    if (t->typ == 0x06 && t->size <= 65535L)
        warn("Typ 06, wäre als 04 klein genug\n");
}

/* logische Laufwerke: Kette der EBRs; Eintrag 1 ist das Laufwerk (Start
   relativ zum EBR), Eintrag 2 der naechste EBR (relativ zum Beginn der
   erweiterten Partition) */
static void erweitert(struct teil *x)
{
    ulong ext = x->start, ebr = ext, end = x->start + x->size;
    struct teil *t;
    uchar *e;
    int n = 5;
    char nr[3];

    while (n < 5 + 12) {
        uchar t2;
        ulong next;

        if (rd(ebr, buf) < 0)
            return;
        if (buf[510] != 0x55 || buf[511] != 0xAA) {
            err("EBR in Sektor %lu ohne 55AA\n", ebr);
            return;
        }
        e = buf + 0x1BE;
        sprintf(nr, "%d", n++);
        if (!(t = neu(nr)))
            return;
        t->boot = e[0];
        t->typ = e[4];
        t->start = ebr + dw(e + 8);
        t->size = dw(e + 12);
        t->hidden = dw(e + 8);          /* bei DOS relativ zum EBR */
        t->art = fattyp(t->typ) ? ART_FAT : t->typ ? ART_ANDERS : ART_LEER;
        t2 = e[16 + 4];
        next = dw(e + 16 + 8);
        if (t2 != 0x05 || !next)
            break;
        if (ext + next <= ebr || ext + next >= end) {
            err("nächster EBR %lu außerhalb oder rückwärts\n", ext + next);
            break;
        }
        ebr = ext + next;
    }
}

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
                err("%s: Verzeichnis ohne gültigen Startcluster %lu\n", voll, c);
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
                    warn("%s: Länge 0, aber Startcluster %lu\n", voll, c);
                continue;
            }
            if (c < 2 || c >= ncl + 2) {
                err("%s: ungültiger Startcluster %lu\n", voll, c);
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
            err("%s: Verzeichniskette läuft in belegten Cluster %lu\n", pfad, c);
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

    fortschritt("  FAT-Kopien vergleichen ...");
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

    seen = calloc(1, (unsigned int)((ncl + 2 + 7) / 8));
    refd = calloc(1, (unsigned int)((ncl + 2 + 7) / 8));
    if (!seen || !refd) {
        err("zu wenig Speicher für %lu Cluster\n", ncl);
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
                        err("Cluster %lu zeigt nach %lx, außerhalb\n", n, v);
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
    fortschritt_weg();
    if (diff)
        printf("  %lu FAT-Sektoren verschieden", diff);
    else if (nfat > 1)
        printf("  %u FATs gleich", nfat);
    else
        printf("  eine FAT");
    printf(", Cluster: %lu frei, %lu belegt, %lu defekt (%lu KB frei)\n",
        frei, belegt, defekt, frei * spc / 2);

    fortschritt("  Verzeichnisbaum durchlaufen ...");
    nfiles = ndirs = 0;
    verzeichnis(0, 1, "", 0);
    fortschritt_weg();

    for (n = 2; n < ncl + 2; n++) {
        v = fatget(n);
        if (v == 0 || is_bad(v) || BIT(seen, n))
            continue;
        verloren++;
        if (!BIT(refd, n))
            ketten++;
    }
    printf("  %lu %s, %lu %s, %s\n", nfiles, pl(nfiles, "Datei", "Dateien"),
        ndirs, pl(ndirs, "Verzeichnis", "Verzeichnisse"),
        verloren ? "verlorene Cluster:" : "keine verlorenen Cluster");
    if (verloren)
        err("%lu verlorene Cluster in %lu Ketten (belegt, aber von keiner Datei)\n",
            verloren, ketten);
    free(seen);
    free(refd);
    (void)nr;
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
        if (b[0] == 0xE5 || b[0] == 0x21)       /* frei, Zeitstempel */
            continue;
        if ((b[0] > 15 && b[0] != 0x20) || b[12] > 31 || b[15] > 128)
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

/* Ein CP/M-Dateisystem pruefen: Verzeichnis ab Sektor dir0 (zugleich
   Block 0), Bloecke zu spb Sektoren, hoechster Block dsm, hoechster
   Eintrag drm; 16-Bit-Blocknummern, 8 je Eintrag (alle Formate der DMV:
   NCR-Haelfte, RomWBW-Scheibe, DR-Bereich). Ein Verzeichnis, das nicht
   wie CP/M aussieht (z. B. nur Nullen), heisst "nicht eingerichtet". */
static void cpm_pruefen(ulong dir0, unsigned int spb, unsigned int dsm, unsigned int drm)
{
    uchar b[SECT], *map;
    unsigned int s, e, k, blk, users = 0, dirsec = (drm + 1) / 16, dirblk;
    unsigned int exm = spb / 4 - 1;     /* 8 Blocknummern je Eintrag */
    ulong eintraege = 0, dateien = 0, bloecke, doppelt = 0, aussen = 0;
    char lab[12];

    lab[0] = 0;
    if (rd(dir0, b) < 0)
        return;
    if (!cpm_dir(b)) {
        printf("  Verzeichnis nicht eingerichtet (kein CP/M-Inhalt)\n");
        return;
    }
    map = calloc(dsm / 8 + 1, 1);
    if (!map) {
        err("zu wenig Speicher für %u Blöcke\n", dsm + 1);
        return;
    }
    dirblk = (dirsec + spb - 1) / spb;
    for (k = 0; k < dirblk; k++)
        SETB(map, k);
    bloecke = dirblk;
    for (s = 0; s < dirsec && !abbruch; s++) {
        if (rd(dir0 + s, b) < 0)
            break;
        for (e = 0; e < SECT / 32; e++) {
            uchar *d = b + e * 32;
            if (d[0] == 0xE5)
                continue;
            if (d[0] > 15) {
                if (d[0] == 0x20) {             /* Name des Laufwerks */
                    memcpy(lab, d + 1, 11);
                    lab[11] = 0;
                } else if (d[0] != 0x21)        /* 21h: Zeitstempel */
                    warn("CP/M-Eintrag %u: User %u\n", s * 16 + e, d[0]);
                continue;
            }
            eintraege++;
            users |= 1 << d[0];
            if (d[14] == 0 && d[12] <= exm)
                dateien++;
            for (k = 0; k < 8; k++) {
                blk = w(d + 16 + 2 * k);
                if (!blk)
                    continue;
                if (blk > dsm) {
                    if (aussen++ < 5)
                        err("CP/M-Datei %.8s.%.3s: Block %u über DSM %u\n",
                            d + 1, d + 9, blk, dsm);
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
            if (verbose && d[14] == 0 && d[12] <= exm)
                printf("    %u: %.8s.%.3s\n", d[0], d + 1, d + 9);
        }
    }
    free(map);
    if (lab[0])
        printf("  Name \"%s\"\n", lab);
    if (!eintraege)
        printf("  keine Dateien (%u Verzeichniseinträge frei)\n", drm + 1);
    else {
        printf("  %lu von %u Verzeichniseinträgen, %lu %s, User", eintraege, drm + 1,
            dateien, pl(dateien, "Datei", "Dateien"));
        for (k = 0; k < 16; k++)
            if (users & (1 << k))
                printf(" %u", k);
        printf("\n");
    }
    printf("  %lu Blöcke zu %u KB belegt (mit Verzeichnis), %lu KB frei\n",
        bloecke, spb / 2, (ulong)(dsm + 1 - bloecke) * (spb / 2));
}

static void cpm_half(void)
{
    cpm_pruefen(0, 16, CPM_DSM, 511);
}

/* DBh: Kopfblock nach DRs PC-Muster im ersten Zylinder der Partition,
   Kopf 0, Sektor 4. 256 Worte mit Summe 0; ab 21h Lage und Geometrie,
   ab 29h der DPB (SPT in 128-Byte-Saetzen, OFF in Spuren ab Plattenanfang),
   ab 40h Blockgroesse/128. */
static void dr_kopf(struct teil *t, int befunde)
{
    ulong l, sum = 0;
    unsigned int i, c0, spur, bls;
    uchar k[SECT];

    t->ok = 0;
    if (!heads || !spt)
        return;
    c0 = t->start / ((ulong)heads * spt);
    l = (ulong)c0 * heads * spt + 3;
    if (rd(l, k) < 0)
        return;
    for (i = 0; i < SECT; i += 2)
        sum += w(k + i);
    if ((sum & 0xFFFF) || w(k + 0x23) == 0) {
        if (befunde)
            err("kein DR-Kopfblock (Zylinder %u, Kopf 0, Sektor 4)\n", c0);
        return;
    }
    spur = k[0x26] * (k[0x28] * 128 / SECT);        /* Sektoren je Spur */
    bls = w(k + 0x40) * 128;
    t->dsm = w(k + 0x2E);
    t->drm = w(k + 0x30);
    t->spb = bls / SECT;
    t->off = w(k + 0x36);
    t->dir0 = (ulong)t->off * spur;
    t->ok = t->spb && t->dsm && (128U << k[0x2B]) == bls;
    if (!befunde)
        return;
    if (!t->ok)
        err("DR-Kopfblock: Blockgröße %u passt nicht zu BSH %u\n", bls, k[0x2B]);
    if (w(k + 0x21) != c0)
        err("DR-Kopfblock nennt Zylinder %u, Partition beginnt auf %u\n", w(k + 0x21), c0);
    if (k[0x25] != heads || k[0x26] != spt)
        err("DR-Kopfblock: %u Köpfe/%u Sektoren, DMVGEOM1 %u/%u\n",
            k[0x25], k[0x26], heads, spt);
    if (t->dir0 < l + 1 || t->dir0 >= t->start + t->size)
        err("Verzeichnis (OFF %u Spuren) außerhalb der Partition\n", w(k + 0x36));
    else if (t->dir0 + (ulong)(t->dsm + 1) * t->spb > t->start + t->size)
        err("DSM %u: Blöcke reichen über das Partitionsende\n", t->dsm);
}

/* 2Eh: Zahl der ganzen Scheiben, Rest in Sektoren */
static unsigned int scheiben(struct teil *t)
{
    return (unsigned int)(t->size / RW_SLICE);
}

static void rw_pruefen(struct teil *t)
{
    unsigned int i, n = scheiben(t);
    char z[24];

    for (i = 0; i < n && !abbruch; i++) {
        sprintf(z, "  Scheibe %u:\n", i);
        printf("%s", z);
        cpm_pruefen(t->start + i * RW_SLICE + RW_SYS, 8, RW_DSM, RW_DRM);
    }
}

/* ----------------------------------------------------- Bootsektor -- */
static uchar bs[SECT];          /* Bootsektor des aktuellen Teils */

/* BPB lesen und Lage der FAT-Bereiche rechnen; 0 = brauchbar */
static int bpb_lesen(struct teil *t)
{
    halbe(t);
    p0 = t->start;
    if (rd(p0, bs) < 0)
        return -1;
    bps = w(bs + 11);
    spc = bs[13];
    reserved = w(bs + 14);
    nfat = bs[16];
    rootents = w(bs + 17);
    totsec = w(bs + 19) ? w(bs + 19) : dw(bs + 32);
    fatsz = w(bs + 22);
    fatlba = p0 + reserved;
    rootlba = fatlba + (ulong)nfat * fatsz;
    datalba = rootlba + ((ulong)rootents * 32 + SECT - 1) / SECT;
    ncl = 0;
    if (spc && totsec > datalba - p0)
        ncl = (totsec - (datalba - p0)) / spc;
    fat16 = ncl >= 4085;
    if (bps != SECT || !spc || (spc & (spc - 1)) || !nfat || !fatsz || !rootents || !ncl)
        return 1;
    return 0;
}

/* Spalte "Inhalt" und Befunde zum Bootsektor eines FAT-Teils */
static void fat_inhalt(struct teil *t, char *z)
{
    int r = bpb_lesen(t);

    if (r < 0) {
        strcpy(z, "nicht lesbar");
        return;
    }
    if (r) {
        strcpy(z, "kein brauchbarer BPB");
        return;
    }
    sprintf(z, "%s, Cluster %u KB", fat16 ? "FAT16" : "FAT12", spc / 2);
}

static void fat_befunde(struct teil *t)
{
    ulong hid = original ? (ulong)w(bs + 28) : dw(bs + 28);

    if (!original && !memcmp(bs + 3, "16BIT", 5))
        err("16BIT im Bootsektor einer Partition: MS-DOS 3.3 verwirft den BPB\n");
    else if (!original && memcmp(bs + 3, "IBM  3.3", 8))
        warn("OEM \"%.8s\" statt \"IBM  3.3\": MS-DOS 3.3 nimmt den BPB nicht\n", bs + 3);
    if (!original && (bs[510] != 0x55 || bs[511] != 0xAA))
        warn("Bootsektor ohne 55AA\n");
    if (totsec != t->size)
        err("BPB nennt %lu Sektoren, der Eintrag %lu\n", totsec, t->size);
    if (!original && hid != t->hidden)
        err("versteckte Sektoren %lu, erwartet %lu\n", hid, t->hidden);
    if (!original && geo_ok && (w(bs + 24) != spt || w(bs + 26) != heads))
        warn("BPB-Geometrie %u/%u, DMVGEOM1 %u/%u\n", w(bs + 24), w(bs + 26), spt, heads);
    if (!original && bs[21] != 0xF8)
        warn("Medienbyte %02x (Platte: F8)\n", bs[21]);
    if (!original && (bs[0x24] & ~0x18) != 0)
        warn("Byte 24h hat Bits ausser der Einheit (18h)\n");
    if (t->typ == 0x01 && fat16)
        err("Typ 01 (FAT12), aber %lu Cluster ergeben FAT16\n", ncl);
    if (t->typ != 0x01 && !fat16)
        warn("Typ %02x, aber nur %lu Cluster: FAT12\n", t->typ, ncl);
    if ((ulong)fatsz * SECT < (fat16 ? (ncl + 2) * 2 : (ncl + 2) * 3 / 2))
        err("FAT zu klein für %lu Cluster\n", ncl);
    fatbufsec = 0xFFFFFFFFL;
    if (rd(fatlba, fatbuf) == 0) {
        fatbufsec = fatlba;
        if (fatbuf[0] != bs[21])
            err("FAT beginnt mit %02x, Medienbyte %02x\n", fatbuf[0], bs[21]);
    }
}

/* -v: Einzelheiten unter der Zeile */
static void fat_einzeln(struct teil *t)
{
    if (t->nr[0] >= '1' && t->nr[0] <= '4' && !t->nr[1])
        printf("      CHS %u/%u/%u bis %u/%u/%u\n", t->sc, t->sh, t->ss, t->ec, t->eh, t->es);
    printf("      OEM \"%.8s\", versteckt %lu", bs + 3,
        original ? (ulong)w(bs + 28) : dw(bs + 28));
    if (!original)
        printf(", Byte 24h/25h %02x/%02x", bs[0x24], bs[0x25]);
    printf(", Daten ab %lu\n", datalba);
    printf("      BPB: %u Byte/Sektor, %u Sekt./Cluster, %lu reserviert, %u FAT%s zu %u\n",
        bps, spc, reserved, nfat, nfat == 1 ? "" : "s", fatsz);
    printf("           %u Wurzeleinträge, %lu Sektoren, Medium %02x, %u/%u Sekt./Köpfe\n",
        rootents, totsec, bs[21], w(bs + 24), w(bs + 26));
}

/* ------------------------------------------------ Originalformat -- */
/* NCR MS-DOS 2.11 Originalformat (CONFIG.COM / FMT20): kein MBR, kein
   DMVGEOM1; physisch 4 Koepfe, 17 Sektoren. 10 oder 20 MB: eine FAT12-
   Partition ueber alle Koepfe. 2 x 5 MB: Haelfte A auf Kopf 0-1, B auf
   Kopf 2-3, jede DOS (BPB mit 2 Koepfen) oder CP/M. */
static int originalformat(void)
{
    struct teil *t;
    int h;

    original = 1;
    if (rd(0, buf) < 0)
        return 0;
    if (bpb_ok(buf, 4)) {
        t = neu("-");
        t->typ = 0x01;
        t->art = ART_FAT;
        t->size = w(buf + 19);
        t->hidden = w(buf + 28);
        t->boot = buf[3] == '1' ? 0x80 : 0;     /* "16BIT": das Z80-ROM startet */
        return 1;
    }
    for (h = 1; h <= 2; h++) {
        t = neu(h == 1 ? "A" : "B");
        t->halb = h;
        halbe(t);
        t->size = 10370;            /* 305 Zylinder x 2 Koepfe x 17 */
        if (rd(0, buf) < 0)
            t->art = ART_ANDERS;
        else if (bpb_ok(buf, 2)) {
            t->typ = 0x01;
            t->art = ART_FAT;
            t->size = w(buf + 19);
            t->boot = buf[3] == '1' ? 0x80 : 0;
        } else if (cpm_dir(buf))
            t->art = ART_CPM;
        else
            t->art = ART_ANDERS;
    }
    map_hp = 0;
    return 2;
}

/* ------------------------------------------------------- Platte -- */
static int platte_lesen(const char *dev, int einheit)
{
    fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    nteile = 0;
    original = 0;
    map_hp = 0;
    still++;
    geometrie();            /* Befunde erst unter der Plattenzeile */
    still--;
    (void)einheit;
    return 0;
}

/* --------------------------------------------------------- Kuerzel -- */
/* Kuerzel eines Teils fuer Geraete- und Strukturseite (5 Zeichen), wie in
   der Hilfe erklaert */
static void kuerzel(struct teil *t, char *k)
{
    switch (t->art) {
    case ART_FAT:
        if (t->halb)
            strcpy(k, "DOS05");
        else if (original)
            strcpy(k, t->size <= 24000L ? "DOS10" : "DOS20");
        else
            strcpy(k, t->typ == 0x01 ? "FAT12" : t->typ == 0x06 ? "FAT2G" : "FAT16");
        break;
    case ART_CPM:
        strcpy(k, "CPM05");
        break;
    case ART_ERW:
        strcpy(k, "EXTND");
        break;
    case ART_RWBW:
        sprintf(k, "RWBW%u", scheiben(t));
        break;
    case ART_DRCPM:
        sprintf(k, "CPM%02lu", (t->size + 1024) / 2048);
        break;
    case ART_LEER:
        strcpy(k, "-----");
        break;
    default:
        if (t->halb)
            strcpy(k, "?????");
        else
            sprintf(k, "?%02X", t->typ);
    }
}

/* Zylinder/Kopf/Sektor eines Sektors im Teil (rel ab Teilanfang) */
static void chs_text(struct teil *t, ulong rel, char *z)
{
    ulong c, h, sk;

    if (t->halb) {              /* Haelfte: 2 Koepfe ab Kopf 0 bzw. 2 */
        c = rel / 34;
        h = (rel / 17) % 2 + 2 * (t->halb - 1);
        sk = rel % 17 + 1;
    } else {
        rel += t->start;
        c = rel / ((ulong)heads * spt);
        h = (rel / spt) % heads;
        sk = rel % spt + 1;
    }
    sprintf(z, "%lu/%lu/%lu", c, h, sk);
}

/* belegte Zylinder einer Platte im Originalformat (ohne DMVGEOM1):
   CONFIG schreibt die Plattengroesse nirgends hin, erschlossen wird
   nur der belegte Teil aus den Groessen der Teile */
static unsigned int e_cyls;

static void erschliessen(void)
{
    int i;
    ulong c;

    e_cyls = 0;
    for (i = 0; i < nteile; i++) {
        c = teile[i].halb ? (teile[i].size + 33) / 34 : (teile[i].size + 67) / 68;
        if (c > e_cyls)
            e_cyls = (unsigned int)c;
    }
}

static int teile_suchen(void)
{
    int i, r = mbr(), n;

    e_cyls = 0;
    if (r == 1) {
        r = originalformat() + 10;      /* 11 eine Partition, 12 2 x 5 MB */
        erschliessen();
        return r;
    }
    n = nteile;
    for (i = 0; i < n; i++)
        if (teile[i].art == ART_ERW)
            erweitert(&teile[i]);
    return r;
}

/* ------------------------------------------------- Strukturseite -- */
/* Spalten: Nr, startbar, Typ und Kuerzel, Anfang und Ende als Z/K/S,
   Groesse in KB, Inhalt; darunter LBA und Bootsektor-Angaben (Weg a) */
static const char kopf_struktur[] =
"Nr  B  Typ         Anfang Z/K/S    Ende Z/K/S  Größe KB  Inhalt\n"
"==============================================================================\n";

static void zeile(struct teil *t)
{
    char z[40], typ[16], k[8], anf[16], end[16];

    kuerzel(t, k);
    if (t->halb || original)
        sprintf(typ, "-- %s", k);
    else
        sprintf(typ, "%02X %s", t->typ, k);
    z[0] = 0;
    switch (t->art) {
    case ART_FAT:
        fat_inhalt(t, z);
        break;
    case ART_CPM:
        strcpy(z, "CP/M, Blöcke 8 KB");
        break;
    case ART_RWBW: {
        unsigned int n = scheiben(t);
        sprintf(z, "%u %s zu 8 MB", n, pl(n, "Scheibe", "Scheiben"));
        break;
    }
    case ART_DRCPM:
        still++;
        dr_kopf(t, 0);
        still--;
        if (t->ok)
            sprintf(z, "CP/M, Blöcke %u KB", t->spb / 2);
        else
            strcpy(z, "ohne DR-Kopfblock");
        break;
    case ART_ERW: {
        int i, a = 0, b = 0;
        for (i = 0; i < nteile; i++)
            if (teile[i].nr[0] >= '5' || teile[i].nr[1]) {
                if (!a)
                    a = atoi(teile[i].nr);
                b = atoi(teile[i].nr);
            }
        if (!a)
            strcpy(z, "ohne log. Laufwerk");
        else if (a == b)
            sprintf(z, "log. Laufwerk %d", a);
        else
            sprintf(z, "log. Laufwerke %d-%d", a, b);
        break;
    }
    case ART_LEER:
        strcpy(z, "leer");
        break;
    default:
        strcpy(z, t->halb ? "weder DOS noch CP/M" : "nicht erkannt");
    }
    chs_text(t, 0, anf);
    chs_text(t, t->size ? t->size - 1 : 0, end);
    printf("%-4s%-3s%-12s%12s  %12s%10lu  %s\n", t->nr, t->boot == 0x80 ? "*" : "",
        typ, anf, end, t->size / 2, z);
}

/* Zeile darunter: LBA (bzw. Kopfpaar) und was der Teil selbst sagt */
static void einzeln(struct teil *t)
{
    char z[100];

    if (t->halb)
        sprintf(z, "      Kopf %d-%d, %lu Sektoren", 2 * t->halb - 2, 2 * t->halb - 1,
            t->size);
    else
        sprintf(z, "      LBA %lu-%lu", t->start, t->start + t->size - 1);
    switch (t->art) {
    case ART_FAT:
        if (bpb_lesen(t) == 0)
            sprintf(z + strlen(z), ", OEM \"%.8s\", %u FAT%s zu %u, %u Einträge",
                bs + 3, nfat, nfat == 1 ? "" : "s", fatsz, rootents);
        break;
    case ART_RWBW:
        if (t->size % RW_SLICE)
            sprintf(z + strlen(z), ", dahinter %lu Sektoren frei", t->size % RW_SLICE);
        break;
    case ART_DRCPM:
        if (t->ok)
            sprintf(z + strlen(z), ", Kopfblock: OFF %u, DSM %u, DRM %u",
                t->off, t->dsm, t->drm);
        break;
    }
    printf("%s\n", z);
}

static void uebersicht(int nr, const char *dev)
{
    int i, r, aktiv = 0;
    struct teil *t;

    if (platte_lesen(dev, nr - 1) < 0) {
        printf("Platte %d (%s): nicht vorhanden\n", nr, dev);
        return;
    }
    geo_zeilen(nr, dev);
    geometrie();
    r = teile_suchen();
    if (r < 0) {
        close(fd);
        return;
    }
    if (r >= 10) {
        printf("  Bootsatz: ohne 55AA - NCR-Originalformat (MS-DOS 2.11), %s\n",
            r == 11 ? "eine Partition" : "2 x 5 MB");
        if (geo_ok && (heads != 4 || spt != 17))
            warn("DMVGEOM1 nennt %u/%u Köpfe/Sektoren, das Originalformat hat 4/17\n",
                heads, spt);
        if (e_cyls)
            printf("  belegt: %u/4/17 (erschlossen), %lu KB\n", e_cyls,
                (ulong)e_cyls * 34);
    } else {
        rd(0, buf);
        if (memcmp(buf + 3, "16BIT  ", 7)) {
            printf("  Bootsatz: 55AA%s%s\n", leer[0] ? ", leer: " : "", leer);
            err("ohne Kennung \"16BIT  \" ab Byte 3 startet das Z80-ROM nicht\n");
        } else
            printf("  Bootsatz: 16BIT, 55AA%s%s\n", leer[0] ? ", leer: " : "", leer);
    }
    if (!nteile) {
        if (r < 10)
            warn("keine Partition eingetragen\n");
        close(fd);
        return;
    }
    printf("\n");
    seite(kopf_struktur);
    for (i = 0; i < nteile; i++) {
        t = &teile[i];
        zeile(t);
        einzeln(t);
        if (t->boot == 0x80)
            aktiv++;
        if (r < 10 && t->nr[0] >= '1' && t->nr[0] <= '4' && !t->nr[1])
            eintrag_pruefen(t);
        if (t->art == ART_FAT && bpb_lesen(t) == 0) {
            fat_befunde(t);
            if (verbose)
                fat_einzeln(t);
        }
        if (t->art == ART_DRCPM)
            dr_kopf(t, 1);
        map_hp = 0;
    }
    if (r < 10 && aktiv != 1)
        warn("%d Einträge als startbar markiert (erwartet 1)\n", aktiv);
    close(fd);
}

/* ------------------------------------------------------- dmvfs -- */
static void fs_pruefen(int nr, const char *dev, const char *welcher)
{
    int i, gefunden = 0;
    struct teil *t;

    if (platte_lesen(dev, nr - 1) < 0) {
        if (welcher)
            printf("Platte %d (%s): nicht vorhanden\n", nr, dev);
        return;
    }
    still = 1;
    if (teile_suchen() < 0) {
        still = 0;
        close(fd);
        return;
    }
    for (i = 0; i < nteile; i++) {
        t = &teile[i];
        if (welcher && strcmp(welcher, t->nr))
            continue;
        gefunden = 1;
        if (t->art == ART_FAT) {
            still = 1;
            if (bpb_lesen(t)) {
                still = 0;
                printf("\nPlatte %d, Nr %s: kein brauchbarer BPB, nicht geprüft\n", nr, t->nr);
                continue;
            }
            still = 0;
            printf("\nPlatte %d, Nr %s: %s, %lu Cluster zu %u Sektoren\n", nr, t->nr,
                fat16 ? "FAT16" : "FAT12", ncl, spc);
            fatpruefung(0);
        } else if (t->art == ART_CPM) {
            still = 0;
            halbe(t);
            printf("\nPlatte %d, Nr %s: CP/M, Blöcke zu 8 KB\n", nr, t->nr);
            cpm_half();
        } else if (t->art == ART_RWBW) {
            still = 0;
            printf("\nPlatte %d, Nr %s: RomWBW, %u Scheiben zu 8 MB\n", nr, t->nr,
                scheiben(t));
            rw_pruefen(t);
        } else if (t->art == ART_DRCPM) {
            still = 1;
            dr_kopf(t, 0);
            still = 0;
            if (!t->ok)
                printf("\nPlatte %d, Nr %s: ohne DR-Kopfblock, nicht geprüft\n", nr, t->nr);
            else {
                printf("\nPlatte %d, Nr %s: CP/M (DR), Blöcke zu %u KB\n", nr, t->nr,
                    t->spb / 2);
                cpm_pruefen(t->dir0, t->spb, t->dsm, t->drm);
            }
        } else if (welcher) {
            still = 0;
            printf("\nPlatte %d, Nr %s: kein FAT- oder CP/M-Dateisystem\n", nr, t->nr);
        }
        map_hp = 0;
        still = 1;
    }
    still = 0;
    if (welcher && !gefunden)
        printf("Platte %d: keine Nr %s (siehe dmvhd)\n", nr, welcher);
    close(fd);
}

/* --------------------------------------------------------- Hilfe -- */
static const char erklaerung[] =
"Hilfe 1/2: Geräteseite und Kürzel\n"
"\n"
"HD 1-3          /dev/cfa, cfb, cfc: Einheit 0-2 am WD1001 (K012)\n"
"Größe,          ganze Platte nach dem DMVGEOM1-Block. Mit ? (NCR-Original-\n"
"Geometrie       format) aus den Partitionen erschlossen, nur der belegte\n"
"                Teil: CONFIG schreibt die Plattengröße nirgends hin.\n"
"Aufteilung      Nr und Kürzel jedes Teils, * = startbar:\n"
"  FAT12 FAT16   DOS-Partition, Typ 01 bzw. 04 (bis 32 MB)\n"
"  FAT2G         Typ 06, FAT16 über 32 MB: kennt erst DR DOS, MS-DOS 3.3\n"
"                sieht die Partition nicht\n"
"  EXTND         erweiterte Partition (Typ 05), logische Laufwerke ab Nr 5\n"
"  RWBWn         CP/M: RomWBW-Behälter (Typ 2E) mit n Scheiben zu 8 MB,\n"
"                lesbar von jeder CP/M-Fassung\n"
"  CPMnn         CP/M-86 nach DRs Muster (Typ DB), nn MB, mit Kopfblock\n"
"  DOS05 CPM05   NCR-Originalformat 2 x 5 MB, Hälfte A Kopf 0-1, B Kopf 2-3\n"
"  DOS10 DOS20   NCR-Originalformat, eine DOS-Partition über 4 Köpfe\n"
"  ?52           anderer Typ (hier 52), weder DOS noch CP/M der DMV\n"
"Befunde         F Fehler: so liest kein DMV-System die Platte richtig\n"
"                W Warnung: ungewöhnlich, kann gewollt sein\n"
"FDISK von MS-DOS 3.3 zeigt Typ 2E und DB als \"Non-DOS\" und löscht sie\n"
"nicht. DR-FDISK nennt DB \"CP/M\" und löscht es; 2E und 52 heißen dort\n"
"\"(other)\" und lassen sich nur mit FDISK /D löschen.\n"
"\n"
"Hilfe 2/2: Strukturseite, Dateisysteme, Tasten\n"
"\n"
"DMVGEOM1        Geometrieblock der DMV-Ports (Zylinder 0, Kopf 0, Sektor-\n"
"                ID 1, Kopie auf ID 2). Ohne ihn gilt die Einheit für\n"
"                MS-DOS 3.3 und DR DOS als nicht vorhanden.\n"
"Bootsatz        Sektor 0: 16BIT ab Byte 3 braucht das Z80-ROM zum Start,\n"
"                55AA kennzeichnet die Tabelle. Eine einzelne Partition legt\n"
"                DR-FDISK in Eintrag 4.\n"
"Anfang, Ende    Zylinder/Kopf/Sektor. Darunter LBA (bzw. Kopfpaar) und\n"
"                was der Teil selbst sagt: OEM und FATs aus dem Bootsektor\n"
"                (MS-DOS 3.3 braucht \"IBM  3.3\", im Originalformat ist\n"
"                \"16BIT\" richtig), Scheiben, DR-Kopfblock.\n"
"Dateisysteme    FAT: Kopien, Cluster, Ketten, Verzeichnisse, verlorene\n"
"                Cluster. CP/M: Verzeichnis und Blöcke je Hälfte, je\n"
"                RomWBW-Scheibe (4-KB-Blöcke) oder DR-Bereich (DPB aus dem\n"
"                Kopfblock).\n"
"Tasten          G Geräte, 1 2 3 Struktur, D Dateisysteme der Platte,\n"
"                L Diskettenlaufwerke (Schritttest), H Hilfe, Q Ende;\n"
"                bei \"weiter mit Eingabetaste\" bricht Q nur die Seite ab\n"
"Ohne Menü       dmvhd -n [-v] [-e], dmvfs [-v] [Platte [Nr]], dmvhd -s\n"
"Alles wird nur gelesen; dmvhd und dmvfs schreiben nie auf eine Platte.\n"
"Der Schritttest bewegt nur die Köpfe der Diskettenlaufwerke.\n";

/* ------------------------------------------------------- main -- */
static const char *geraet(const char *s)
{
    static char b[20];

    if (s[0] == '/' || strchr(s, '.'))
        return s;               /* Pfad, auch Abbilddatei zum Testen */
    snprintf(b, sizeof(b), "/dev/%s", s);
    return b;
}

static const char *devs[3] = { "/dev/cfa", "/dev/cfb", "/dev/cfc" };
static int ndev;

/* Platte 1-3 nach dem Geraet; Abbilddateien zaehlen der Reihe nach */
static int pnr(int i)
{
    const char *d = devs[i];

    return !strcmp(d, "/dev/cfa") ? 1 : !strcmp(d, "/dev/cfb") ? 2 :
        !strcmp(d, "/dev/cfc") ? 3 : i + 1;
}

/* einmal ausgeben (-n, dmvfs, Umleitung): u Uebersicht, v Einzelheiten,
   d Dateisysteme */
static void ausgabe(int art, const char *nr)
{
    int i;

    if (art != 'd')
        verbose = art == 'v';   /* bei d: -v zeigt die Dateien */
    if (art == 'd')
        printf("Dateisysteme prüfen (dmvfs), nur lesend\n");
    else
        printf("dmvhd - Plattendiagnose NCR DMV, nur lesend\n");
    for (i = 0; i < ndev && !abbruch; i++) {
        if (art == 'd')
            fs_pruefen(pnr(i), devs[i], nr);
        else {
            printf("\n");
            uebersicht(pnr(i), devs[i]);
        }
    }
    printf("\n%d Fehler, %d %s\n", fehler, warnungen, pl(warnungen, "Warnung", "Warnungen"));
}

/* ------------------------------------------------------- Menue -- */
/* Am Bildschirm: Zeilen 1-24 rollen, Zeile 25 ist die Statuszeile mit
   den Tasten (ESC [ 1 ; 24 r der DMV-Konsole); Tastenbuchstaben
   hervorgehoben (ESC [ 1 m: gelb, auf Mono invers). */
#define HELL    "\033[1m"
#define NORMAL  "\033[0m"

/* Statuszeile; was in {} steht, wird hervorgehoben (die Tasten) */
static void status(const char *t)
{
    fflush(stdout);
    fputs("\033[25;1H\033[K", stdout);
    for (; *t; t++)
        if (*t == '{')
            fputs(HELL, stdout);
        else if (*t == '}')
            fputs(NORMAL, stdout);
        else
            putchar(*t);
    fflush(stdout);
}

/* ------------------------------------------------- Geraeteseite -- */
static const char kopf_geraete[] =
"HD  Gerät      Größe     Geometrie   Aufteilung (Nr Inhalt)        Befunde\n"
"==============================================================================\n";

/* eine Platte: Groesse, Geometrie, Teile zu dritt je Zeile, Befunde */
static void kurz(int i, int dicht)
{
    int k, n = 0;
    char z[160], e[16], groesse[16], geo[16], bef[16];

    if (platte_lesen(devs[i], pnr(i) - 1) < 0) {
        printf("%-4d%-11s%s\n", pnr(i), "-", "nicht vorhanden");
        if (!dicht)
            printf("\n");
        return;
    }
    close(fd);
    fehler = warnungen = 0;
    stumm = 1;                  /* nur zaehlen */
    uebersicht(pnr(i), devs[i]);
    stumm = 0;
    if (geo_ok) {
        sprintf(groesse, "%lu KB", disksects / 2);
        sprintf(geo, "%u/%u/%u", cyls, heads, spt);
    } else if (e_cyls) {
        sprintf(groesse, "%lu KB", (ulong)e_cyls * 34);
        sprintf(geo, "%u/4/17?", e_cyls);
    } else {
        strcpy(groesse, "-");
        strcpy(geo, "?");
    }
    if (!fehler && !warnungen)
        strcpy(bef, "keine");
    else
        sprintf(bef, "%dF %dW", fehler, warnungen);
    sprintf(z, "%-4d%-11.10s%-10s%-12s", pnr(i), devs[i], groesse, geo);
    for (k = 0; k < nteile; k++) {
        struct teil *t = &teile[k];
        char kz[8];
        kuerzel(t, kz);
        sprintf(e, "%s %s%s", t->nr, kz, t->boot == 0x80 ? "*" : "");
        sprintf(z + strlen(z), "%-10s", e);
        if (++n % 3 == 0 || k == nteile - 1) {
            if (n <= 3) {           /* Befunde in die erste Zeile */
                while (strlen(z) < 67)
                    strcat(z, " ");
                strcat(z, bef);
            }
            while (strlen(z) && z[strlen(z) - 1] == ' ')
                z[strlen(z) - 1] = 0;
            printf("%s\n", z);
            sprintf(z, "%37s", "");
        }
    }
    if (!nteile) {
        strcat(z, original ? "nicht erkannt" : "keine Partition");
        while (strlen(z) < 67)
            strcat(z, " ");
        printf("%s%s\n", z, bef);
    }
    if (!dicht)
        printf("\n");
}

static void geraete(int dicht)
{
    int i;

    seite(kopf_geraete);
    for (i = 0; i < ndev; i++)
        kurz(i, dicht);
}

static const char fuss_geraete[] =
"Geometrie (Zyl/Kpf/Sek) aus dem DMVGEOM1-Block; mit ? aus den von CONFIG\n"
"angelegten Partitionen erschlossen (nur der belegte Teil der Platte).\n"
"Befunde: F Fehler, W Warnungen; * startbar. Die Kürzel erklärt die Hilfe.\n";

/* ------------------------------------------------ Startbildschirm -- */
/* Speicher: der Z80 zaehlt die 64-KB-Baenke ueber der Hauptplatine im
   Postfach 0:FE06 (MEM16). Ob die 8088 ueber 512 KB auch RAM sieht
   (K235+ mit K0815), zeigt eine Schreib-Lese-Probe je Bank ab 80000h;
   ohne RAM spiegelt sich dort das ROM der Karte. Der Speicher darueber
   gehoert nicht ELKS, die Probe stellt den alten Inhalt wieder her.
   Obergrenze 1020 KB: FF000h-FFFFFh haelt der Dekoder fuer das ROM frei. */
static void speicher(unsigned int *z80, unsigned int *cpu)
{
#ifdef __ia16__
    unsigned int bank, alt, kb = 512;
    volatile unsigned short __far *p;

    *z80 = *(volatile unsigned char __far *)0x0000FE06UL * 64 + 64;
    for (bank = 8; bank < 16; bank++) {     /* 80000h, 90000h ... F0000h */
        p = (volatile unsigned short __far *)((unsigned long)bank << 28);
        alt = *p;
        *p = 0x5AA5;
        if (*p != 0x5AA5) {
            *p = alt;
            break;
        }
        *p = 0xA55A;
        if (*p != 0xA55A) {
            *p = alt;
            break;
        }
        *p = alt;
        kb += bank == 15 ? 60 : 64;
    }
    *cpu = kb;
#else
    *z80 = *cpu = 512;
#endif
    if (*z80 > 1020)
        *z80 = 1020;
}

/* K235 hat einen 8259, dessen Maskenregister (91h) der Kern beschrieben
   hat; ohne Interruptcontroller (K230/K231) liest der Bus FFh */
static void startseite(void)
{
    int k235 = inb(0x91) != 0xFF;
    unsigned int z80, cpu;

#ifdef __ia16__
    sleep(5);                   /* Tux und die Startmeldungen stehen lassen */
#endif
    printf("\fNCR Decision Mate V - ELKS-Prüfsystem %s, nur lesend\n\n", DMV_VERSION);
    speicher(&z80, &cpu);
    if (cpu > 512)
        printf("Arbeitsspeicher  %u KB, davon nutzt ELKS 512 KB\n", cpu);
    else if (z80 > 512)
        printf("Arbeitsspeicher  %u KB, die 8088 sieht davon 512 KB\n", z80);
    else
        printf("Arbeitsspeicher  %u KB\n", z80);
    printf("CPU-Karte        %s\n", !k235 ? "K230 oder K231 (8088, ohne Interruptcontroller)" :
        cpu > 512 ? "K235+ (V20 mit 8259, sieht über 512 KB)" :
        "K235 (8088/V20 mit Interruptcontroller 8259)");
    printf("Disketten        Schritttest im Menü von dmvhd (Taste L)\n\n");
    geraete(1);
    printf("\nEinzelheiten, Strukturen und Dateisysteme: dmvhd\n");
}

/* ------------------------------------------ Diskettenlaufwerke -- */
/* Schritttest am uPD765 (50h Status, 51h Daten, 13h Bit 3 Interrupt,
   14h Motor): Spur 0 suchen, 44 Spuren hinein, 43 zurueck. Ein Laufwerk
   mit 40 Spuren (DD, 48 tpi) laeuft am Anschlag auf, kommt bei Spur 0 an
   und meldet sie; eines mit 80 Spuren (QD, 96 tpi) steht auf Spur 1. */
#define FDC_MSR     0x50
#define FDC_DAT     0x51
#define FDC_INT     0x13
#define FDC_MOTOR   0x14

static int fdc_aus(int b)
{
    long n;

    for (n = 0; n < 60000L; n++)
        if ((inb(FDC_MSR) & 0xC0) == 0x80) {
            outb(b, FDC_DAT);
            return 0;
        }
    return -1;
}

static int fdc_ein(void)
{
    long n;

    for (n = 0; n < 60000L; n++)
        if ((inb(FDC_MSR) & 0xC0) == 0xC0)
            return inb(FDC_DAT);
    return -1;
}

/* Befehl mit Kopfbewegung, dann SENSE INTERRUPT STATUS; ST0 oder -1 */
static int fdc_bewegen(int u, int befehl, int spur)
{
    long n;
    int st0;

    outb(0, FDC_MOTOR);         /* Motor nachtriggern */
    if (fdc_aus(befehl) || fdc_aus(u) || (befehl == 0x0F && fdc_aus(spur)))
        return -1;
    for (n = 0; n < 400000L; n++)
        if (inb(FDC_INT) & 0x08)
            break;
    if (n == 400000L || fdc_aus(0x08))
        return -1;
    st0 = fdc_ein();
    fdc_ein();                  /* PCN */
    return st0;
}

/* SENSE DRIVE STATUS: waehlt das Laufwerk und liefert ST3 (-1: keine
   Antwort). Bit 5 bereit, 4 Spur 0, 3 zweiseitig. */
static int fdc_st3(int u)
{
    outb(0, FDC_MOTOR);
    if (fdc_aus(0x04) || fdc_aus(u))
        return -1;
    return fdc_ein();
}

/* bis zu etwa 3 s warten, bis das Laufwerk bereit meldet. Bereit heisst:
   Motor laeuft und die Indexpulse einer eingelegten Diskette kommen; ohne
   Diskette meldet ein Laufwerk nie bereit, und der Controller steppt dann
   nicht. */
static int fdc_bereit(int u)
{
    int i, st3 = -1;
    unsigned int k;

    for (i = 0; i < 300; i++) {
        st3 = fdc_st3(u);
        if (st3 < 0 || (st3 & 0x20))
            return st3;
        for (k = 0; k < 2500; k++)
            inb(FDC_MSR);
    }
    return st3;
}

static void disketten(void)
{
    int u, st0, st3;

    printf("Diskettenlaufwerke: Schritttest\n\n");
    printf("Je Laufwerk: Spur 0 suchen, 44 Spuren hinein, 43 zurück. Mit 40\n");
    printf("Spuren läuft der Kopf am Anschlag auf und meldet danach Spur 0, mit\n");
    printf("80 Spuren steht er auf Spur 1. Im Laufwerk muss eine Diskette liegen\n");
    printf("(irgendeine): erst die drehende Diskette meldet das Laufwerk bereit.\n\n");
    for (u = 0; u < 2; u++) {
        printf("Laufwerk %c: ", 'A' + u);
        fflush(stdout);
        st3 = fdc_bereit(u);
        if (st3 < 0) {
            printf("keine Antwort vom Diskettencontroller\n");
            continue;
        }
        if (!(st3 & 0x20)) {
            printf("nicht bereit (keine Diskette oder kein Laufwerk)\n");
            continue;
        }
        st0 = fdc_bewegen(u, 0x07, 0);
        if (st0 >= 0 && (st0 & 0x10))           /* mehr als 77 Schritte */
            st0 = fdc_bewegen(u, 0x07, 0);
        if (st0 < 0 || (st0 & 0xC0)) {
            printf("Spur 0 nicht gefunden (ST0 %02X)\n", st0 & 0xFF);
            continue;
        }
        if (fdc_bewegen(u, 0x0F, 44) < 0 || fdc_bewegen(u, 0x0F, 1) < 0 ||
            (st3 = fdc_st3(u)) < 0) {
            printf("Schritttest abgebrochen\n");
            continue;
        }
        fdc_bewegen(u, 0x07, 0);
        printf("%s, %s\n", (st3 & 0x10) ? "40 Spuren (DD, 48 tpi)" : "80 Spuren (QD, 96 tpi)",
            (st3 & 0x08) ? "zweiseitig" : "einseitig gemeldet");
    }
    printf("\nGeprüft wird die Mechanik, nicht das Lesen. Ein Laufwerk, das\n");
    printf("über die 40. Spur hinaus fährt, gilt als QD.\n");
}

int main(int argc, char **argv)
{
    static char dv[3][64];
    int i, n = 0, fs = 0, nur = 0, art, k, wahl = 0, start = 0;
    const char *p = strrchr(argv[0], '/'), *nr = 0;

    if (!strcmp(p ? p + 1 : argv[0], "dmvfs"))
        fs = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-n"))
            seitenweise = 0;
        else if (!strcmp(argv[i], "-f"))
            fs = 1;
        else if (!strcmp(argv[i], "-e"))
            nur = 1;
        else if (!strcmp(argv[i], "-s"))
            start = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Aufruf: dmvhd [-n] [-v] [-e] [-s] [Platte ...]\n"
                "        dmvfs [-v] [-n] [Platte [Nr]]\n"
                "  -n ohne Menü, -v Einzelheiten, -e Hilfe, -s Startbildschirm\n");
            return 2;
        } else if (fs && n == 1 && !nr && strlen(argv[i]) <= 2)
            nr = argv[i];       /* Nr aus der Tabelle: 1-4, 5.., -, A, B */
        else if (n < 3) {
            strncpy(dv[n], geraet(argv[i]), sizeof(dv[n]) - 1);
            dv[n][sizeof(dv[n]) - 1] = 0;
            devs[n] = dv[n];
            n++;
        }
    }
    if (!isatty(1) || !isatty(0))
        seitenweise = 0;
    ndev = n ? n : 3;
    if (start) {
        seitenweise = 0;
        startseite();
        fflush(stdout);
        return 0;
    }
    if (!seitenweise || nur || fs || nr) {      /* einmal ausgeben */
        if (!n) {               /* Vorgabe: nur die vorhandenen Platten */
            for (i = k = 0; i < 3; i++)
                if ((fd = open(devs[i], O_RDONLY)) >= 0) {
                    close(fd);
                    devs[k++] = devs[i];
                }
            ndev = k;
        }
        if (nur)
            seite(erklaerung);
        else
            ausgabe(fs ? 'd' : verbose ? 'v' : 'u', nr);
        fflush(stdout);
        return fehler ? 1 : 0;
    }

    /* Menue: g Geraete, 1-3 Struktur, d Dateisysteme, l Disketten,
       h Hilfe, q Ende */
    menue = 1;
    printf("\033[1;24r");
    art = 'g';
    for (;;) {
        abbruch = 0;
        fehler = warnungen = 0;
        printf("\f");
        if (art == 'g') {
            printf("dmvhd - Plattendiagnose NCR DMV, nur lesend\n\n");
            geraete(0);
            seite(fuss_geraete);
        } else if (art == 'h')
            seite(erklaerung);
        else if (art == 'l')
            disketten();
        else {
            for (i = 0; i < ndev && pnr(i) != wahl; i++)
                ;
            if (i == ndev)
                printf("Platte %d gibt es nicht.\n", wahl);
            else if (art == 'd') {
                verbose = 0;
                printf("Platte %d: Dateisysteme prüfen\n", wahl);
                fs_pruefen(wahl, devs[i], 0);
                printf("\n%d Fehler, %d %s\n", fehler, warnungen,
                    pl(warnungen, "Warnung", "Warnungen"));
            } else {
                verbose = 0;
                uebersicht(wahl, devs[i]);
                printf("\n%d Fehler, %d %s\n", fehler, warnungen,
                    pl(warnungen, "Warnung", "Warnungen"));
            }
        }
        if (!wahl)
            status("{1} {2} {3} Struktur der Platte   {L} Disketten   {H} Hilfe   {Q} Ende");
        else {
            char z[100];
            sprintf(z, "{G} Geräte   {D} Dateisysteme Platte %d   {1} {2} {3} Struktur   "
                "{H} Hilfe   {Q} Ende", wahl);
            status(z);
        }
        do
            k = taste();
        while (!strchr(wahl ? "gdhl123q" : "hl123q", k) || !k);
        if (k == 'q')
            break;
        if (k >= '1' && k <= '3') {
            wahl = k - '0';
            art = 's';
        } else {
            if (k == 'g')
                wahl = 0;
            art = k;
        }
    }
    printf("\033[r\f");
    fflush(stdout);
    return 0;
}
