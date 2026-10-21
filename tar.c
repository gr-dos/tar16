/*
 * tar - file archiver for 16-bit MS-DOS (Open Watcom C)
 *
 * Implements the POSIX/SUS tar utility:
 *
 *     tar -c|-r|-t|-u|-x [-bfvwlmo] [blocksize] [archive] [file...]
 *
 * The key may be given with or without a leading '-'.  Arguments for
 * the 'b' and 'f' modifiers follow the key in the order the modifier
 * letters appear.  Archives are written in POSIX.1 ustar format.  On
 * input, ustar, old V7 headers and POSIX.1-2001 pax extended headers
 * (path, linkpath, size, mtime) are understood.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>
#include <dos.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h>

#define BLOCK     512
#define MAXBF     20
#define PATHMAX   260
#define NHASH     256

struct hdr {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

struct ent {
    char          name[PATHMAX];
    char          link[PATHMAX];
    long          size;
    long          mtime;
    unsigned      mode;
    unsigned long uid;
    unsigned long gid;
    char          uname[33];
    char          gname[33];
    char          type;
};

/* entries already in the archive, for the 'u' function */
struct seen {
    struct seen *next;
    long         mtime;
    char         name[1];
};

static char         func;
static int          verbose, confirm, nomtime;
static char        *archive = "-";
static int          bfactor = MAXBF;
static int          afd = -1;
static FILE        *msg;            /* verbose output stream */
static FILE        *tty;            /* for 'w' confirmation */
static char         afull[PATHMAX]; /* full path of archive file */

static char        *rec;
static unsigned     recsize;
static unsigned     recpos;
static unsigned     reclen;
static long         apos;           /* archive offset of next block */

static int          errors;
static char       **operands;
static int          noperands;
static char        *found;
static struct seen *seentab[NHASH];

/* pax extended header state */
static char         gx_path[PATHMAX], gx_link[PATHMAX];
static long         gx_size, gx_mtime;
static int          gx_flags;
static char         xx_path[PATHMAX], xx_link[PATHMAX];
static long         xx_size, xx_mtime;
static int          xx_flags;
#define PX_PATH   1
#define PX_LINK   2
#define PX_SIZE   4
#define PX_MTIME  8

static void usage(void)
{
    fprintf(stderr,
        "usage: tar -c[vwbfmo] [blocksize] [archive] file...\n"
        "       tar -r[vwbf] [blocksize] [archive] file...\n"
        "       tar -t[vbf] [blocksize] [archive] [file...]\n"
        "       tar -u[vwbf] [blocksize] [archive] file...\n"
        "       tar -x[vwbflmo] [blocksize] [archive] [file...]\n");
    exit(2);
}

static void warn(const char *s1, const char *s2)
{
    if (s2 != NULL)
        fprintf(stderr, "tar: %s: %s\n", s1, s2);
    else
        fprintf(stderr, "tar: %s\n", s1);
    errors = 1;
}

static void fatal(const char *s1, const char *s2)
{
    warn(s1, s2);
    exit(2);
}

/* ---------------------------------------------------------------- */
/* archive block I/O                                                */

/* read the next 512-byte block, NULL at end of archive */
static char *rdblock(void)
{
    char *p;
    int   n;

    if (recpos >= reclen) {
        reclen = 0;
        recpos = 0;
        while (reclen < recsize) {
            n = read(afd, rec + reclen, recsize - reclen);
            if (n < 0)
                fatal(archive, strerror(errno));
            if (n == 0)
                break;
            reclen += n;
        }
        if (reclen == 0)
            return NULL;
        if (reclen % BLOCK != 0)
            fatal(archive, "archive ends in a partial block");
    }
    p = rec + recpos;
    recpos += BLOCK;
    apos += BLOCK;
    return p;
}

static void wrrecord(void)
{
    unsigned off = 0;
    int      n;

    while (off < recsize) {
        n = write(afd, rec + off, recsize - off);
        if (n <= 0)
            fatal(archive, n < 0 ? strerror(errno) : "write error");
        off += n;
    }
    recpos = 0;
}

/* get a zeroed block in the output record */
static char *wrblock(void)
{
    char *p;

    if (recpos >= recsize)
        wrrecord();
    p = rec + recpos;
    memset(p, 0, BLOCK);
    recpos += BLOCK;
    return p;
}

/* write the end-of-archive marker and pad out the last record */
static void wrend(void)
{
    wrblock();
    wrblock();
    if (recpos > 0) {
        memset(rec + recpos, 0, recsize - recpos);
        wrrecord();
    }
}

/* ---------------------------------------------------------------- */
/* name handling                                                    */

/* convert a host path to an archive member name */
static void tarname(char *dst, const char *src)
{
    if (isalpha((unsigned char)src[0]) && src[1] == ':')
        src += 2;
    for (; *src != '\0'; src++, dst++)
        *dst = (*src == '\\') ? '/' : tolower((unsigned char)*src);
    *dst = '\0';
}

/* convert an archive member name to a host path */
static void hostname(char *dst, const char *src)
{
    char *start = dst;

    for (; *src != '\0'; src++, dst++)
        *dst = (*src == '/') ? '\\' : *src;
    *dst = '\0';
    while (dst > start + 1 && dst[-1] == '\\' && dst[-2] != ':')
        *--dst = '\0';
}

static unsigned hash(const char *s)
{
    unsigned h = 0;

    while (*s != '\0')
        h = h * 31 + tolower((unsigned char)*s++);
    return h % NHASH;
}

/* compare names ignoring case and a trailing slash */
static int samename(const char *a, const char *b)
{
    while (*a != '\0' && tolower((unsigned char)*a) ==
                         tolower((unsigned char)*b))
        a++, b++;
    if (*a == '/' && a[1] == '\0')
        a++;
    if (*b == '/' && b[1] == '\0')
        b++;
    return *a == '\0' && *b == '\0';
}

/* does an archive member name match the operands? */
static int selected(const char *name)
{
    int i;
    size_t n;

    if (noperands == 0)
        return 1;
    for (i = 0; i < noperands; i++) {
        n = strlen(operands[i]);
        if (samename(name, operands[i]) ||
            (strnicmp(name, operands[i], n) == 0 && name[n] == '/')) {
            found[i] = 1;
            return 1;
        }
    }
    return 0;
}

static int ask(const char *action, const char *name)
{
    char buf[80];

    if (!confirm)
        return 1;
    if (tty == NULL && (tty = fopen("CON", "r")) == NULL)
        fatal("CON", strerror(errno));
    fprintf(stderr, "%s %s? ", action, name);
    fflush(stderr);
    if (fgets(buf, sizeof buf, tty) == NULL)
        return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

/* ---------------------------------------------------------------- */
/* header encoding and decoding                                     */

static unsigned long getoct(const char *p, int n)
{
    unsigned long v = 0;

    while (n > 0 && (*p == ' ' || *p == '\0'))
        p++, n--;
    while (n > 0 && *p >= '0' && *p <= '7') {
        v = (v << 3) + (*p - '0');
        p++, n--;
    }
    return v;
}

static void putoct(char *p, int n, unsigned long v)
{
    p[--n] = '\0';
    while (n > 0) {
        p[--n] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

static void getstr(char *dst, const char *src, int n)
{
    while (n-- > 0 && *src != '\0')
        *dst++ = *src++;
    *dst = '\0';
}

static int chksum(const struct hdr *h, long *ssum)
{
    const unsigned char *p = (const unsigned char *)h;
    long u = 0, s = 0;
    int  i;

    for (i = 0; i < BLOCK; i++) {
        if (i >= 148 && i < 156) {
            u += ' ';
            s += ' ';
        } else {
            u += p[i];
            s += (signed char)p[i];
        }
    }
    if (ssum != NULL)
        *ssum = s;
    return (int)u;
}

/* split a name into the ustar prefix and name fields */
static int putname(struct hdr *h, const char *name)
{
    size_t len = strlen(name);
    size_t i;

    if (len <= 100) {
        memcpy(h->name, name, len);
        return 0;
    }
    if (len > 256)
        return -1;
    for (i = len - 1; i > 0; i--) {
        if (name[i] == '/' && i <= 155 && len - i - 1 <= 100 &&
            len - i - 1 > 0) {
            memcpy(h->prefix, name, i);
            memcpy(h->name, name + i + 1, len - i - 1);
            return 0;
        }
    }
    return -1;
}

/* can the name be stored in a ustar header? */
static int namefits(const char *name)
{
    struct hdr *h = (struct hdr *)(rec + recsize);   /* scratch block */

    memset(h, 0, BLOCK);
    return putname(h, name) == 0;
}

static long parsenum(const char *s)
{
    long v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* parse pax extended header records */
static void paxparse(char *buf, long len, int global)
{
    char *p = buf, *end = buf + len, *kw, *val, *eq, *rec_end;
    long  rlen;
    char *path = global ? gx_path : xx_path;
    char *link = global ? gx_link : xx_link;
    int  *flags = global ? &gx_flags : &xx_flags;

    while (p < end) {
        rlen = parsenum(p);
        rec_end = p + rlen;
        if (rlen <= 0 || rec_end > end || rec_end[-1] != '\n') {
            warn(archive, "malformed extended header");
            return;
        }
        rec_end[-1] = '\0';
        kw = strchr(p, ' ');
        if (kw == NULL || (eq = strchr(++kw, '=')) == NULL) {
            p = rec_end;
            continue;
        }
        *eq = '\0';
        val = eq + 1;
        if (strcmp(kw, "path") == 0) {
            if (strlen(val) < PATHMAX) {
                strcpy(path, val);
                *flags |= PX_PATH;
            } else
                warn(val, "path name too long");
        } else if (strcmp(kw, "linkpath") == 0) {
            if (strlen(val) < PATHMAX) {
                strcpy(link, val);
                *flags |= PX_LINK;
            }
        } else if (strcmp(kw, "size") == 0) {
            if (global)
                gx_size = parsenum(val);
            else
                xx_size = parsenum(val);
            *flags |= PX_SIZE;
        } else if (strcmp(kw, "mtime") == 0) {
            if (global)
                gx_mtime = parsenum(val);
            else
                xx_mtime = parsenum(val);
            *flags |= PX_MTIME;
        }
        p = rec_end;
    }
}

static void skipdata(long size)
{
    long n;

    for (n = (size + BLOCK - 1) / BLOCK; n > 0; n--)
        if (rdblock() == NULL)
            fatal(archive, "unexpected end of archive");
}

static void readpax(long size, int global)
{
    char *buf, *b;
    long  off;
    unsigned n;

    if (size > 16384L) {
        warn(archive, "extended header too large; ignored");
        skipdata(size);
        return;
    }
    if ((buf = malloc((size_t)size + 1)) == NULL)
        fatal("out of memory", NULL);
    for (off = 0; off < size; off += BLOCK) {
        if ((b = rdblock()) == NULL)
            fatal(archive, "unexpected end of archive");
        n = (size - off > BLOCK) ? BLOCK : (unsigned)(size - off);
        memcpy(buf + off, b, n);
    }
    buf[size] = '\0';
    paxparse(buf, size, global);
    free(buf);
}

/*
 * Read the next member header.  Returns 1 with *e filled in, or 0 at
 * the end of the archive.  *where receives the archive offset of the
 * header block (of the end marker at end of archive).
 */
static int nextent(struct ent *e, long *where)
{
    struct hdr *h;
    long        ssum;
    int         sum;
    size_t      len;

    for (;;) {
        *where = apos;
        if ((h = (struct hdr *)rdblock()) == NULL)
            return 0;
        if (h->name[0] == '\0') {
            char *p = (char *)h;
            int   i;

            for (i = 0; i < BLOCK && p[i] == '\0'; i++)
                ;
            if (i == BLOCK)
                return 0;
        }
        sum = (int)getoct(h->chksum, 8);
        if (sum != chksum(h, &ssum) && (long)sum != ssum)
            fatal(archive, "directory checksum error");

        e->type = h->typeflag;
        e->size = (long)getoct(h->size, 12);

        if (e->type == 'x' || e->type == 'g') {
            readpax(e->size, e->type == 'g');
            continue;
        }

        e->name[0] = '\0';
        if (memcmp(h->magic, "ustar", 5) == 0 && h->prefix[0] != '\0') {
            getstr(e->name, h->prefix, 155);
            strcat(e->name, "/");
        }
        getstr(e->name + strlen(e->name), h->name, 100);
        getstr(e->link, h->linkname, 100);
        e->mode = (unsigned)getoct(h->mode, 8) & 07777;
        e->uid = getoct(h->uid, 8);
        e->gid = getoct(h->gid, 8);
        e->mtime = (long)getoct(h->mtime, 12);
        if (memcmp(h->magic, "ustar", 5) == 0) {
            getstr(e->uname, h->uname, 32);
            getstr(e->gname, h->gname, 32);
        } else {
            e->uname[0] = e->gname[0] = '\0';
            /* old archives mark directories with a trailing slash */
            len = strlen(e->name);
            if ((e->type == '0' || e->type == '\0') && len > 0 &&
                e->name[len - 1] == '/')
                e->type = '5';
        }

        if (gx_flags & PX_PATH)  strcpy(e->name, gx_path);
        if (gx_flags & PX_LINK)  strcpy(e->link, gx_link);
        if (gx_flags & PX_SIZE)  e->size = gx_size;
        if (gx_flags & PX_MTIME) e->mtime = gx_mtime;
        if (xx_flags & PX_PATH)  strcpy(e->name, xx_path);
        if (xx_flags & PX_LINK)  strcpy(e->link, xx_link);
        if (xx_flags & PX_SIZE)  e->size = xx_size;
        if (xx_flags & PX_MTIME) e->mtime = xx_mtime;
        xx_flags = 0;

        /* links and directories carry no data in the archive */
        if (e->type == '1' || e->type == '2' || e->type == '5')
            e->size = 0;
        return 1;
    }
}

/* ---------------------------------------------------------------- */
/* t: list                                                          */

static void modestr(char *s, const struct ent *e)
{
    static const char rwx[] = "rwxrwxrwx";
    int i;

    switch (e->type) {
    case '5': s[0] = 'd'; break;
    case '2': s[0] = 'l'; break;
    case '3': s[0] = 'c'; break;
    case '4': s[0] = 'b'; break;
    case '6': s[0] = 'p'; break;
    default:  s[0] = '-'; break;
    }
    for (i = 0; i < 9; i++)
        s[i + 1] = (e->mode & (0400 >> i)) ? rwx[i] : '-';
    if (e->mode & 04000)
        s[3] = (e->mode & 0100) ? 's' : 'S';
    if (e->mode & 02000)
        s[6] = (e->mode & 010) ? 's' : 'S';
    if (e->mode & 01000)
        s[9] = (e->mode & 01) ? 't' : 'T';
    s[10] = '\0';
}

static void listent(const struct ent *e)
{
    char       mode[11], owner[80], date[20];
    time_t     t;
    struct tm *tm;

    if (!verbose) {
        printf("%s\n", e->name);
        return;
    }
    modestr(mode, e);
    if (e->uname[0] != '\0')
        sprintf(owner, "%s/%s", e->uname,
                e->gname[0] != '\0' ? e->gname : "-");
    else
        sprintf(owner, "%lu/%lu", e->uid, e->gid);
    t = (time_t)e->mtime;
    if ((tm = localtime(&t)) != NULL)
        strftime(date, sizeof date, "%b %d %H:%M %Y", tm);
    else
        strcpy(date, "?");
    printf("%s %s %8ld %s %s", mode, owner, e->size, date, e->name);
    if (e->type == '1')
        printf(" link to %s", e->link);
    else if (e->type == '2')
        printf(" -> %s", e->link);
    printf("\n");
}

/* ---------------------------------------------------------------- */
/* x: extract                                                       */

static void mkparents(char *path)
{
    char *p;

    for (p = path; *p != '\0'; p++) {
        if (*p == '\\' && p > path && p[-1] != ':' && p[-1] != '\\') {
            *p = '\0';
            mkdir(path);
            *p = '\\';
        }
    }
}

static void setattrs(const char *path, const struct ent *e)
{
    struct utimbuf ut;

    if (!nomtime) {
        ut.actime = ut.modtime = (time_t)e->mtime;
        if (utime(path, &ut) != 0)
            warn(path, strerror(errno));
    }
    if (!(e->mode & 0200))
        chmod(path, S_IRUSR);
}

static int openout(const char *path)
{
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
              S_IRUSR | S_IWUSR);
    if (fd < 0 && errno == EACCES) {
        /* an existing read-only file is replaced */
        chmod(path, S_IRUSR | S_IWUSR);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                  S_IRUSR | S_IWUSR);
    }
    return fd;
}

static void copylink(const char *path, const struct ent *e)
{
    char  src[PATHMAX];
    int   in, out, n;
    char *buf = rec + recsize;  /* scratch space after the record */

    hostname(src, e->link);
    if ((in = open(src, O_RDONLY | O_BINARY)) < 0) {
        warn(e->link, strerror(errno));
        return;
    }
    if ((out = openout(path)) < 0) {
        warn(path, strerror(errno));
        close(in);
        return;
    }
    while ((n = read(in, buf, BLOCK)) > 0)
        if (write(out, buf, n) != n) {
            warn(path, strerror(errno));
            break;
        }
    close(in);
    close(out);
    setattrs(path, e);
}

static void extract(const struct ent *e)
{
    char        path[PATHMAX];
    char       *b;
    long        left;
    unsigned    n;
    int         fd;
    struct stat st;

    hostname(path, e->name);
    if (path[0] == '\0') {
        skipdata(e->size);
        return;
    }
    if (!ask("x", e->name)) {
        skipdata(e->size);
        return;
    }
    mkparents(path);

    switch (e->type) {
    case '5':
        if (mkdir(path) != 0 &&
            (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)))
            warn(path, strerror(errno));
        else if (verbose)
            fprintf(msg, "x %s\n", e->name);
        return;
    case '1':
        if (verbose)
            fprintf(msg, "x %s link to %s\n", e->name, e->link);
        copylink(path, e);
        return;
    case '2':
        warn(e->name, "cannot create symbolic link");
        return;
    case '3':
    case '4':
    case '6':
        warn(e->name, "cannot create special file");
        skipdata(e->size);
        return;
    }

    /* regular file; unknown types are treated as regular files */
    if ((fd = openout(path)) < 0) {
        warn(path, strerror(errno));
        skipdata(e->size);
        return;
    }
    if (verbose)
        fprintf(msg, "x %s, %ld bytes\n", e->name, e->size);
    for (left = e->size; left > 0; left -= n) {
        if ((b = rdblock()) == NULL)
            fatal(archive, "unexpected end of archive");
        n = left > BLOCK ? BLOCK : (unsigned)left;
        if (fd >= 0 && write(fd, b, n) != (int)n) {
            warn(path, errno ? strerror(errno) : "disk full");
            close(fd);
            fd = -1;
        }
    }
    if (fd >= 0) {
        close(fd);
        setattrs(path, e);
    }
}

/* ---------------------------------------------------------------- */
/* c, r, u: write                                                   */

static void remember(const char *name, long mtime)
{
    struct seen *s;
    unsigned     h = hash(name);

    for (s = seentab[h]; s != NULL; s = s->next)
        if (samename(s->name, name)) {
            if (mtime > s->mtime)
                s->mtime = mtime;
            return;
        }
    if ((s = malloc(sizeof *s + strlen(name))) == NULL)
        fatal("out of memory", NULL);
    strcpy(s->name, name);
    s->mtime = mtime;
    s->next = seentab[h];
    seentab[h] = s;
}

/* for 'u': is the archive copy at least as new as the file? */
static int uptodate(const char *name, long mtime)
{
    struct seen *s;

    for (s = seentab[hash(name)]; s != NULL; s = s->next)
        if (samename(s->name, name))
            return s->mtime >= mtime;
    return 0;
}

static void writehdr(const char *name, const struct stat *st, char type,
                     long size)
{
    struct hdr *h = (struct hdr *)wrblock();

    putname(h, name);
    /* DOS reports every file as world-writable; apply a umask of 022 */
    putoct(h->mode, 8, st->st_mode & 07755);
    putoct(h->uid, 8, 0);
    putoct(h->gid, 8, 0);
    putoct(h->size, 12, size);
    putoct(h->mtime, 12, (unsigned long)st->st_mtime);
    h->typeflag = type;
    memcpy(h->magic, "ustar", 6);
    memcpy(h->version, "00", 2);
    putoct(h->chksum, 7, chksum(h, NULL));
    h->chksum[7] = ' ';
}

static void addpath(char *path);

static void adddir(char *path, const char *name, const struct stat *st)
{
    struct find_t ft;
    size_t        len = strlen(path);
    char          aname[PATHMAX];
    unsigned      rc;

    if (!(func == 'u' && uptodate(name, st->st_mtime)) &&
        ask("a", name)) {
        strcpy(aname, name);
        strcat(aname, "/");
        if (!namefits(aname)) {
            warn(aname, "path name too long");
        } else {
            writehdr(aname, st, '5', 0L);
            if (verbose)
                fprintf(msg, "a %s\n", aname);
        }
    }

    if (len + 13 >= PATHMAX) {
        warn(path, "path name too long");
        return;
    }
    if (len > 0 && path[len - 1] != '\\' && path[len - 1] != ':')
        path[len++] = '\\';
    strcpy(path + len, "*.*");
    rc = _dos_findfirst(path, _A_NORMAL | _A_RDONLY | _A_HIDDEN |
                        _A_SYSTEM | _A_SUBDIR | _A_ARCH, &ft);
    while (rc == 0) {
        if (strcmp(ft.name, ".") != 0 && strcmp(ft.name, "..") != 0) {
            strcpy(path + len, ft.name);
            addpath(path);
        }
        rc = _dos_findnext(&ft);
    }
    path[len] = '\0';
}

static void addfile(const char *path, const char *name,
                    const struct stat *st)
{
    char     full[PATHMAX];
    char    *b;
    long     left;
    unsigned n;
    int      fd, got;

    if (afull[0] != '\0' && _fullpath(full, path, sizeof full) != NULL &&
        stricmp(full, afull) == 0) {
        fprintf(stderr, "tar: %s: file is the archive; not dumped\n", path);
        return;
    }
    if (func == 'u' && uptodate(name, st->st_mtime))
        return;
    if (!ask("a", name))
        return;
    if ((fd = open(path, O_RDONLY | O_BINARY)) < 0) {
        warn(path, strerror(errno));
        return;
    }
    if (!namefits(name)) {
        warn(name, "path name too long");
        close(fd);
        return;
    }
    writehdr(name, st, '0', st->st_size);
    if (verbose)
        fprintf(msg, "a %s, %ld bytes\n", name, st->st_size);
    for (left = st->st_size; left > 0; left -= n) {
        b = wrblock();
        n = left > BLOCK ? BLOCK : (unsigned)left;
        got = (fd >= 0) ? read(fd, b, n) : 0;
        if (got < 0) {
            warn(path, strerror(errno));
            got = 0;
        }
        if ((unsigned)got < n) {
            if (fd >= 0 && got >= 0)
                warn(path, "file changed size; padded with zeros");
            memset(b + got, 0, n - got);
            close(fd);
            fd = -1;
        }
    }
    if (fd >= 0)
        close(fd);
}

static void addpath(char *path)
{
    struct stat st;
    char        name[PATHMAX];
    size_t      len;

    /* drop trailing separators, but keep "\" and "C:\" */
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '\\' || path[len - 1] == '/') &&
           path[len - 2] != ':')
        path[--len] = '\0';

    if (stat(path, &st) != 0) {
        warn(path, strerror(errno));
        return;
    }
    tarname(name, path);
    len = strlen(name);
    while (len > 0 && name[len - 1] == '/')
        name[--len] = '\0';
    if (S_ISDIR(st.st_mode))
        adddir(path, name, &st);
    else if (S_ISREG(st.st_mode))
        addfile(path, name, &st);
    else
        warn(path, "not a regular file or directory");
}

/* ---------------------------------------------------------------- */

static void openarchive(void)
{
    int mode;

    if (strcmp(archive, "-") == 0) {
        if (func == 'r' || func == 'u')
            fatal("cannot update the standard input", NULL);
        afd = (func == 'c') ? fileno(stdout) : fileno(stdin);
        setmode(afd, O_BINARY);
        return;
    }
    switch (func) {
    case 'c': mode = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'r':
    case 'u': mode = O_RDWR | O_CREAT; break;
    default:  mode = O_RDONLY; break;
    }
    afd = open(archive, mode | O_BINARY, S_IRUSR | S_IWUSR);
    if (afd < 0)
        fatal(archive, strerror(errno));
    if (func != 't' && func != 'x' &&
        _fullpath(afull, archive, sizeof afull) == NULL)
        afull[0] = '\0';
}

int main(int argc, char **argv)
{
    char      *key;
    int        argi = 1, i;
    struct ent e;
    long       where;
    char       path[PATHMAX];

    tzset();
    if (argc < 2)
        usage();
    key = argv[argi++];
    if (*key == '-')
        key++;
    if (*key == '\0')
        usage();
    for (; *key != '\0'; key++) {
        switch (*key) {
        case 'c': case 'r': case 't': case 'u': case 'x':
            if (func != 0 && func != *key)
                usage();
            func = *key;
            break;
        case 'v': verbose = 1; break;
        case 'w': confirm = 1; break;
        case 'm': nomtime = 1; break;
        case 'l':                       /* DOS has no links */
        case 'o':                       /* DOS has no ownership */
            break;
        case 'f':
            if (argi >= argc)
                usage();
            archive = argv[argi++];
            break;
        case 'b':
            if (argi >= argc)
                usage();
            bfactor = atoi(argv[argi++]);
            if (bfactor < 1 || bfactor > MAXBF)
                fatal(argv[argi - 1], "invalid blocking factor");
            break;
        default:
            fprintf(stderr, "tar: unknown option '%c'\n", *key);
            usage();
        }
    }
    if (func == 0)
        usage();
    operands = argv + argi;
    noperands = argc - argi;
    if (noperands == 0 && (func == 'c' || func == 'r' || func == 'u'))
        usage();

    recsize = bfactor * BLOCK;
    /* one record plus one block of scratch space */
    if ((rec = malloc(recsize + BLOCK)) == NULL)
        fatal("out of memory", NULL);
    msg = (strcmp(archive, "-") == 0 && func != 't' && func != 'x')
          ? stderr : stdout;

    openarchive();

    switch (func) {
    case 't':
    case 'x':
        if ((found = calloc(noperands + 1, 1)) == NULL)
            fatal("out of memory", NULL);
        for (i = 0; i < noperands; i++) {
            char *s;

            for (s = operands[i]; *s != '\0'; s++)
                if (*s == '\\')
                    *s = '/';
            while (s > operands[i] + 1 && s[-1] == '/')
                *--s = '\0';
        }
        while (nextent(&e, &where)) {
            if (!selected(e.name)) {
                skipdata(e.size);
            } else if (func == 't') {
                listent(&e);
                skipdata(e.size);
            } else {
                extract(&e);
            }
        }
        for (i = 0; i < noperands; i++)
            if (!found[i])
                warn(operands[i], "not found in archive");
        break;

    case 'r':
    case 'u':
        while (nextent(&e, &where)) {
            if (func == 'u')
                remember(e.name, e.mtime);
            skipdata(e.size);
        }
        if (lseek(afd, where, SEEK_SET) < 0)
            fatal(archive, strerror(errno));
        recpos = 0;
        /* fall through */
    case 'c':
        for (i = 0; i < noperands; i++) {
            if (strlen(operands[i]) >= PATHMAX - 13) {
                warn(operands[i], "path name too long");
                continue;
            }
            strcpy(path, operands[i]);
            addpath(path);
        }
        wrend();
        break;
    }

    if (afd != fileno(stdin) && afd != fileno(stdout) && close(afd) != 0)
        fatal(archive, strerror(errno));
    return errors;
}
