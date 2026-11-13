/*
 * imperium - the Asciiverse launcher for Imperium.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * Imperium is played on a server that keeps the universe and runs it in
 * real time. This one program gets a player into a game three ways:
 *
 *   imperium                  a menu of the three below
 *   imperium online           the public game at empiredirectory.net
 *   imperium host             a game of your own on this computer, which
 *                             players on your network can join too
 *   imperium join HOST[:PORT] a game someone hosts (port 3458 by default)
 *   imperium stop             stop the game this computer hosts
 *
 * Online and join speak plain text over TCP to TelImp, the game's own
 * network front end. Host keeps the universe in the player's state folder
 * (IMPERIUM_HOME, see Include/ImpFifo.h), starts ImpServ and TelImp from
 * the game folder beside this program, and runs ConImp, the console
 * client. The universe stays between sessions; the game (server and
 * network front end) stops when the host leaves.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define ONLINE_HOST "empiredirectory.net"
#define DEFAULT_PORT "3458"

static char GameDir[PATH_MAX];   /* help/, doc/ and the programs */
static char Home[PATH_MAX];      /* the hosted universe */

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("imperium: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ---------------------------------------------------------------- paths */

static void findGameDir(void)
{
    char self[PATH_MAX], *slash;
    ssize_t n;

#ifdef __APPLE__
    uint32_t size = sizeof(self);
    char buf[PATH_MAX];

    if (_NSGetExecutablePath(buf, &size) != 0 || realpath(buf, self) == NULL)
    {
        die("can't find where I am installed");
    }
    n = (ssize_t) strlen(self);
#else
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0)
    {
        die("can't find where I am installed");
    }
    self[n] = '\0';
#endif
    slash = strrchr(self, '/');
    if (slash != NULL)
    {
        *slash = '\0';
    }
    snprintf(GameDir, sizeof(GameDir), "%s/imperium-game", self);
}

static void mkdirs(const char *path, mode_t mode)
{
    char tmp[PATH_MAX], *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            (void) mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
    {
        die("can't create %s: %s", tmp, strerror(errno));
    }
}

static void findHome(void)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0')
    {
        struct passwd *pw = getpwuid(getuid());

        home = (pw != NULL) ? pw->pw_dir : "/tmp";
    }
#ifdef __APPLE__
    snprintf(Home, sizeof(Home), "%s/Library/Application Support/imperium",
        home);
    (void) state;
#else
    if (state != NULL && state[0] == '/')
    {
        snprintf(Home, sizeof(Home), "%s/imperium", state);
    }
    else
    {
        snprintf(Home, sizeof(Home), "%s/.local/state/imperium", home);
    }
#endif
    /* a client tells the server the path of its reply pipe in a request
       of 133 bytes (Include/Request.h), so the game folder must leave
       room for "/fifo/iconXXXXXXXX" */
    if (strlen(Home) > 100)
    {
        die("the game folder %s is too long a path for Imperium; set "
            "XDG_STATE_HOME to a shorter folder", Home);
    }
    setenv("IMPERIUM_HOME", Home, 1);
}

static void pathIn(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

/* --------------------------------------------------------- the network */

static struct termios SavedTty;
static int TtySaved = 0;

static void echoOn(void)
{
    if (TtySaved)
    {
        (void) tcsetattr(0, TCSANOW, &SavedTty);
    }
}

static void echoOff(void)
{
    struct termios t;

    if (TtySaved)
    {
        t = SavedTty;
        t.c_lflag &= ~(tcflag_t) ECHO;
        (void) tcsetattr(0, TCSANOW, &t);
    }
}

static void onSignal(int sig)
{
    echoOn();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* does the text the server has sent since its last newline ask for a
   password? */
static int asksPassword(const char *line)
{
    char low[512];
    size_t i;

    for (i = 0; line[i] != '\0' && i < sizeof(low) - 1; i++)
    {
        low[i] = (char) tolower((unsigned char) line[i]);
    }
    low[i] = '\0';
    return strstr(low, "password") != NULL;
}

static int play(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    struct pollfd fds[2];
    char buf[4096], partial[512];
    size_t plen = 0;
    int sock = -1, err, hidden = 0;
    ssize_t n, i;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    printf("Connecting to %s, port %s...\n", host, port);
    fflush(stdout);
    if ((err = getaddrinfo(host, port, &hints, &res)) != 0)
    {
        die("can't find %s: %s", host, gai_strerror(err));
    }
    for (ai = res; ai != NULL; ai = ai->ai_next)
    {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
        {
            continue;
        }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
        {
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0)
    {
        die("can't reach a game at %s, port %s: %s", host, port,
            strerror(errno));
    }
    if (isatty(0) && tcgetattr(0, &SavedTty) == 0)
    {
        TtySaved = 1;
        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);
        signal(SIGHUP, onSignal);
    }
    signal(SIGPIPE, SIG_IGN);

    fds[0].fd = sock;
    fds[0].events = POLLIN;
    fds[1].fd = 0;
    fds[1].events = POLLIN;
    partial[0] = '\0';
    for (;;)
    {
        if (poll(fds, 2, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
        {
            n = read(sock, buf, sizeof(buf));
            if (n <= 0)
            {
                break;
            }
            /* the game sends plain text; drop any telnet commands and the
               carriage returns of its line ends */
            for (i = 0; i < n; i++)
            {
                unsigned char c = (unsigned char) buf[i];

                if (c == 255 && i + 1 < n)
                {
                    i += (buf[i + 1] >= (char) 251) ? 2 : 1;
                    continue;
                }
                if (c == '\r')
                {
                    continue;
                }
                putchar(c);
                if (c == '\n')
                {
                    plen = 0;
                }
                else if (plen < sizeof(partial) - 1)
                {
                    partial[plen++] = (char) c;
                }
            }
            partial[plen] = '\0';
            fflush(stdout);
            if (!hidden && plen > 0 && asksPassword(partial))
            {
                echoOff();
                hidden = 1;
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP))
        {
            n = read(0, buf, sizeof(buf));
            if (n <= 0)
            {
                break;
            }
            if (hidden)
            {
                echoOn();
                hidden = 0;
                putchar('\n');
                fflush(stdout);
            }
            if (write(sock, buf, (size_t) n) != n)
            {
                break;
            }
        }
    }
    echoOn();
    close(sock);
    printf("\nDisconnected from %s.\n", host);
    return 0;
}

/* ------------------------------------------------------------ hosting */

static int portListening(void)
{
    char port[PATH_MAX];
    int fd;

    snprintf(port, sizeof(port), "%s/fifo/server/imperium.port", Home);
    fd = open(port, O_WRONLY | O_NONBLOCK);
    if (fd >= 0)
    {
        close(fd);
        return 1;
    }
    return 0;
}

/* the server's pid, from server.pid, if that process is still alive */
static pid_t serverPid(void)
{
    char path[PATH_MAX];
    long p = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/server.pid", Home);
    if ((f = fopen(path, "r")) == NULL)
    {
        return 0;
    }
    if (fscanf(f, "%ld", &p) != 1)
    {
        p = 0;
    }
    fclose(f);
    if (p > 1 && kill((pid_t) p, 0) == 0)
    {
        return (pid_t) p;
    }
    (void) unlink(path);
    return 0;
}

static void linkFresh(const char *name)
{
    char from[PATH_MAX], to[PATH_MAX];

    pathIn(from, sizeof(from), GameDir, name);
    pathIn(to, sizeof(to), Home, name);
    (void) unlink(to);
    if (symlink(from, to) != 0)
    {
        die("can't link %s: %s", to, strerror(errno));
    }
}

static void writeAllow(const char *name, const char *user)
{
    char path[PATH_MAX];
    FILE *f;

    pathIn(path, sizeof(path), Home, name);
    if ((f = fopen(path, "w")) == NULL)
    {
        die("can't write %s: %s", path, strerror(errno));
    }
    fprintf(f, "# written by the imperium launcher: who may run %s\n%s all\n",
        strcmp(name, "is.allow") == 0 ? "ImpShut" : "ImpCtrl", user);
    fclose(f);
}

static pid_t spawn(const char *dir, const char *log, char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
    {
        die("can't start %s: %s", argv[0], strerror(errno));
    }
    if (pid == 0)
    {
        int fd;

        if (chdir(dir) != 0)
        {
            _exit(127);
        }
        (void) setsid();
        fd = open(log != NULL ? log : "/dev/null",
            O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0)
        {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        fd = open("/dev/null", O_RDONLY);
        if (fd >= 0)
        {
            dup2(fd, 0);
            close(fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static void program(char *out, size_t size, const char *name)
{
    pathIn(out, size, GameDir, name);
    if (access(out, X_OK) != 0)
    {
        die("%s is missing; reinstall Imperium", out);
    }
}

static void createUniverse(const char *data)
{
    char impcre[PATH_MAX], pw[16], path[PATH_MAX];
    const char *letters = "abcdefghjkmnpqrstuvwxyz23456789";
    pid_t pid;
    int status, i;
    FILE *f;

    program(impcre, sizeof(impcre), "ImpCre");
    srand((unsigned) (time(NULL) ^ getpid()));
    for (i = 0; i < 10; i++)
    {
        pw[i] = letters[rand() % (int) strlen(letters)];
    }
    pw[10] = '\0';
    printf("Creating a new universe...\n");
    fflush(stdout);

    /* ImpCre reads its settings from the environment (ImpCre/default.sh
       lists them all). A ten by ten galaxy of galactic sectors for up to
       eight players, with a ten minute Imperium Time Unit, so a game
       moves along in days rather than months. */
    setenv("DoNotAsk", "1", 1);
    setenv("GodPassword", pw, 1);
    setenv("CreationPassword", "add.me", 1);
    setenv("HorizSize", "10", 1);
    setenv("VertSize", "10", 1);
    setenv("NumPlay", "9", 1);
    setenv("ConTime", "1440", 1);
    setenv("SecItu", "600", 1);
    setenv("StartingCash", "5000", 1);
    setenv("ChangePlayers", "1", 1);
    setenv("PublicMessages", "1", 1);
    setenv("Name", "Asciiverse private game", 1);
    pid = fork();
    if (pid == 0)
    {
        if (chdir(data) != 0)
        {
            _exit(127);
        }
        execl(impcre, "ImpCre", (char *) NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
    {
        die("ImpCre could not create the universe");
    }
    pathIn(path, sizeof(path), Home, "deity.txt");
    if ((f = fopen(path, "w")) != NULL)
    {
        fprintf(f, "The deity (game master) of this universe is player \"god\""
            ", password %s.\nNew players join with the creation password "
            "add.me.\n", pw);
        fclose(f);
        (void) chmod(path, 0600);
    }
}

static int stopServer(int quiet)
{
    char impshut[PATH_MAX], pidfile[PATH_MAX];
    pid_t pid, server;
    int status, waited;
    FILE *f;

    pathIn(pidfile, sizeof(pidfile), Home, "telimp.pid");
    if ((f = fopen(pidfile, "r")) != NULL)
    {
        long p = 0;

        if (fscanf(f, "%ld", &p) == 1 && p > 1)
        {
            (void) kill(-(pid_t) p, SIGTERM);
        }
        fclose(f);
        (void) unlink(pidfile);
    }
    server = serverPid();
    if (server == 0)
    {
        if (!quiet)
        {
            printf("No game is running here.\n");
        }
        return 0;
    }
    /* ImpShut asks the server to flush its files and exit; it can miss
       while the server reopens its port, so ask until it goes */
    program(impshut, sizeof(impshut), "ImpShut");
    for (waited = 0; waited < 100 && kill(server, 0) == 0; waited++)
    {
        if (waited % 10 == 0)
        {
            pid = fork();
            if (pid == 0)
            {
                int fd = open("/dev/null", O_WRONLY);

                if (fd >= 0)
                {
                    dup2(fd, 1);
                    dup2(fd, 2);
                    close(fd);
                }
                execl(impshut, "ImpShut", (char *) NULL);
                _exit(127);
            }
            if (pid > 0)
            {
                (void) waitpid(pid, &status, 0);
            }
        }
        (void) waitpid(server, &status, WNOHANG);
        usleep(100000);
    }
    if (kill(server, 0) == 0)
    {
        (void) kill(server, SIGTERM);
    }
    pathIn(pidfile, sizeof(pidfile), Home, "server.pid");
    (void) unlink(pidfile);
    if (!quiet)
    {
        printf("Imperium server stopped.\n");
    }
    return 0;
}

static int host(int lan)
{
    char data[PATH_MAX], path[PATH_MAX], log[PATH_MAX], impserv[PATH_MAX],
        telimp[PATH_MAX], conimp[PATH_MAX], fifo[PATH_MAX];
    const char *user;
    struct passwd *pw;
    pid_t pid;
    int status, i;
    FILE *f;

    program(impserv, sizeof(impserv), "ImpServ");
    program(telimp, sizeof(telimp), "TelImp");
    program(conimp, sizeof(conimp), "ConImp");

    pathIn(data, sizeof(data), Home, "data");
    pathIn(fifo, sizeof(fifo), Home, "fifo");
    mkdirs(data, 0700);
    mkdirs(fifo, 01777);
    (void) chmod(fifo, 01777);
    pathIn(path, sizeof(path), fifo, "server");
    mkdirs(path, 0755);
    linkFresh("help");
    linkFresh("doc");
    pw = getpwuid(geteuid());
    user = (pw != NULL) ? pw->pw_name : "all";
    writeAllow("is.allow", user);
    writeAllow("ic.allow", user);

    pathIn(path, sizeof(path), data, "imp.world");
    if (access(path, F_OK) != 0)
    {
        createUniverse(data);
    }

    pathIn(log, sizeof(log), Home, "server.log");
    if (serverPid() == 0)
    {
        char *argv[] = { impserv, NULL };

        pid = spawn(data, log, argv);
        pathIn(path, sizeof(path), Home, "server.pid");
        if ((f = fopen(path, "w")) != NULL)
        {
            fprintf(f, "%ld\n", (long) pid);
            fclose(f);
        }
        for (i = 0; i < 50 && !portListening(); i++)
        {
            usleep(100000);
        }
        if (!portListening())
        {
            die("the Imperium server did not start; see %s", log);
        }
    }
    pathIn(path, sizeof(path), Home, "telimp.pid");
    if (lan && access(path, F_OK) != 0)
    {
        char *argv[] = { telimp, NULL };
        const char *port = getenv("IMPERIUM_TELNET_PORT");

        pid = spawn(Home, log, argv);
        if ((f = fopen(path, "w")) != NULL)
        {
            fprintf(f, "%ld\n", (long) pid);
            fclose(f);
        }
        printf("Players on your network can join with:  imperium join "
            "<this computer>%s%s\n", port != NULL ? ":" : "",
            port != NULL ? port : "");
    }
    printf("New players join with the creation password add.me. The deity "
        "password is in\n%s/deity.txt.\n\n", Home);
    fflush(stdout);

    pid = fork();
    if (pid == 0)
    {
        if (chdir(Home) != 0)
        {
            _exit(127);
        }
        execl(conimp, "ConImp", (char *) NULL);
        _exit(127);
    }
    if (pid > 0)
    {
        (void) waitpid(pid, &status, 0);
    }
    stopServer(1);
    printf("Your universe is saved in %s.\n", Home);
    return 0;
}

/* ----------------------------------------------------------------- menu */

static int join(const char *where)
{
    char host[256], *colon;
    const char *port = DEFAULT_PORT;

    snprintf(host, sizeof(host), "%s", where);
    colon = strrchr(host, ':');
    if (colon != NULL && strchr(host, ':') == colon)
    {
        *colon = '\0';
        port = colon + 1;
    }
    return play(host, port);
}

static void usage(void)
{
    puts("Imperium: interstellar exploration, trade and war.\n\n"
         "  imperium                  choose from a menu\n"
         "  imperium online           the public game at " ONLINE_HOST "\n"
         "  imperium host             a game of your own on this computer,\n"
         "                            open to players on your network\n"
         "  imperium host --alone     the same, without the network\n"
         "  imperium join HOST[:PORT] a game someone hosts (port 3458)\n"
         "  imperium stop             stop the game this computer hosts");
}

static int menu(void)
{
    char line[256], *p;

    puts("\n  I M P E R I U M\n"
         "  interstellar exploration, trade and war, since 1990\n\n"
         "  1  Play online at " ONLINE_HOST "\n"
         "  2  Host a game on this computer (players on your network can join)\n"
         "  3  Join a game on another computer\n"
         "  q  Quit\n");
    for (;;)
    {
        printf("Choose 1, 2, 3 or q: ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
        {
            return 0;
        }
        for (p = line; isspace((unsigned char) *p); p++)
        {
        }
        switch (tolower((unsigned char) *p))
        {
            case '1':
                return play(ONLINE_HOST, DEFAULT_PORT);
            case '2':
                return host(1);
            case '3':
                printf("Computer name or address (add :PORT if not 3458): ");
                fflush(stdout);
                if (fgets(line, sizeof(line), stdin) == NULL)
                {
                    return 0;
                }
                line[strcspn(line, " \r\n")] = '\0';
                if (line[0] != '\0')
                {
                    return join(line);
                }
                break;
            case 'q':
                return 0;
            default:
                break;
        }
    }
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    findGameDir();
    findHome();
    if (argc < 2)
    {
        return menu();
    }
    if (strcmp(argv[1], "online") == 0)
    {
        return play(ONLINE_HOST, DEFAULT_PORT);
    }
    if (strcmp(argv[1], "host") == 0)
    {
        return host(!(argc > 2 && strcmp(argv[2], "--alone") == 0));
    }
    if (strcmp(argv[1], "join") == 0 && argc > 2)
    {
        return join(argv[2]);
    }
    if (strcmp(argv[1], "stop") == 0)
    {
        return stopServer(0);
    }
    usage();
    return strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
        ? 0 : 2;
}
