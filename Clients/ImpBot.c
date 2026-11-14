/*
 * ImpBot - a computer player for Imperium.
 *
 * Copyright (C) 2026 the Asciiverse
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ImpBot plays Imperium the way a person does: it connects to TelImp's
 * TCP port, logs in (creating its player the first time), types commands
 * and reads what the game prints back. It knows nothing a person at a
 * terminal could not know: its map is what its own scans have shown it,
 * and it is bound by the same BTUs, money and production as everyone else.
 * The server is not changed in any way.
 *
 * Each session it looks over its empire, sets its planets to work,
 * builds, explores with its ships, settles the planets it finds, arms
 * its planets, and when it is strong enough goes after its neighbours.
 * Then it logs off and comes back when its BTUs have built up again.
 *
 *   ImpBot [options]
 *     -h HOST      the game's host (default 127.0.0.1)
 *     -p PORT      TelImp's port (default 3458)
 *     -n NAME      player name (default "Imperator")
 *     -w PASSWORD  player password (required)
 *     -c PASSWORD  the game's creation password (default add.me)
 *     -l LEVEL     easy, normal or hard (default normal)
 *     -r RACE      race number to join when creating the player
 *     -s FILE      where to keep what the bot knows (its map and notes)
 *     -L FILE      write a log of the session to FILE
 *     -1           play one session and exit
 *
 * Everything it learns is kept in the -s file, so it remembers its map
 * between sessions. It keeps playing until it is stopped with a signal.
 *
 * It is self-contained (only the C library and BSD sockets), so it can
 * be built anywhere TelImp can.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

/* ------------------------------------------------------------ settings */

typedef enum { L_EASY = 0, L_NORMAL = 1, L_HARD = 2 } Level_t;

static const char *Host = "127.0.0.1";
static const char *Port = "3458";
static const char *Name = "Imperator";
static const char *BaseName = "Imperator";
static char NameBuf[64];        /* the name it plays under after a rebirth */
static int Rebirths = 0;
static const char *Password = NULL;
static const char *CreatePass = "add.me";
static const char *StateFile = NULL;
static const char *LogFile = NULL;
static int WantRace = -1;
static int OneSession = 0;
static Level_t Level = L_NORMAL;
static FILE *Log = NULL;
static volatile sig_atomic_t Stopping = 0;

/* how each level plays */
typedef struct
{
    int btuReserve;     /* BTUs kept back at the end of a session */
    int sleepScale;     /* percent: how long it waits between sessions */
    int maxShips;       /* ships it is willing to run */
    int planetWeapons;  /* weapons it installs on each planet */
    int attackRatio;    /* percent: attacks when its power is this much */
                        /* of the target's (0: only fights back) */
    int maxColonies;    /* planets it tries to settle */
    int riskyMoves;     /* moves into unscanned space are allowed */
} Style_t;

static const Style_t Styles[3] =
{
    /* easy: slow, careful, defends but only strikes back */
    { 40, 250, 3, 1,   0,  6, 0 },
    /* normal */
    { 10, 120, 6, 3, 150, 14, 0 },
    /* hard: spends everything, expands fast, attacks sooner */
    {  4, 100, 10, 6, 110, 40, 1 }
};

#define ST (Styles[Level])

/* ------------------------------------------------------------- logging */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    char stamp[32];
    time_t now;

    if (Log == NULL)
    {
        return;
    }
    now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(Log, "%s [%s] ", stamp, Name);
    va_start(ap, fmt);
    vfprintf(Log, fmt, ap);
    va_end(ap);
    fputc('\n', Log);
    fflush(Log);
}

static void onSignal(int sig)
{
    (void) sig;
    Stopping = 1;
}

/* ------------------------------------------------------------ the line */

#define OUTMAX (512 * 1024)

static int Sock = -1;
static char Out[OUTMAX];        /* what the last command printed */
static size_t OutLen;
static int Btu = 0;             /* player BTUs, from the prompt */
static int Lost = 0;            /* the connection went away */
static size_t Mark = 0;         /* where Out stood when we last typed */

static long nowMs(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int sendLine(const char *line)
{
    char buf[512];
    size_t n;

    snprintf(buf, sizeof(buf), "%s\r\n", line);
    n = strlen(buf);
    Mark = OutLen;
    if (Log != NULL)
    {
        logf_("> %s", line);
    }
    if (write(Sock, buf, n) != (ssize_t) n)
    {
        Lost = 1;
        return 0;
    }
    return 1;
}

/* the text after the last newline in Out */
static const char *tail(void)
{
    const char *p = Out + OutLen;

    if (Mark > OutLen)
    {
        Mark = 0;
    }
    while (p > Out + Mark && p[-1] != '\n')
    {
        p--;
    }
    return p;
}

/* is the end of Out the main "[btu:time] Command: " prompt? */
static int atMainPrompt(void)
{
    const char *t = tail();
    int b, m;
    char c;

    if (sscanf(t, "[%d:%d] Command%c", &b, &m, &c) == 3 && c == ':')
    {
        Btu = b;
        return 1;
    }
    return 0;
}

/* does the end of Out look like a question waiting for an answer? */
static int atSubPrompt(void)
{
    const char *t = tail();
    size_t n = strlen(t);

    while (n > 0 && t[n - 1] == ' ')
    {
        n--;
    }
    return n > 0 && (t[n - 1] == ':' || t[n - 1] == '?' || t[n - 1] == '>');
}

/* read what the game sends until it waits for input; returns 1 for the
   main prompt, 2 for another question, 0 if the line is gone */
static int readUntilPrompt(int timeoutMs)
{
    struct pollfd pfd;
    char buf[8192];
    long deadline = nowMs() + timeoutMs, idleSince = nowMs();
    ssize_t n, i;
    int r, got = 0;

    for (;;)
    {
        if (got && OutLen > 0 && atMainPrompt())
        {
            return 1;
        }
        pfd.fd = Sock;
        pfd.events = POLLIN;
        r = poll(&pfd, 1, 50);
        if (r < 0 && errno != EINTR)
        {
            Lost = 1;
            return 0;
        }
        if (r > 0)
        {
            n = read(Sock, buf, sizeof(buf));
            if (n <= 0)
            {
                Lost = 1;
                return 0;
            }
            for (i = 0; i < n; i++)
            {
                unsigned char c = (unsigned char) buf[i];

                /* telnet commands and carriage returns are not text */
                if (c == 255 && i + 1 < n)
                {
                    i += ((unsigned char) buf[i + 1] >= 251) ? 2 : 1;
                    continue;
                }
                if (c == '\r' || c == 0)
                {
                    continue;
                }
                if (OutLen < OUTMAX - 1)
                {
                    Out[OutLen++] = (char) c;
                }
            }
            Out[OutLen] = '\0';
            idleSince = nowMs();
            got = 1;
            continue;
        }
        if (got && OutLen > 0 && atSubPrompt() && nowMs() - idleSince > 250)
        {
            return 2;
        }
        if (nowMs() > deadline || Stopping)
        {
            Lost = 1;
            return 0;
        }
    }
}

/* Answers a question a command asks; returns the reply to type, or NULL
   to just press return (which takes the default or cancels). */
typedef const char *(*Answer_t)(const char *question, void *ctx);

/* the last line of text (the question being asked) */
static void lastLine(char *buf, size_t size)
{
    snprintf(buf, size, "%s", tail());
}

/*
 * cmd - type a command and collect everything it prints into Out. Any
 * further questions it asks go to the answer function. Returns 1 when
 * the game is back at its main prompt.
 */
static int cmd(Answer_t answer, void *ctx, const char *fmt, ...)
{
    char line[512], q[512];
    va_list ap;
    size_t start;
    int r, asked = 0;

    if (Lost || Stopping)
    {
        return 0;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    OutLen = 0;
    Out[0] = '\0';
    Mark = 0;
    if (!sendLine(line))
    {
        return 0;
    }
    for (;;)
    {
        start = OutLen;
        r = readUntilPrompt(60000);
        if (Log != NULL && OutLen > start)
        {
            logf_("< %s", Out + start);
        }
        if (r == 1)
        {
            /* drop the prompt itself from the output */
            OutLen = (size_t) (tail() - Out);
            Out[OutLen] = '\0';
            return 1;
        }
        if (r == 0)
        {
            return 0;
        }
        lastLine(q, sizeof(q));
        {
            const char *a = (answer != NULL && asked < 200) ?
                answer(q, ctx) : NULL;

            asked++;
            /* a command that keeps asking gets "." and then empty lines */
            if (asked > 220)
            {
                a = ".";
            }
            if (!sendLine(a != NULL ? a : ""))
            {
                return 0;
            }
        }
    }
}

/* answer every question with the same list of replies, in order */
typedef struct
{
    const char **replies;
    int count, next;
} Seq_t;

static const char *seqAnswer(const char *q, void *ctx)
{
    Seq_t *s = ctx;

    (void) q;
    if (s->next < s->count)
    {
        return s->replies[s->next++];
    }
    return NULL;
}

/* ------------------------------------------------------- small helpers */

static int ciContains(const char *hay, const char *needle)
{
    return strcasestr(hay, needle) != NULL;
}

/* numbers in reports may be written 12x (1200) or 12k (12000) */
static long reportNum(const char *s)
{
    long v;
    char *end;

    while (*s == ' ')
    {
        s++;
    }
    v = strtol(s, &end, 10);
    if (*end == 'x')
    {
        v *= 100;
    }
    else if (*end == 'k')
    {
        v *= 1000;
    }
    return v;
}

/* split a line on '|' into fields; returns the count */
static int splitBars(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;

    while (n < max)
    {
        f[n++] = p;
        p = strchr(p, '|');
        if (p == NULL)
        {
            break;
        }
        *p++ = '\0';
    }
    return n;
}

static int rnd(int n)
{
    return n > 0 ? (int) (random() % n) : 0;
}

/* --------------------------------------------------------------- world */

static int Rows = 0, Cols = 0;      /* in subsectors */
static int SecPerItu = 600;
static time_t WorldCreated = 0;
static char *Map = NULL;            /* what each subsector looked like */
static int HomePlanet = -1;
static int HomeRow = -1, HomeCol = -1;
static long Money = 0;
static int MyNumber = -1;
static int Sessions = 0;
static int AttackedBy[64];          /* who has fired on us lately */

/*
 * The map holds one character per subsector:
 *   ?            not seen
 *   ' '          empty space
 *   *            a star (deadly)
 *   $ @          supernova / black hole sector (deadly)
 *   0            a planet whose class we could not make out
 *   H A B C ...  a planet of that class
 *   s            a ship was there (so it is not a star)
 */

#define MAP(r, c) Map[(r) * Cols + (c)]

static int onMap(int r, int c)
{
    return r >= 0 && c >= 0 && r < Rows && c < Cols;
}

static int deadly(char m)
{
    return m == '*' || m == '$' || m == '@';
}

static int isPlanetChar(char m)
{
    return m == '0' || m == 'H' || (m >= 'A' && m <= 'Q' && m != 'H');
}

/* what we know about a planet */
typedef struct
{
    int num, row, col;
    char cls;
    int owner;          /* -1 unknown, 0 unowned, 1 ours, 2 someone else */
    char ownerName[32];
    int size, gas, water, minr, gold, eff, civ, mil;
    int scanned;        /* we have its details */
    long when;          /* session we last looked at it */
    int tried;          /* settling it failed this many times */
} KPlanet;

#define MAXKP 4096
static KPlanet KP[MAXKP];
static int NKP = 0;

static KPlanet *kpAt(int r, int c)
{
    int i;

    for (i = 0; i < NKP; i++)
    {
        if (KP[i].row == r && KP[i].col == c)
        {
            return &KP[i];
        }
    }
    return NULL;
}

static KPlanet *kpAdd(int r, int c)
{
    KPlanet *k = kpAt(r, c);

    if (k == NULL && NKP < MAXKP)
    {
        k = &KP[NKP++];
        memset(k, 0, sizeof(*k));
        k->num = -1;
        k->row = r;
        k->col = c;
        k->owner = -1;
        k->cls = '0';
    }
    return k;
}

static void mapSet(int r, int c, char m)
{
    if (!onMap(r, c))
    {
        return;
    }
    /* a planet's class, once seen, is not forgotten for a '0' */
    if (m == '0' && isPlanetChar(MAP(r, c)))
    {
        return;
    }
    /* a ship hides what is under it; keep what we knew */
    if (m == 's' && MAP(r, c) != '?')
    {
        return;
    }
    MAP(r, c) = m;
    if (isPlanetChar(m))
    {
        KPlanet *k = kpAdd(r, c);

        if (k != NULL && m != '0')
        {
            k->cls = m;
        }
    }
}

/* ------------------------------------------------------- the state file */

static void saveState(void)
{
    char tmp[1024];
    FILE *f;
    int r, i;

    if (StateFile == NULL || Map == NULL)
    {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.new", StateFile);
    if ((f = fopen(tmp, "w")) == NULL)
    {
        return;
    }
    fprintf(f, "ImpBot 1\nsize %d %d\nhome %d %d %d\nsessions %d\n", Rows,
        Cols, HomePlanet, HomeRow, HomeCol, Sessions);
    if (Rebirths > 0)
    {
        fprintf(f, "rebirth %d %s\n", Rebirths, Name);
    }
    for (i = 0; i < NKP; i++)
    {
        KPlanet *k = &KP[i];

        fprintf(f, "planet %d %d %d %c %d %d %d %d %d %d %d %d %d %d %ld %d "
            "%s\n", k->num, k->row, k->col, k->cls, k->owner, k->size,
            k->gas, k->water, k->minr, k->gold, k->eff, k->civ, k->mil,
            k->scanned, k->when, k->tried,
            k->ownerName[0] != '\0' ? k->ownerName : "-");
    }
    fprintf(f, "map\n");
    for (r = 0; r < Rows; r++)
    {
        fwrite(&MAP(r, 0), 1, (size_t) Cols, f);
        fputc('\n', f);
    }
    fclose(f);
    (void) rename(tmp, StateFile);
}

static void allocMap(int rows, int cols)
{
    free(Map);
    Rows = rows;
    Cols = cols;
    Map = malloc((size_t) rows * (size_t) cols);
    if (Map == NULL)
    {
        fprintf(stderr, "ImpBot: out of memory\n");
        exit(1);
    }
    memset(Map, '?', (size_t) rows * (size_t) cols);
}

static void loadState(void)
{
    char line[8192];
    FILE *f;
    int r = 0, inMap = 0;

    if (StateFile == NULL || (f = fopen(StateFile, "r")) == NULL)
    {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        line[strcspn(line, "\n")] = '\0';
        if (inMap)
        {
            if (r < Rows && (int) strlen(line) >= Cols)
            {
                memcpy(&MAP(r, 0), line, (size_t) Cols);
            }
            r++;
            continue;
        }
        if (strncmp(line, "size ", 5) == 0)
        {
            int rows, cols;

            if (sscanf(line + 5, "%d %d", &rows, &cols) == 2 && rows > 0 &&
                cols > 0 && rows <= 4000 && cols <= 4000)
            {
                allocMap(rows, cols);
            }
        }
        else if (strncmp(line, "home ", 5) == 0)
        {
            (void) sscanf(line + 5, "%d %d %d", &HomePlanet, &HomeRow,
                &HomeCol);
        }
        else if (strncmp(line, "rebirth ", 8) == 0)
        {
            char *p = line + 8;

            Rebirths = atoi(p);
            p = strchr(p, ' ');
            if (p != NULL && p[1] != '\0')
            {
                snprintf(NameBuf, sizeof(NameBuf), "%s", p + 1);
                Name = NameBuf;
            }
        }
        else if (strncmp(line, "sessions ", 9) == 0)
        {
            Sessions = atoi(line + 9);
        }
        else if (strncmp(line, "planet ", 7) == 0 && NKP < MAXKP)
        {
            KPlanet *k = &KP[NKP];
            char cls, who[64];

            memset(k, 0, sizeof(*k));
            if (sscanf(line + 7, "%d %d %d %c %d %d %d %d %d %d %d %d %d %d "
                "%ld %d %63s", &k->num, &k->row, &k->col, &cls, &k->owner,
                &k->size, &k->gas, &k->water, &k->minr, &k->gold, &k->eff,
                &k->civ, &k->mil, &k->scanned, &k->when, &k->tried, who) == 17)
            {
                k->cls = cls;
                snprintf(k->ownerName, sizeof(k->ownerName), "%s",
                    strcmp(who, "-") == 0 ? "" : who);
                NKP++;
            }
        }
        else if (strcmp(line, "map") == 0 && Map != NULL)
        {
            inMap = 1;
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------ reports */

/* call fn for each line of Out */
static void eachLine(void (*fn)(char *line, void *ctx), void *ctx)
{
    char *copy = strdup(Out), *line, *save = NULL;

    if (copy == NULL)
    {
        return;
    }
    for (line = strtok_r(copy, "\n", &save); line != NULL;
        line = strtok_r(NULL, "\n", &save))
    {
        fn(line, ctx);
    }
    free(copy);
}

static void readInfo(void)
{
    const char *p;
    int rows, cols;

    if (!cmd(NULL, NULL, "info"))
    {
        return;
    }
    if ((p = strstr(Out, "World size: ")) != NULL &&
        sscanf(p, "World size: %d rows by %d columns", &rows, &cols) == 2 &&
        rows > 0 && cols > 0)
    {
        if (Map == NULL || Rows != rows * 10 || Cols != cols * 10)
        {
            allocMap(rows * 10, cols * 10);
            NKP = 0;
        }
    }
    if ((p = strstr(Out, "World created ")) != NULL)
    {
        struct tm tm;

        memset(&tm, 0, sizeof(tm));
        tm.tm_isdst = -1;
        if (strptime(p + strlen("World created "), "%a %b %d %H:%M:%S %Y",
            &tm) != NULL)
        {
            WorldCreated = mktime(&tm);
        }
    }
    if ((p = strstr(Out, "An ITU (Imperium Time Unit) is ")) != NULL)
    {
        SecPerItu = atoi(p + strlen("An ITU (Imperium Time Unit) is "));
        if (SecPerItu < 1)
        {
            SecPerItu = 1;
        }
    }
}

static void readStatus(void)
{
    const char *p;

    if (!cmd(NULL, NULL, "status"))
    {
        return;
    }
    if ((p = strstr(Out, "Cash on hand: ")) != NULL)
    {
        Money = atol(p + strlen("Cash on hand: "));
    }
    if ((p = strstr(Out, "Home planet: ")) != NULL)
    {
        HomePlanet = atoi(p + strlen("Home planet: "));
    }
    if (strstr(Out, "Compressed mode is ON") != NULL)
    {
        (void) cmd(NULL, NULL, "change compressed");
    }
}

static void findMyNumber(char *line, void *ctx)
{
    int n;
    char star, who[128];

    (void) ctx;
    /* " 1  *Sat Nov 14 ..." - our own line has the '*' and our name */
    if (sscanf(line, "%d %c", &n, &star) == 2 && star == '*' &&
        strlen(line) > strlen(Name))
    {
        snprintf(who, sizeof(who), "%s", line + strlen(line) - strlen(Name));
        if (strcmp(who, Name) == 0)
        {
            MyNumber = n;
        }
    }
}

/* ships */

typedef struct
{
    int num;
    char type;
    int row, col, eff, tf, energy, fuel, armour, planet;
    char fleet;
    /* cargo */
    int avail, civ, sci, mil, off, mis, ore, bars, air, ftanks;
    int comp, eng, life, elec, weap;
    /* big items installed */
    int photons, sensorsIn, enginesIn;
    int seen;
} MyShip;

#define MAXSHIPS 64
static MyShip Ships[MAXSHIPS];
static int NShips = 0;

static MyShip *shipNum(int num)
{
    int i;

    for (i = 0; i < NShips; i++)
    {
        if (Ships[i].num == num)
        {
            return &Ships[i];
        }
    }
    return NULL;
}

static void parseShipStatus(char *line, void *ctx)
{
    char *f[16];
    int n, num, r, c;
    char t;
    MyShip *s;

    (void) ctx;
    if (strchr(line, '|') == NULL || strstr(line, "Row,Col") != NULL)
    {
        return;
    }
    n = splitBars(line, f, 16);
    if (n < 10 || sscanf(f[0], "%d %c", &num, &t) != 2 ||
        sscanf(f[1], "%d,%d", &r, &c) != 2)
    {
        return;
    }
    if ((s = shipNum(num)) == NULL)
    {
        if (NShips >= MAXSHIPS)
        {
            return;
        }
        s = &Ships[NShips++];
        memset(s, 0, sizeof(*s));
        s->num = num;
    }
    s->type = t;
    s->row = r;
    s->col = c;
    while (*f[2] == ' ')
    {
        f[2]++;
    }
    s->fleet = *f[2];
    s->eff = atoi(f[3]);
    s->tf = atoi(f[4]);
    s->energy = (int) reportNum(f[5]);
    s->fuel = (int) reportNum(f[6]);
    s->armour = (int) reportNum(f[7]);
    s->planet = (sscanf(f[9], "%d", &num) == 1) ? num : -1;
    s->seen = 1;
}

static void parseShipCargo(char *line, void *ctx)
{
    char *f[20];
    int n, num;
    char t;
    MyShip *s;

    (void) ctx;
    if (strchr(line, '|') == NULL || strstr(line, "Avl") != NULL)
    {
        return;
    }
    n = splitBars(line, f, 20);
    if (n < 17 || sscanf(f[0], "%d %c", &num, &t) != 2 ||
        (s = shipNum(num)) == NULL)
    {
        return;
    }
    s->avail = (int) reportNum(f[1]);
    s->civ = (int) reportNum(f[2]);
    s->sci = (int) reportNum(f[3]);
    s->mil = (int) reportNum(f[4]);
    s->off = (int) reportNum(f[5]);
    s->mis = (int) reportNum(f[6]);
    s->ore = (int) reportNum(f[8]);
    s->bars = (int) reportNum(f[9]);
    s->air = (int) reportNum(f[10]);
    s->ftanks = (int) reportNum(f[11]);
    s->comp = (int) reportNum(f[12]);
    s->eng = (int) reportNum(f[13]);
    s->life = (int) reportNum(f[14]);
    s->elec = (int) reportNum(f[15]);
    s->weap = (int) reportNum(f[16]);
}

/* " Item | Ship | TF | T | Wgt | Eff | Ins" */
typedef struct
{
    int item, where, tf, eff, installed;
    char type;
} BigItem;

#define MAXBIG 512
static BigItem Big[MAXBIG];
static int NBig = 0;

static void parseBig(char *line, void *ctx)
{
    char *f[10];
    int n;
    BigItem *b;

    (void) ctx;
    if (NBig >= MAXBIG || strchr(line, '|') == NULL ||
        strstr(line, "Item") != NULL)
    {
        return;
    }
    n = splitBars(line, f, 10);
    if (n < 7)
    {
        return;
    }
    b = &Big[NBig];
    if (sscanf(f[0], "%d", &b->item) != 1 || sscanf(f[1], "%d",
        &b->where) != 1)
    {
        return;
    }
    b->tf = atoi(f[2]);
    while (*f[3] == ' ')
    {
        f[3]++;
    }
    b->type = *f[3];
    b->eff = atoi(f[5]);
    b->installed = ciContains(f[6], "yes");
    NBig++;
}

static void readShips(void)
{
    int i;

    for (i = 0; i < NShips; i++)
    {
        Ships[i].seen = 0;
    }
    if (!cmd(NULL, NULL, "ships status"))
    {
        return;
    }
    eachLine(parseShipStatus, NULL);
    /* forget ships that are gone */
    for (i = 0; i < NShips; )
    {
        if (!Ships[i].seen)
        {
            Ships[i] = Ships[--NShips];
        }
        else
        {
            i++;
        }
    }
    if (NShips == 0)
    {
        return;
    }
    if (cmd(NULL, NULL, "ships cargo"))
    {
        eachLine(parseShipCargo, NULL);
    }
    NBig = 0;
    if (cmd(NULL, NULL, "ships big"))
    {
        eachLine(parseBig, NULL);
    }
    for (i = 0; i < NShips; i++)
    {
        int j;
        MyShip *s = &Ships[i];

        s->photons = s->sensorsIn = s->enginesIn = 0;
        for (j = 0; j < NBig; j++)
        {
            if (Big[j].where == s->num && Big[j].installed)
            {
                if (Big[j].type == 'p')
                {
                    s->photons++;
                }
                else if (Big[j].type == 's')
                {
                    s->sensorsIn++;
                }
                else if (Big[j].type == 'e')
                {
                    s->enginesIn++;
                }
            }
        }
        /* a ship is never in a star */
        if (onMap(s->row, s->col) && MAP(s->row, s->col) == '?')
        {
            MAP(s->row, s->col) = 's';
        }
    }
}

/* planets we own */

typedef struct
{
    int num, row, col;
    char cls;
    int size, eff, minr, gold, pol, gas, water, mob, ore, bars, tf, res;
    int civ, sci, mil, off, btu;
    int miss, planes, airt, ftnk, comp, eng, life, elec, weap;
    int prod[17], work[17];
    int seen;
} MyPlanet;

#define MAXMINE 256
static MyPlanet Mine[MAXMINE];
static int NMine = 0;

static MyPlanet *mineNum(int num, int add)
{
    int i;

    for (i = 0; i < NMine; i++)
    {
        if (Mine[i].num == num)
        {
            return &Mine[i];
        }
    }
    if (!add || NMine >= MAXMINE)
    {
        return NULL;
    }
    memset(&Mine[NMine], 0, sizeof(MyPlanet));
    Mine[NMine].num = num;
    return &Mine[NMine++];
}

/* skip the flag and class columns at the start of a census line; returns
   the class letter and leaves *rest after it */
static char censusClass(char *line, char **rest)
{
    char *p = line, cls = 0;

    while (*p != '\0')
    {
        while (*p == ' ')
        {
            p++;
        }
        if (isdigit((unsigned char) *p) || *p == '\0')
        {
            break;
        }
        cls = *p;
        while (*p != ' ' && *p != '\0')
        {
            cls = *p;
            p++;
        }
    }
    *rest = p;
    return cls;
}

static void parseGeo(char *line, void *ctx)
{
    char *rest, cls;
    int v[13], r, c, num;
    MyPlanet *m;

    (void) ctx;
    if (strstr(line, "eff") != NULL || strchr(line, '|') != NULL)
    {
        return;
    }
    cls = censusClass(line, &rest);
    if (cls == 0 || sscanf(rest, "%d %d %d %d %d %d %d %d %d %d %d %d %d "
        "%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
        &v[8], &v[9], &v[10], &v[11], &num, &r, &c) != 15)
    {
        return;
    }
    if ((m = mineNum(num, 1)) == NULL)
    {
        return;
    }
    m->cls = cls;
    m->size = v[0];
    m->eff = v[1];
    m->minr = v[2];
    m->gold = v[3];
    m->pol = v[4];
    m->gas = v[5];
    m->water = v[6];
    m->mob = v[7];
    m->ore = v[8];
    m->bars = v[9];
    m->tf = v[10];
    m->res = v[11];
    m->row = r;
    m->col = c;
    m->seen = 1;
}

static void parsePop(char *line, void *ctx)
{
    char *rest, cls;
    int v[8], num;
    MyPlanet *m;

    (void) ctx;
    if (strstr(line, "civl") != NULL)
    {
        return;
    }
    cls = censusClass(line, &rest);
    if (cls == 0 || sscanf(rest, "%d %d %d %d %d %d %d %d %d", &v[0], &v[1],
        &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &num) != 9)
    {
        return;
    }
    if ((m = mineNum(num, 0)) == NULL)
    {
        return;
    }
    m->civ = v[0];
    m->sci = v[1];
    m->mil = v[2];
    m->off = v[3];
    m->btu = v[4];
}

static void parseItems(char *line, void *ctx)
{
    char *f[16];
    int n, num;
    MyPlanet *m;

    (void) ctx;
    if (strstr(line, "miss") != NULL || line[0] != '|')
    {
        return;
    }
    n = splitBars(line, f, 16);
    if (n < 13 || sscanf(f[12], "%d", &num) != 1 ||
        (m = mineNum(num, 0)) == NULL)
    {
        return;
    }
    m->miss = (int) reportNum(f[1]);
    m->planes = (int) reportNum(f[2]);
    m->ore = (int) reportNum(f[3]);
    m->bars = (int) reportNum(f[4]);
    m->airt = (int) reportNum(f[5]);
    m->ftnk = (int) reportNum(f[6]);
    m->comp = (int) reportNum(f[7]);
    m->eng = (int) reportNum(f[8]);
    m->life = (int) reportNum(f[9]);
    m->elec = (int) reportNum(f[10]);
    m->weap = (int) reportNum(f[11]);
}

/* the production census has a line of units ending in the planet number,
   then a line of work percentages */
static MyPlanet *ProdLast = NULL;

static void parseProd(char *line, void *ctx)
{
    char *f[24];
    int n, num, i;

    (void) ctx;
    if (line[0] != '|' || strstr(line, "Civ") != NULL ||
        strstr(line, "Prd") != NULL || strstr(line, "Wk%") != NULL ||
        strstr(line, "___") != NULL)
    {
        return;
    }
    n = splitBars(line, f, 24);
    if (n < 19)
    {
        return;
    }
    if (sscanf(f[18], "%d", &num) == 1)
    {
        ProdLast = mineNum(num, 0);
        if (ProdLast != NULL)
        {
            for (i = 0; i < 17; i++)
            {
                ProdLast->prod[i] = (int) reportNum(f[i + 1]);
            }
        }
    }
    else if (ProdLast != NULL)
    {
        for (i = 0; i < 17; i++)
        {
            ProdLast->work[i] = atoi(f[i + 1]);
        }
        ProdLast = NULL;
    }
}

static void readPlanets(void)
{
    int i;

    for (i = 0; i < NMine; i++)
    {
        Mine[i].seen = 0;
    }
    /* class 1 is the race's home planet, which is shared */
    if (!cmd(NULL, NULL, "census geo ?class#1"))
    {
        return;
    }
    eachLine(parseGeo, NULL);
    for (i = 0; i < NMine; )
    {
        if (!Mine[i].seen)
        {
            Mine[i] = Mine[--NMine];
        }
        else
        {
            i++;
        }
    }
    for (i = 0; i < NMine; i++)
    {
        KPlanet *k = kpAdd(Mine[i].row, Mine[i].col);

        mapSet(Mine[i].row, Mine[i].col, Mine[i].cls);
        if (k != NULL)
        {
            k->num = Mine[i].num;
            k->owner = 1;
            k->cls = Mine[i].cls;
            k->gas = Mine[i].gas;
            k->water = Mine[i].water;
            k->size = Mine[i].size;
            k->scanned = 1;
            snprintf(k->ownerName, sizeof(k->ownerName), "%s", Name);
        }
    }
    if (NMine == 0)
    {
        return;
    }
    if (cmd(NULL, NULL, "census pop ?class#1"))
    {
        eachLine(parsePop, NULL);
    }
    if (cmd(NULL, NULL, "census items ?class#1"))
    {
        eachLine(parseItems, NULL);
    }
    ProdLast = NULL;
    if (cmd(NULL, NULL, "census prod ?class#1"))
    {
        eachLine(parseProd, NULL);
    }
}

/*
 * Where the race's home planet is. Every look at the home planet brings it
 * up to date and bills whoever looked for its soldiers' pay, so we only
 * ever ask once, and not at all if a ship is sitting on it.
 */
static void readHome(void)
{
    char *line, *save = NULL, *copy;
    int i;

    if (HomePlanet < 0 || HomeRow >= 0)
    {
        return;
    }
    for (i = 0; i < NShips; i++)
    {
        if (Ships[i].planet == HomePlanet)
        {
            HomeRow = Ships[i].row;
            HomeCol = Ships[i].col;
            mapSet(HomeRow, HomeCol, 'H');
            return;
        }
    }
    if (!cmd(NULL, NULL, "census geo /1"))
    {
        return;
    }
    copy = strdup(Out);
    for (line = strtok_r(copy, "\n", &save); line != NULL;
        line = strtok_r(NULL, "\n", &save))
    {
        char *rest;
        int v[13], r, c, num;

        if (censusClass(line, &rest) == 'H' && sscanf(rest, "%d %d %d %d %d "
            "%d %d %d %d %d %d %d %d %d,%d", &v[0], &v[1], &v[2], &v[3],
            &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &num,
            &r, &c) == 15 && num == HomePlanet)
        {
            HomeRow = r;
            HomeCol = c;
            mapSet(r, c, 'H');
        }
    }
    free(copy);
}

/* ------------------------------------------------------------- scans */

/* read a short range scan map from Out into our map */
static void parseSRScan(void)
{
    char *lines[512], *copy = strdup(Out), *save = NULL, *l;
    int n = 0, i, j, k, first = -1, ncell, cols[256];

    if (copy == NULL)
    {
        return;
    }
    for (l = strtok_r(copy, "\n", &save); l != NULL && n < 512;
        l = strtok_r(NULL, "\n", &save))
    {
        lines[n++] = l;
    }
    /* map rows look like "0022 0     0022": the row number, a space, one
       character and a space per column, and the row number again */
    for (i = 0; i < n; i++)
    {
        size_t len = strlen(lines[i]);
        int digits = 0;

        while (isdigit((unsigned char) lines[i][digits]))
        {
            digits++;
        }
        if (digits >= 4 && lines[i][digits] == ' ' && len > (size_t) digits *
            2 && strncmp(lines[i], lines[i] + len - digits, (size_t) digits)
            == 0)
        {
            first = i;
            break;
        }
    }
    if (first < 1)
    {
        free(copy);
        return;
    }
    {
        int digits = 0, hdr, start;
        size_t len = strlen(lines[first]);

        while (isdigit((unsigned char) lines[first][digits]))
        {
            digits++;
        }
        ncell = (int) (len - (size_t) digits * 2) / 2;
        if (ncell > 256)
        {
            ncell = 256;
        }
        /* the column numbers are written downwards above the map, one
           line per digit */
        start = first;
        while (start > 0 && lines[start - 1][0] != '\0' &&
            strspn(lines[start - 1], "0123456789 ") ==
            strlen(lines[start - 1]))
        {
            start--;
        }
        hdr = first - start;
        if (hdr < 1)
        {
            free(copy);
            return;
        }
        for (j = 0; j < ncell; j++)
        {
            cols[j] = 0;
            for (k = start; k < first; k++)
            {
                size_t pos = (size_t) digits + 1 + (size_t) j * 2;
                char ch = (strlen(lines[k]) > pos) ? lines[k][pos] : ' ';

                cols[j] = cols[j] * 10 + (isdigit((unsigned char) ch) ?
                    ch - '0' : 0);
            }
        }
        for (i = first; i < n; i++)
        {
            int row, d2 = 0;

            while (isdigit((unsigned char) lines[i][d2]))
            {
                d2++;
            }
            if (d2 != digits || lines[i][d2] != ' ')
            {
                break;
            }
            row = atoi(lines[i]);
            for (j = 0; j < ncell; j++)
            {
                size_t pos = (size_t) digits + 1 + (size_t) j * 2;
                char ch;

                if (strlen(lines[i]) <= pos)
                {
                    break;
                }
                ch = lines[i][pos];
                if (ch == '?')
                {
                    continue;
                }
                if (ch == ' ')
                {
                    if (MAP(row, cols[j]) == '?' || MAP(row, cols[j]) == 's')
                    {
                        mapSet(row, cols[j], ' ');
                    }
                }
                else if (ch == '*' || ch == '$' || ch == '@' || ch == '0' ||
                    isupper((unsigned char) ch))
                {
                    mapSet(row, cols[j], ch);
                }
                else
                {
                    /* a ship */
                    mapSet(row, cols[j], 's');
                }
            }
        }
    }
    free(copy);
}

/* read a long range scan: one character per galactic sector */
static void parseLRScan(void)
{
    char *lines[256], *copy = strdup(Out), *save = NULL, *l;
    int n = 0, i, j, k, first = -1, ncell, cols[128];

    if (copy == NULL)
    {
        return;
    }
    for (l = strtok_r(copy, "\n", &save); l != NULL && n < 256;
        l = strtok_r(NULL, "\n", &save))
    {
        lines[n++] = l;
    }
    for (i = 0; i < n; i++)
    {
        size_t len = strlen(lines[i]);

        if (len > 9 && isdigit((unsigned char) lines[i][0]) &&
            isdigit((unsigned char) lines[i][3]) && lines[i][4] == ' ')
        {
            first = i;
            break;
        }
    }
    if (first < 1)
    {
        free(copy);
        return;
    }
    {
        int start = first, hdr;
        size_t len = strlen(lines[first]);

        ncell = (int) (len - 8) / 2;
        if (ncell > 128)
        {
            ncell = 128;
        }
        while (start > 0 && lines[start - 1][0] != '\0' &&
            strspn(lines[start - 1], "0123456789 ") ==
            strlen(lines[start - 1]))
        {
            start--;
        }
        hdr = first - start;
        if (hdr < 1)
        {
            free(copy);
            return;
        }
        for (j = 0; j < ncell; j++)
        {
            cols[j] = 0;
            for (k = start; k < first; k++)
            {
                size_t pos = 5 + (size_t) j * 2;
                char ch = (strlen(lines[k]) > pos) ? lines[k][pos] : ' ';

                cols[j] = cols[j] * 10 + (isdigit((unsigned char) ch) ?
                    ch - '0' : 0);
            }
        }
        for (i = first; i < n; i++)
        {
            int row;

            if (!isdigit((unsigned char) lines[i][0]) || lines[i][4] != ' ')
            {
                break;
            }
            row = atoi(lines[i]);
            for (j = 0; j < ncell; j++)
            {
                size_t pos = 5 + (size_t) j * 2;
                char ch;
                int r, c;

                if (strlen(lines[i]) <= pos)
                {
                    break;
                }
                ch = lines[i][pos];
                /* '@' black hole, '*' supernova: the whole sector is death */
                if (ch == '@' || ch == '*')
                {
                    for (r = row; r < row + 10; r++)
                    {
                        for (c = cols[j]; c < cols[j] + 10; c++)
                        {
                            mapSet(r, c, ch == '@' ? '@' : '$');
                        }
                    }
                }
            }
        }
    }
    free(copy);
}

/* "Results of sensor scan of planet 416:" and its details */
static KPlanet *parsePlanetScan(int row, int col)
{
    const char *p = strstr(Out, "Results of sensor scan of planet ");
    KPlanet *k;
    const char *q;
    char cls;
    int num;

    if (p == NULL)
    {
        return NULL;
    }
    num = atoi(p + strlen("Results of sensor scan of planet "));
    k = kpAdd(row, col);
    if (k == NULL)
    {
        return NULL;
    }
    k->num = num;
    k->when = Sessions;
    if (strstr(p, "home planet of the") != NULL)
    {
        k->cls = 'H';
        k->owner = 2;
        mapSet(row, col, 'H');
        return k;
    }
    if ((q = strstr(p, "Is a class ")) != NULL)
    {
        cls = q[strlen("Is a class ")];
        k->cls = cls;
        mapSet(row, col, cls);
    }
    if ((q = strstr(p, "controlled by ")) != NULL)
    {
        char who[64];
        size_t len;

        q += strlen("controlled by ");
        len = strcspn(q, "\n");
        snprintf(who, sizeof(who), "%.*s", (int) len, q);
        snprintf(k->ownerName, sizeof(k->ownerName), "%s", who);
        k->owner = (strcmp(who, Name) == 0) ? 1 : 2;
    }
    else if (strstr(p, "not under the control") != NULL)
    {
        k->owner = 0;
        k->ownerName[0] = '\0';
    }
    /* the line after the "Size|Polut|..." header */
    if ((q = strstr(p, "Size|")) != NULL && (q = strchr(q, '\n')) != NULL)
    {
        char buf[256], *f[16];
        size_t len;
        int n;

        q++;
        len = strcspn(q, "\n");
        snprintf(buf, sizeof(buf), "%.*s", (int) len, q);
        n = splitBars(buf, f, 16);
        if (n >= 12)
        {
            k->size = atoi(f[0]);
            k->eff = atoi(f[2]);
            k->minr = atoi(f[3]);
            k->gold = atoi(f[4]);
            k->gas = atoi(f[5]);
            k->water = atoi(f[6]);
            k->civ = atoi(f[9]);
            k->mil = atoi(f[10]);
            k->scanned = 1;
        }
    }
    return k;
}

/* ------------------------------------------------------------ logging in */

static char RaceReply[16];
static int RaceWasEmpty = 0;    /* nobody else of our race when we joined */

/* pick the race with the fewest players, so each computer empire tends to
   get a home planet of its own */
static void chooseRace(void)
{
    char *copy = strdup(Out), *line, *save = NULL;
    int best = -1, bestPlayers = 1 << 30, ties = 0, race = -1;

    if (copy == NULL)
    {
        snprintf(RaceReply, sizeof(RaceReply), "0");
        return;
    }
    for (line = strtok_r(copy, "\n", &save); line != NULL;
        line = strtok_r(NULL, "\n", &save))
    {
        char *f[8];
        int n, num;

        if (line[0] != '|')
        {
            continue;
        }
        n = splitBars(line, f, 8);
        if (n < 6)
        {
            continue;
        }
        if (sscanf(f[1], "%d", &num) == 1)
        {
            race = num;
        }
        else if (race >= 0 && isdigit((unsigned char) f[4][strspn(f[4],
            " ")]))
        {
            int players = atoi(f[4]);

            if (players < bestPlayers)
            {
                bestPlayers = players;
                best = race;
                ties = 1;
            }
            else if (players == bestPlayers && rnd(++ties) == 0)
            {
                best = race;
            }
            race = -1;
        }
    }
    free(copy);
    if (WantRace >= 0)
    {
        best = WantRace;
    }
    snprintf(RaceReply, sizeof(RaceReply), "%d", best >= 0 ? best : 0);
    RaceWasEmpty = (WantRace < 0 && bestPlayers == 0);
}

static int LoginFailed = 0;

static const char *loginAnswer(const char *q, void *ctx)
{
    (void) ctx;
    if (ciContains(q, "player name"))
    {
        return Name;
    }
    if (ciContains(q, "wish to create") || ciContains(q, "(y/n)"))
    {
        return "y";
    }
    if (ciContains(q, "creation password"))
    {
        return CreatePass;
    }
    if (ciContains(q, "password"))
    {
        if (ciContains(Out, "Invalid password") || ciContains(Out,
            "incorrect"))
        {
            LoginFailed = 1;
            return NULL;
        }
        return Password;
    }
    if (ciContains(q, "race"))
    {
        chooseRace();
        logf_("joining race %s", RaceReply);
        return RaceReply;
    }
    return NULL;
}

static int connectGame(void)
{
    struct addrinfo hints, *res, *ai;
    int err, r, asked = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((err = getaddrinfo(Host, Port, &hints, &res)) != 0)
    {
        logf_("can't find %s: %s", Host, gai_strerror(err));
        return 0;
    }
    Sock = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next)
    {
        Sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (Sock < 0)
        {
            continue;
        }
        if (connect(Sock, ai->ai_addr, ai->ai_addrlen) == 0)
        {
            break;
        }
        close(Sock);
        Sock = -1;
    }
    freeaddrinfo(res);
    if (Sock < 0)
    {
        logf_("can't connect to %s port %s", Host, Port);
        return 0;
    }
    Lost = 0;
    LoginFailed = 0;
    Mark = 0;
    OutLen = 0;
    Out[0] = '\0';
    for (;;)
    {
        const char *a;

        r = readUntilPrompt(30000);
        if (r == 1)
        {
            logf_("logged in with %d BTUs", Btu);
            return 1;
        }
        if (r == 0 || LoginFailed || ++asked > 20)
        {
            logf_("login failed: %s", tail());
            close(Sock);
            Sock = -1;
            return 0;
        }
        logf_("< %s", tail());
        a = loginAnswer(tail(), NULL);
        if (LoginFailed)
        {
            close(Sock);
            Sock = -1;
            return 0;
        }
        if (!sendLine(a != NULL ? a : ""))
        {
            return 0;
        }
    }
}

static void disconnectGame(void)
{
    if (Sock >= 0)
    {
        if (!Lost)
        {
            (void) sendLine("bye");
            usleep(300000);
        }
        close(Sock);
        Sock = -1;
    }
}

/* ------------------------------------------------------------ finding a way */

static const int DR[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
static const int DC[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
static const char DIRCH[8] = { '7', '8', '9', '4', '6', '1', '2', '3' };

#define WIN 70      /* how far around a ship we plan, in subsectors */

static int passable(int r, int c, int risky)
{
    char m;

    if (!onMap(r, c))
    {
        return 0;
    }
    m = MAP(r, c);
    if (deadly(m))
    {
        return 0;
    }
    if (m == '?')
    {
        return risky;
    }
    /* something too far off to make out: a planet or a star */
    if (m == '0')
    {
        return 0;
    }
    return 1;
}

/*
 * Breadth-first search from (r0,c0) over the window around it. Cells are
 * passable if we know they are safe (or, for risky players, unknown). The
 * goal function says which cells are wanted. On success the path of
 * direction digits is written to path (at most maxLen steps) and the goal
 * cell to *gr,*gc.
 */
typedef int (*Goal_t)(int r, int c, void *ctx);

static int findPath(int r0, int c0, Goal_t goal, void *ctx, char *path,
    int maxLen, int *gr, int *gc, int risky)
{
    static short prev[2 * WIN + 1][2 * WIN + 1];
    static int queue[(2 * WIN + 1) * (2 * WIN + 1)];
    int head = 0, qtail = 0, i, found = -1, len;
    int r, c, wr, wc;

    for (r = 0; r < 2 * WIN + 1; r++)
    {
        for (c = 0; c < 2 * WIN + 1; c++)
        {
            prev[r][c] = -1;
        }
    }
    prev[WIN][WIN] = 8;
    queue[qtail++] = WIN * (2 * WIN + 1) + WIN;
    while (head < qtail)
    {
        int cur = queue[head++];

        wr = cur / (2 * WIN + 1);
        wc = cur % (2 * WIN + 1);
        r = r0 + wr - WIN;
        c = c0 + wc - WIN;
        if (cur != WIN * (2 * WIN + 1) + WIN && goal(r, c, ctx))
        {
            found = cur;
            break;
        }
        /* orthogonal moves first, so paths prefer them when equal */
        for (i = 0; i < 8; i++)
        {
            static const int order[8] = { 1, 3, 4, 6, 0, 2, 5, 7 };
            int d = order[i];
            int nr = wr + DR[d], nc = wc + DC[d];

            if (nr < 0 || nc < 0 || nr > 2 * WIN || nc > 2 * WIN ||
                prev[nr][nc] != -1)
            {
                continue;
            }
            if (!passable(r0 + nr - WIN, c0 + nc - WIN, risky) &&
                !goal(r0 + nr - WIN, c0 + nc - WIN, ctx))
            {
                continue;
            }
            /* the goal itself must not be deadly */
            if (deadly(MAP(r0 + nr - WIN, c0 + nc - WIN)) || (!risky &&
                MAP(r0 + nr - WIN, c0 + nc - WIN) == '?' &&
                !goal(r0 + nr - WIN, c0 + nc - WIN, ctx)))
            {
                continue;
            }
            prev[nr][nc] = (short) d;
            queue[qtail++] = nr * (2 * WIN + 1) + nc;
        }
    }
    if (found < 0)
    {
        return -1;
    }
    /* walk back to build the path */
    {
        char rev[4 * WIN];
        int n = 0;

        wr = found / (2 * WIN + 1);
        wc = found % (2 * WIN + 1);
        *gr = r0 + wr - WIN;
        *gc = c0 + wc - WIN;
        while (!(wr == WIN && wc == WIN) && n < 4 * WIN)
        {
            int d = prev[wr][wc];

            rev[n++] = DIRCH[d];
            wr -= DR[d];
            wc -= DC[d];
        }
        len = n;
        if (len > maxLen)
        {
            len = maxLen;
        }
        for (i = 0; i < len; i++)
        {
            path[i] = rev[n - 1 - i];
        }
        path[len] = '\0';
        return n;
    }
}

/* goals */

static int goalCell(int r, int c, void *ctx)
{
    int *rc = ctx;

    return r == rc[0] && c == rc[1];
}

/* a known safe cell next to unknown space, preferring ones near home */
static int goalFrontier(int r, int c, void *ctx)
{
    int i;

    (void) ctx;
    if (!onMap(r, c) || MAP(r, c) == '?' || deadly(MAP(r, c)))
    {
        return 0;
    }
    for (i = 0; i < 8; i += 1)
    {
        if ((DR[i] == 0 || DC[i] == 0) && onMap(r + DR[i], c + DC[i]) &&
            MAP(r + DR[i], c + DC[i]) == '?')
        {
            return 1;
        }
    }
    return 0;
}

/* an unexamined planet */
static int goalUnscanned(int r, int c, void *ctx)
{
    KPlanet *k;

    (void) ctx;
    if (!onMap(r, c) || !isPlanetChar(MAP(r, c)) || MAP(r, c) == 'H')
    {
        return 0;
    }
    k = kpAt(r, c);
    return k == NULL || !k->scanned;
}

/* ------------------------------------------------------------ moving */

static const char *dotAnswer(const char *q, void *ctx)
{
    (void) q;
    (void) ctx;
    return ".";
}


static void scanFrom(MyShip *s)
{
    if (cmd(NULL, NULL, "scan sr %d", s->num))
    {
        parseSRScan();
    }
}

/* move a ship along a path; returns 1 if it moved */
static int moveShip(MyShip *s, const char *path)
{
    const char *p;
    int r = s->row, c = s->col;

    if (path[0] == '\0' || s->planet >= 0)
    {
        return 0;
    }
    if (!cmd(dotAnswer, NULL, "navigate %d %s.", s->num, path))
    {
        return 0;
    }
    for (p = path; *p != '\0'; p++)
    {
        int d;

        for (d = 0; d < 8; d++)
        {
            if (DIRCH[d] == *p)
            {
                r += DR[d];
                c += DC[d];
            }
        }
    }
    /* read where it really ended up */
    {
        int num = s->num;
        int destroyed = strstr(Out, " destroyed") != NULL ||
            strstr(Out, "takes 100% damage") != NULL;

        readShips();
        s = shipNum(num);
        if (s == NULL || destroyed)
        {
            /* only the last step went somewhere unknown: that was a star */
            logf_("ship %d flew into a star at %d,%d", num, r, c);
            if (onMap(r, c))
            {
                MAP(r, c) = '*';
            }
            return 0;
        }
    }
    if (s->row != r || s->col != c)
    {
        logf_("ship %d stopped at %d,%d short of %d,%d", s->num, s->row,
            s->col, r, c);
    }
    return 1;
}

/* ------------------------------------------------------------ cargo */

typedef struct
{
    int civ, sci, mil, off, mis, ftanks;   /* how many to move; -1: all */
} Cargo_t;

/* answer "N civilians here, M on board, (un)load how many (max K)" */
static const char *cargoAnswer(const char *q, void *ctx)
{
    static char buf[16];
    Cargo_t *c = ctx;
    const char *p = strstr(q, "(max ");
    int max, want = 0;

    if (p == NULL)
    {
        return "0";
    }
    max = atoi(p + 5);
    if (strstr(q, " civilians here") != NULL)
    {
        want = c->civ;
    }
    else if (strstr(q, " scientists here") != NULL)
    {
        want = c->sci;
    }
    else if (strstr(q, " military here") != NULL)
    {
        want = c->mil;
    }
    else if (strstr(q, " officers here") != NULL)
    {
        want = c->off;
    }
    else if (strstr(q, " missiles here") != NULL)
    {
        want = c->mis;
    }
    else if (strstr(q, " fuel tanks here") != NULL)
    {
        want = c->ftanks;
    }
    if (want < 0 || want > max)
    {
        want = max;
    }
    snprintf(buf, sizeof(buf), "%d", want);
    return buf;
}

static int colonisable(const KPlanet *k)
{
    return k != NULL && k->scanned && k->owner == 0 && k->cls != 'H' &&
        k->gas >= 45 && k->water < 100 && k->tried < 3;
}

static int goalColony(int r, int c, void *ctx)
{
    KPlanet *k;

    (void) ctx;
    if (!onMap(r, c) || !isPlanetChar(MAP(r, c)))
    {
        return 0;
    }
    k = kpAt(r, c);
    return colonisable(k);
}

static int NumColonies(void)
{
    return NMine;
}

/* ------------------------------------------------------------ planets */

/* how each kind of planet shares out its work, in the order the game asks
   (civilians, military, technology, research, education, ocs, ore mining,
   gold mining, air tanks, fuel tanks, weapons, engines, hull, missile,
   planes, electronics); what is left is cash */
static void planDivvy(const MyPlanet *m, int work[16])
{
    int i, ore;

    for (i = 0; i < 16; i++)
    {
        work[i] = 0;
    }
    ore = m->minr >= 40 ? 30 : (m->minr >= 15 ? 22 : 12);
    work[1] = 6;                            /* military */
    work[2] = 6;                            /* technology */
    work[3] = 4;                            /* research */
    work[4] = 2;                            /* education */
    work[5] = 2;                            /* officers */
    work[6] = ore;                          /* ore mining */
    work[7] = m->gold >= 10 ? 8 : 0;        /* gold mining */
    work[9] = (m->gas > 50 && m->water > 25) ? 5 : 0;   /* fuel tanks */
    work[10] = 8;                           /* weapons */
    work[11] = 4;                           /* engines */
    work[12] = 8;                           /* hull */
    work[13] = 5;                           /* missiles */
    work[15] = 5;                           /* electronics */
    if (Level == L_EASY)
    {
        /* the easy empire builds less and keeps more as cash */
        work[10] = 4;
        work[12] = 4;
        work[2] = 3;
    }
}

static const char *ProdNames[16] =
{
    "civilians", "military", "technology", "research", "education", "ocs",
    "ore mining", "gold mining", "air tanks", "fuel tanks", "weapons",
    "engines", "hull", "missile", "planes", "electronics"
};

static const char *divvyAnswer(const char *q, void *ctx)
{
    static char buf[16];
    int *work = ctx, i;
    const char *p = strstr(q, "devoted to ");

    if (p == NULL)
    {
        return NULL;
    }
    p += strlen("devoted to ");
    for (i = 15; i >= 0; i--)
    {
        if (strncmp(p, ProdNames[i], strlen(ProdNames[i])) == 0)
        {
            snprintf(buf, sizeof(buf), "%d", work[i]);
            return buf;
        }
    }
    return "0";
}

static int workUnset(const MyPlanet *m)
{
    int i, sum = 0;

    for (i = 0; i < 16; i++)
    {
        sum += m->work[i];
    }
    return sum == 0;
}

static void setDivvy(MyPlanet *m)
{
    int work[16];

    planDivvy(m, work);
    if (cmd(divvyAnswer, work, "divvy %d", m->num))
    {
        logf_("set work on planet %d", m->num);
        memcpy(m->work, work, sizeof(work));
    }
}

/* answer the build questions in order */
static int build(int planet, const char *what, const char *sub)
{
    const char *replies[2];
    Seq_t seq;

    replies[0] = what;
    replies[1] = sub;
    seq.replies = replies;
    seq.count = sub != NULL ? 2 : 1;
    seq.next = 0;
    if (!cmd(seqAnswer, &seq, "build %d", planet))
    {
        return 0;
    }
    if (strstr(Out, "Built ") != NULL)
    {
        logf_("built %s %s on %d", what, sub != NULL ? sub : "", planet);
        return 1;
    }
    return 0;
}

/* the number of a big item just built, from "Built weapon 123 on ..." */
static int builtItem(void)
{
    const char *p = strstr(Out, "Built ");
    int n;

    while (p != NULL)
    {
        const char *q = p + 6;

        while (*q != '\0' && *q != '\n' && !isdigit((unsigned char) *q))
        {
            q++;
        }
        if (isdigit((unsigned char) *q) && sscanf(q, "%d", &n) == 1)
        {
            return n;
        }
        p = strstr(p + 1, "Built ");
    }
    return -1;
}

/* big items lying on our planets: " Item | Planet | TF | T | Wgt | Eff" */
typedef struct
{
    int item, planet;
    char type;
} PlBig;

static PlBig PBig[1024];
static int NPBig = 0;

static void parsePlBig(char *line, void *ctx)
{
    char *f[8];
    int n;
    PlBig *b;

    (void) ctx;
    if (NPBig >= 1024 || strchr(line, '|') == NULL ||
        strstr(line, "Item") != NULL)
    {
        return;
    }
    n = splitBars(line, f, 8);
    b = &PBig[NPBig];
    if (n < 6 || sscanf(f[0], "%d", &b->item) != 1 ||
        sscanf(f[1], "%d", &b->planet) != 1)
    {
        return;
    }
    while (*f[3] == ' ')
    {
        f[3]++;
    }
    b->type = *f[3];
    NPBig++;
}

static void readPlanetBig(void)
{
    NPBig = 0;
    if (NMine > 0 && cmd(NULL, NULL, "census big ?class#1"))
    {
        eachLine(parsePlBig, NULL);
    }
}

static int countPlBig(int planet, char type)
{
    int i, n = 0;

    for (i = 0; i < NPBig; i++)
    {
        if (PBig[i].planet == planet && PBig[i].type == type)
        {
            n++;
        }
    }
    return n;
}

/* take a big item of a type from a planet's list; -1 if none */
static int takePlBig(int planet, char type)
{
    int i;

    for (i = 0; i < NPBig; i++)
    {
        if (PBig[i].planet == planet && PBig[i].type == type)
        {
            int item = PBig[i].item;

            PBig[i] = PBig[--NPBig];
            return item;
        }
    }
    return -1;
}

/* planet weapons are installed as soon as they are built, so every
   photon tube on a planet is counted as installed (an install that fails
   because the planet is full leaves it in store, which is also fine) */

static const char *photonAnswer(const char *q, void *ctx)
{
    static char buf[16];
    const char *p;

    (void) ctx;
    if (ciContains(q, "blaser/photon") || ciContains(q, "weapon"))
    {
        return "photon";
    }
    if ((p = strstr(q, "(max ")) != NULL)
    {
        snprintf(buf, sizeof(buf), "%d", atoi(p + 5));
        return buf;
    }
    return NULL;
}

#define SHIP_PROD_COST(t) ((t) == 'a' ? 120 : (t) == 'b' ? 275 : 425)

/* keep money back for military pay and upkeep */
static long reserveMoney(void)
{
    return 600 + 150L * NMine;
}

static int affordProd(int units)
{
    return Money - (long) units * 9 > reserveMoney();
}

static void managePlanets(void)
{
    int i;

    for (i = 0; i < NMine && Btu > ST.btuReserve + 6 && !Lost; i++)
    {
        MyPlanet *m = &Mine[i];

        if (workUnset(m) && m->btu >= 2)
        {
            setDivvy(m);
            continue;
        }
        if (m->eff < 60 || m->btu < 6)
        {
            continue;
        }
        /* defend: torpedo tubes on every planet, with missiles made there */
        if (countPlBig(m->num, 'p') < ST.planetWeapons && m->prod[10] >= 114
            && affordProd(114))
        {
            if (build(m->num, "weapon", "photon"))
            {
                int item = builtItem();

                Money -= 114 * 9;
                if (item >= 0)
                {
                    (void) cmd(NULL, NULL, "install %dp %d", m->num, item);
                    PBig[NPBig].item = -1;
                    if (NPBig < 1024)
                    {
                        PBig[NPBig].planet = m->num;
                        PBig[NPBig].type = 'p';
                        PBig[NPBig].item = item;
                        NPBig++;
                    }
                }
            }
            continue;
        }
        /* a new ship once the planet has made the parts for one */
        if (NShips < ST.maxShips && m->prod[12] >= 275 && m->prod[11] >= 190
            && m->prod[15] >= 40 && m->eff >= 70 && m->civ >= 300 &&
            affordProd(275 + 190 + 40 + 114))
        {
            static const char *parts[4] = { "engine", "computer", "lifesup",
                "sensor" };
            int p, ship;

            if (!build(m->num, "ship", "b"))
            {
                continue;
            }
            ship = builtItem();
            Money -= 275 * 9;
            if (ship < 0)
            {
                continue;
            }
            logf_("new ship %d on planet %d", ship, m->num);
            /* each build costs the planet 5 BTUs */
            for (p = 0; p < 4 && !Lost; p++)
            {
                int item;

                if (strcmp(parts[p], "engine") != 0 &&
                    countPlBig(m->num, parts[p][0] == 'l' ? 'l' :
                    parts[p][0]) > 0)
                {
                    item = takePlBig(m->num, parts[p][0]);
                }
                else if (build(m->num, parts[p], NULL))
                {
                    item = builtItem();
                }
                else
                {
                    item = -1;
                }
                if (item >= 0)
                {
                    (void) cmd(NULL, NULL, "load big %d %d", ship, item);
                    (void) cmd(NULL, NULL, "install %d %d", ship, item);
                }
            }
            /* a torpedo tube if the planet has one spare */
            {
                int item = (countPlBig(m->num, 'p') > ST.planetWeapons) ?
                    takePlBig(m->num, 'p') : -1;

                if (item < 0 && m->prod[10] >= 114 && affordProd(114) &&
                    build(m->num, "weapon", "photon"))
                {
                    item = builtItem();
                }
                if (item >= 0)
                {
                    (void) cmd(NULL, NULL, "load big %d %d", ship, item);
                    (void) cmd(NULL, NULL, "install %d %d", ship, item);
                }
            }
            {
                Cargo_t c = { 0, 4, 10, 4, 40, -1 };

                (void) cmd(cargoAnswer, &c, "load small %d", ship);
                (void) cmd(NULL, NULL, "refuel %d", ship);
            }
            continue;
        }
    }
}

/* ------------------------------------------------------------ ships */

/* what each ship is doing, kept between sessions only in memory */
static int nearestHomeOrColony(MyShip *s, int *gr, int *gc, char *path)
{
    int rc[2];

    (void) path;
    rc[0] = HomeRow;
    rc[1] = HomeCol;
    if (HomeRow < 0)
    {
        return -1;
    }
    *gr = HomeRow;
    *gc = HomeCol;
    return findPath(s->row, s->col, goalCell, rc, path, 30, gr, gc, 0);
}

static int maxSteps(const MyShip *s)
{
    int n = s->energy / 8;

    return n > 30 ? 30 : n;
}

static void scanPlanetHere(MyShip *s)
{
    if (cmd(NULL, NULL, "scan planet 0 %d", s->num))
    {
        KPlanet *k = parsePlanetScan(s->row, s->col);

        if (k != NULL)
        {
            logf_("planet %d at %d,%d: class %c owner %d gas %d water %d",
                k->num, s->row, s->col, k->cls, k->owner, k->gas, k->water);
        }
    }
}

static void scanFromPlanet(int planet);

/* land, put people down and take off again */
static void settle(int num)
{
    MyShip *s = shipNum(num);
    KPlanet *k;
    Cargo_t c;

    if (s == NULL)
    {
        return;
    }
    k = kpAt(s->row, s->col);
    if (!cmd(NULL, NULL, "land %d", num) || strstr(Out, "on the surface") == NULL)
    {
        if (k != NULL)
        {
            k->tried++;
        }
        return;
    }
    /* keep a crew aboard */
    c.civ = s->civ;
    c.sci = s->sci > 2 ? s->sci - 2 : 0;
    c.mil = s->mil > 2 ? s->mil - 2 : 0;
    c.off = s->off > 2 ? s->off - 2 : (s->off > 1 ? 1 : 0);
    c.mis = 0;
    c.ftanks = 0;
    (void) cmd(cargoAnswer, &c, "unload small %d", num);
    if (strstr(Out, "You now own") != NULL)
    {
        logf_("settled planet at %d,%d", s->row, s->col);
        if (k != NULL)
        {
            k->owner = 1;
            scanFromPlanet(k->num);
        }
    }
    else if (k != NULL)
    {
        k->tried++;
    }
    (void) cmd(NULL, NULL, "liftoff %d", num);
}

/* look around from a planet we own: planets see much further than the
   sensors of a small ship, and tell stars from planets */
static void scanFromPlanet(int planet)
{
    if (cmd(NULL, NULL, "scan sr %dp", planet))
    {
        parseSRScan();
    }
}

/*
 * A planet we can't live on can still be held for a moment: put one
 * settler down, look around from it, and take the settler back (which
 * gives the planet up again).
 */
static void probe(int num)
{
    MyShip *s = shipNum(num);
    KPlanet *k;
    Cargo_t c = { 1, 0, 0, 0, 0, 0 };

    if (s == NULL || s->civ < 1)
    {
        return;
    }
    k = kpAt(s->row, s->col);
    if (k == NULL || k->num < 0 || k->owner != 0)
    {
        return;
    }
    k->tried = 9;   /* once is enough */
    if (!cmd(NULL, NULL, "land %d", num) || strstr(Out, "on the surface") == NULL)
    {
        return;
    }
    (void) cmd(cargoAnswer, &c, "unload small %d", num);
    if (strstr(Out, "You now own") != NULL)
    {
        logf_("looking around from planet %d", k->num);
        scanFromPlanet(k->num);
        c.civ = -1;
        (void) cmd(cargoAnswer, &c, "load small %d", num);
    }
    (void) cmd(NULL, NULL, "liftoff %d", num);
}

/*
 * The race's home planet bills whoever touches it for its soldiers' pay
 * since anyone last did, and a player with no money left produces
 * nothing ever again. So we only take settlers from it when we are sure
 * the bill is small: our race had nobody else in it, and the universe is
 * young enough that its pay since then is well within our money.
 */
static int homeBillSmall(void)
{
    long itus;

    if (!RaceWasEmpty || WorldCreated == 0 || Sessions > 2)
    {
        return 0;
    }
    itus = (long) (time(NULL) - WorldCreated) / SecPerItu;
    return itus * 110 < Money - 1500;
}

/* fill up with settlers at the race's home planet or one of ours */
static void takeSettlers(int num)
{
    MyShip *s = shipNum(num);
    Cargo_t c;
    int room;

    if (s == NULL)
    {
        return;
    }
    if (s->planet < 0 && !cmd(NULL, NULL, "land %d", num))
    {
        return;
    }
    room = s->avail - 50;
    c.civ = room > 400 ? 400 : (room > 0 ? room : 0);
    c.sci = 0;
    c.mil = 20;
    c.off = 4;
    c.mis = 0;
    c.ftanks = -1;
    (void) cmd(cargoAnswer, &c, "load small %d", num);
    s = shipNum(num);
    (void) cmd(NULL, NULL, "refuel %d", num);
    (void) cmd(NULL, NULL, "liftoff %d", num);
}

static int canBoard(int planet)
{
    int i;

    if (planet == HomePlanet)
    {
        return NMine == 0 && homeBillSmall();
    }
    for (i = 0; i < NMine; i++)
    {
        if (Mine[i].num == planet && Mine[i].civ > 400)
        {
            return 1;
        }
    }
    return 0;
}

/* goal: one of our planets with people to spare, or the home planet */
static int goalBoarding(int r, int c, void *ctx)
{
    int i;

    (void) ctx;
    if (r == HomeRow && c == HomeCol && NMine == 0 && homeBillSmall())
    {
        return 1;
    }
    for (i = 0; i < NMine; i++)
    {
        if (Mine[i].row == r && Mine[i].col == c && Mine[i].civ > 600)
        {
            return 1;
        }
    }
    return 0;
}

/* an enemy planet we know of */
static int goalEnemy(int r, int c, void *ctx)
{
    KPlanet *k;

    (void) ctx;
    if (!onMap(r, c) || !isPlanetChar(MAP(r, c)))
    {
        return 0;
    }
    k = kpAt(r, c);
    return k != NULL && k->owner == 2 && k->cls != 'H' && k->num >= 0;
}

static int war(void);

/* attack the enemy planet the ship is orbiting */
static void strike(int num)
{
    MyShip *s = shipNum(num);
    KPlanet *k;

    if (s == NULL)
    {
        return;
    }
    k = kpAt(s->row, s->col);
    if (k == NULL || k->num < 0)
    {
        return;
    }
    if (!cmd(photonAnswer, NULL, "fire %dp %d", k->num, num))
    {
        return;
    }
    logf_("fired on planet %d (%s)", k->num, k->ownerName);
    if (strstr(Out, "unowned") != NULL || strstr(Out, "wiped out") != NULL)
    {
        k->owner = 0;
    }
    /* look again: if nobody is left it can be settled */
    scanPlanetHere(s);
}

static void actShip(int num)
{
    char path[128];
    int gr, gc, n, guard;
    MyShip *s;

    for (guard = 0; guard < 12 && Btu > ST.btuReserve + 4 && !Lost; guard++)
    {
        KPlanet *here;

        s = shipNum(num);
        if (s == NULL || s->type == 'm')
        {
            return;
        }
        /* a new player's ship starts on the home planet: take settlers
           aboard straight away, while the home planet's bill is small */
        if (s->planet >= 0 && s->planet == HomePlanet && s->civ < 50 &&
            NMine == 0 && guard == 0 && homeBillSmall())
        {
            takeSettlers(num);
            readShips();
            s = shipNum(num);
            if (s == NULL)
            {
                return;
            }
        }
        if (s->eff < 60)
        {
            return;
        }
        if (s->planet >= 0)
        {
            /* on a planet: take on settlers if we can, then go */
            if (s->civ < 50 && canBoard(s->planet) && NumColonies() <
                ST.maxColonies)
            {
                takeSettlers(num);
            }
            else
            {
                (void) cmd(NULL, NULL, "liftoff %d", num);
            }
            readShips();
            continue;
        }
        if (s->energy < 20)
        {
            if (s->ftanks > 0)
            {
                (void) cmd(NULL, NULL, "refuel %d", num);
            }
            return;
        }
        here = onMap(s->row, s->col) && isPlanetChar(MAP(s->row, s->col)) ?
            kpAt(s->row, s->col) : NULL;
        if (here != NULL && MAP(s->row, s->col) != 'H' && (!here->scanned ||
            here->when + 3 < Sessions))
        {
            scanPlanetHere(s);
            here = kpAt(s->row, s->col);
        }
        /* settle where we are */
        if (here != NULL && colonisable(here) && s->civ >= 10)
        {
            settle(num);
            readShips();
            readPlanets();
            continue;
        }
        /* a planet we can't use, unexplored around: look from it */
        if (here != NULL && here->scanned && here->owner == 0 &&
            here->tried < 9 && !colonisable(here) && s->civ >= 1 &&
            (NMine == 0 || Sessions % 4 == 0))
        {
            probe(num);
            readShips();
            continue;
        }
        /* at war and over an enemy planet with torpedoes aboard */
        if (here != NULL && here->owner == 2 && s->photons > 0 && s->mis >= 3
            && war())
        {
            strike(num);
            return;
        }
        /* where next? */
        n = -1;
        if (s->photons > 0 && s->mis >= 6 && war())
        {
            n = findPath(s->row, s->col, goalEnemy, NULL, path, maxSteps(s),
                &gr, &gc, 0);
        }
        if (n < 0 && s->civ >= 10)
        {
            n = findPath(s->row, s->col, goalColony, NULL, path, maxSteps(s),
                &gr, &gc, 0);
        }
        if (n < 0 && s->civ < 10 && NumColonies() < ST.maxColonies)
        {
            n = findPath(s->row, s->col, goalBoarding, NULL, path,
                maxSteps(s), &gr, &gc, 0);
            if (n == 0 || (gr == s->row && gc == s->col))
            {
                takeSettlers(num);
                readShips();
                continue;
            }
        }
        if (n < 0)
        {
            n = findPath(s->row, s->col, goalUnscanned, NULL, path,
                maxSteps(s), &gr, &gc, 0);
        }
        if (n < 0)
        {
            /* nothing known to do: look further out */
            scanFrom(s);
            n = findPath(s->row, s->col, goalFrontier, NULL, path,
                maxSteps(s), &gr, &gc, ST.riskyMoves);
            if (n < 0)
            {
                n = nearestHomeOrColony(s, &gr, &gc, path);
                if (n <= 0)
                {
                    return;
                }
            }
        }
        if (path[0] == '\0')
        {
            /* arrived at a goal cell already */
            if (gr == s->row && gc == s->col && s->civ < 10)
            {
                takeSettlers(num);
                readShips();
            }
            return;
        }
        if (!moveShip(s, path))
        {
            return;
        }
        s = shipNum(num);
        if (s == NULL)
        {
            return;
        }
        scanFrom(s);
        /* landing on the home planet to load settlers */
        if (s->row == HomeRow && s->col == HomeCol && s->civ < 10 &&
            NumColonies() < ST.maxColonies)
        {
            takeSettlers(num);
            readShips();
        }
    }
}

static void manageShips(void)
{
    int nums[MAXSHIPS], n = NShips, i;

    for (i = 0; i < n; i++)
    {
        nums[i] = Ships[i].num;
    }
    for (i = 0; i < n && Btu > ST.btuReserve + 4 && !Lost; i++)
    {
        actShip(nums[i]);
    }
}

/* ------------------------------------------------------------ war */

/* rough strength: our armed ships and planets against what we know of
   the nearest enemy planet */
static int war(void)
{
    int i, armed = 0, enemyMil = 0, known = 0, ourMil = 0;

    if (ST.attackRatio == 0)
    {
        /* the easy empire only strikes back at whoever hit it */
        for (i = 0; i < 64; i++)
        {
            if (AttackedBy[i])
            {
                return 1;
            }
        }
        return 0;
    }
    for (i = 0; i < NShips; i++)
    {
        if (Ships[i].photons > 0 && Ships[i].mis >= 6)
        {
            armed++;
        }
    }
    for (i = 0; i < NMine; i++)
    {
        ourMil += Mine[i].mil + Mine[i].off;
    }
    for (i = 0; i < NKP; i++)
    {
        if (KP[i].owner == 2 && KP[i].cls != 'H')
        {
            known++;
            enemyMil += KP[i].mil;
        }
    }
    if (known == 0 || armed == 0)
    {
        return 0;
    }
    /* settled enough of our own first */
    if (NMine < 3 && Level != L_HARD)
    {
        return 0;
    }
    return ourMil * 100 >= (enemyMil / known + 10) * ST.attackRatio;
}

/* telegrams tell us who has been firing on us */
static void readNews(void)
{
    char *p;

    if (!cmd(NULL, NULL, "read"))
    {
        return;
    }
    /* answer any "delete these telegrams?" question with the default */
    for (p = Out; (p = strstr(p, " fired on ")) != NULL; p++)
    {
        int i;

        for (i = 0; i < NKP; i++)
        {
            size_t len = strlen(KP[i].ownerName);

            if (len > 0 && KP[i].owner == 2 && p - Out >= (long) len &&
                strncmp(p - len, KP[i].ownerName, len) == 0)
            {
                AttackedBy[i % 64] = 1;
            }
        }
    }
}

/* ------------------------------------------------------------ a session */

static void playSession(void)
{
    Sessions++;
    if (Map == NULL || Sessions == 1)
    {
        readInfo();
    }
    if (Map == NULL)
    {
        return;
    }
    readStatus();
    if (MyNumber < 0 && cmd(NULL, NULL, "player"))
    {
        eachLine(findMyNumber, NULL);
    }
    readNews();
    readShips();
    readHome();
    readPlanets();
    readPlanetBig();
    logf_("session %d: %d BTUs, $%ld, %d ships, %d planets", Sessions, Btu,
        Money, NShips, NMine);
    /*
     * Nothing left: no ships and no planets. Like a person whose empire
     * is gone, start again as a new player (remembering the map).
     */
    if (NShips == 0 && NMine == 0 && Sessions > 1)
    {
        static const char *Suffix[3] = { "II", "III", "IV" };

        if (Rebirths < 3)
        {
            snprintf(NameBuf, sizeof(NameBuf), "%s %s", BaseName,
                Suffix[Rebirths]);
            Rebirths++;
            Name = NameBuf;
            HomePlanet = HomeRow = HomeCol = -1;
            MyNumber = -1;
            memset(AttackedBy, 0, sizeof(AttackedBy));
            logf_("empire lost; starting again as %s", Name);
        }
        saveState();
        return;
    }
    /* a long range scan now and then shows black holes and supernovas */
    if (NShips > 0 && Sessions % 5 == 2 && cmd(NULL, NULL, "scan lr %d",
        Ships[0].num))
    {
        parseLRScan();
    }
    /* look around from a few of our planets now and then */
    if (NMine > 0 && Sessions % 5 == 1)
    {
        int i;

        for (i = 0; i < NMine && i < 4; i++)
        {
            scanFromPlanet(Mine[(i + Sessions / 5) % NMine].num);
        }
    }
    managePlanets();
    manageShips();
    saveState();
}

/* how long to wait for the BTUs to come back */
static int sleepSeconds(void)
{
    /* a player gains about 2.5 BTUs each ITU */
    long itus = (96 - ST.btuReserve) * 2 / 5 + 1;
    long secs = itus * SecPerItu * ST.sleepScale / 100;

    if (secs < 5)
    {
        secs = 5;
    }
    if (secs > 6 * 3600)
    {
        secs = 6 * 3600;
    }
    return (int) secs;
}

static void usage(void)
{
    fprintf(stderr, "usage: ImpBot -w PASSWORD [-h HOST] [-p PORT] [-n NAME]"
        " [-c CREATION-PASSWORD]\n              [-l easy|normal|hard] "
        "[-r RACE] [-s STATEFILE] [-L LOGFILE] [-1]\n");
    exit(2);
}

int main(int argc, char *argv[])
{
    int opt, failures = 0;

    while ((opt = getopt(argc, argv, "h:p:n:w:c:l:r:s:L:1")) != -1)
    {
        switch (opt)
        {
            case 'h': Host = optarg; break;
            case 'p': Port = optarg; break;
            case 'n': Name = optarg; BaseName = optarg; break;
            case 'w': Password = optarg; break;
            case 'c': CreatePass = optarg; break;
            case 'r': WantRace = atoi(optarg); break;
            case 's': StateFile = optarg; break;
            case 'L': LogFile = optarg; break;
            case '1': OneSession = 1; break;
            case 'l':
                if (strcmp(optarg, "easy") == 0)
                {
                    Level = L_EASY;
                }
                else if (strcmp(optarg, "normal") == 0)
                {
                    Level = L_NORMAL;
                }
                else if (strcmp(optarg, "hard") == 0)
                {
                    Level = L_HARD;
                }
                else
                {
                    usage();
                }
                break;
            default:
                usage();
        }
    }
    if (Password == NULL)
    {
        usage();
    }
    /* the game cuts longer passwords off and then drops the line */
    if (strlen(Password) > 9 || strlen(CreatePass) > 9)
    {
        fprintf(stderr, "ImpBot: passwords are at most 9 characters\n");
        return 2;
    }
    if (LogFile != NULL)
    {
        Log = fopen(LogFile, "a");
    }
    srandom((unsigned) (time(NULL) ^ getpid()));
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);
    signal(SIGPIPE, SIG_IGN);
    loadState();
    while (!Stopping)
    {
        int wait;

        if (connectGame())
        {
            failures = 0;
            playSession();
            disconnectGame();
        }
        else
        {
            failures++;
            if (LoginFailed)
            {
                logf_("can't log in as %s; giving up", Name);
                return 1;
            }
        }
        if (OneSession)
        {
            break;
        }
        wait = failures > 0 ? (failures < 6 ? 5 * failures : 60) :
            sleepSeconds();
        logf_("sleeping %d seconds", wait);
        while (wait-- > 0 && !Stopping)
        {
            sleep(1);
        }
    }
    saveState();
    logf_("stopped");
    return 0;
}
