# Changes from upstream

This is the Asciiverse packaging of [Imperium](https://github.com/fstltna/Imperium),
the game of interstellar exploration, trade and war that David Wright
began in 1990 after Chris Gray's Amiga Empire (GPL version 1 or later;
see COPYING). It is built from upstream's `master` at `eefffef`
(6 November 2026, Imperium 1.5pl0). Upstream is the `upstream` remote;
the `asciiverse` branch carries the changes below.

## Ours

- **A game in any folder** (`28ef251`, written to go upstream, not
  offered yet). The server port and the clients' reply pipes lived in
  `FIFO_DIR`, fixed when the game is built. `IMPERIUM_HOME`, when set,
  now names the game directory at run time (`$IMPERIUM_HOME/fifo`, with
  ImpShut's and ImpCtrl's allow files and the server's `nostart` check
  beside it), through `impFifoDir()` in `Include/ImpFifo.h`.
  `IMPERIUM_TELNET_PORT` likewise picks TelImp's TCP port (default 3458).
  Unset, nothing changes.
- **Builds on macOS** (written to go upstream): the five sources that
  include `<crypt.h>` for `crypt()` take `<unistd.h>` on macOS, which has
  no `crypt.h` and declares `crypt()` there (the workflow already links
  without `-lcrypt` on macOS).
- **The launcher** (`asciiverse/imperium.c`), the program av starts:
  - `imperium online` plays the public game at empiredirectory.net,
    port 3458, by talking plain text to its TelImp, and hides what you
    type at password prompts.
  - `imperium host` keeps a universe of the player's own in the state
    folder (`$XDG_STATE_HOME/imperium`, else `~/.local/state/imperium`;
    `~/Library/Application Support/imperium` on macOS). The first time,
    ImpCre makes a 10x10 galaxy for up to sixteen players (eight until
    the computer empires came) with a ten minute Imperium Time Unit, a
    random deity password (in `deity.txt`) and the creation password
    `add.me`. It starts ImpServ and TelImp (so players on the network
    can join; with `--alone` TelImp listens only on 127.0.0.1, for the
    computer empires),
    runs ConImp, and stops both with ImpShut when the host leaves.
  - `imperium join HOST[:PORT]` plays a game someone else hosts.
  - `imperium stop` stops a hosted game; `imperium` alone shows a menu.
  - The game folder must be a short path: a client names its reply pipe
    in a 133-byte request (`Include/Request.h`), so the launcher refuses
    a folder over 100 characters.

- **Computer empires** (`Clients/ImpBot.c`, written to go upstream, not
  offered yet). ImpBot is a computer player that connects to TelImp's TCP
  port and plays by typing commands and reading the replies, exactly as
  a person does: it knows only what its own scans show and is held to
  the same BTUs, money and production. The server is unchanged. It is
  self-contained C (the C library and BSD sockets, no game library or
  secret codes), GPL version 1 or later like the rest of the game, built
  by the Makefile and installed beside TelImp, so it could go upstream
  as it is. Three levels: easy (slow, keeps cash, only strikes back),
  normal and hard (spends every BTU, settles more, attacks sooner).
  What changed in av.6 (from test games on local universes,
  2026-11-15; the plan to 1.0 is `asciiverse/IMPBOT-ROADMAP.md`):
  - **Fuel on every planet.** doc items says fuel tanks need more than
    50% air and 25% water, but the game makes them on any planet people
    can live on (`Library/update.c` has no such rule), so every colony
    makes fuel tanks and ships refuel at any of them. Empires no longer
    stall at one or two planets with a ship they can't move.
  - **Officers.** Officers come only from officer school, which the game
    runs only on a planet whose soldiers are more than 3% of the most
    people it can hold, with officers at least 5% of them; soldiers are
    trained only where officers are at least 1% of the civilians. The
    first ship keeps its five officers until it can start a school on a
    small planet, settled with few people, its soldiers and ore brought
    along (people double with every update, so the chance comes only in
    a new colony's first updates), and the school's soldiers are kept in
    the band where it trains. When no school starts within a dozen
    sessions, the officers go to the warships. Ships trade officers with
    every planet they land on; planets with tubes get one once the
    warships have theirs, and a planet under attack gets two.
  - **Money.** Planets move between three shares of cash (about a third,
    an eighth, a few percent) as money builds up and falls, with room
    between the marks so they don't change back and forth (each change
    costs efficiency); gold mining, which only piles up interest, stops
    when money piles up. A rich empire runs more ships and tubes. A
    planet has at most 18 BTUs and every build costs 5, so ship parts
    are built into store over several sessions and the ship takes them
    aboard when it is built.
  - **Colonies.** A planet that has been fired on gets more tubes and
    officers first; one under attack with almost nobody left is given up
    (nothing more built there, its officers taken off). Normal attacks
    with two armed ships and three planets, or with one once it holds
    five.
  - **Fixes.** Ship reports print thousands with K, which ImpBot read as
    units (big ships took no settlers); a computer empire restarted on an
    old map read the ITU as ten minutes and slept six hours; a player
    left marked as logged on by a broken connection now logs on anyway,
    as a person would; a ship that can't lift off waits instead of
    trying again and again; ships prefer planets whose class is known to
    ones that may be stars.
  What could still be better: most first ships are still lost to stars
  (a single sensor array can't tell a star from a planet), about half
  the empires restart once or twice; an officer school rarely starts;
  computer empires find each other's colonies slowly, so attacks are
  few; defence is passive; money still outruns what there is to buy.
- **TelImp can listen on one address** (written to go upstream, not
  offered yet): `IMPERIUM_TELNET_BIND`, when set, names the IPv4 address
  TelImp listens on (anything it can't read means 127.0.0.1). Unset,
  it listens on every address as before.
- **The launcher's `host` starts computer empires**: three in a new
  universe, `--bots N` (0 to 7) and `--level easy|normal|hard` to
  change them. Their names, passwords and levels are in `bots.txt` in
  the game folder, their maps and logs in `bots/`. They run while the
  game runs and stop with it. With `--alone`, TelImp runs for them on
  127.0.0.1 only.

- **A deity password that works**: the launcher made a 10-character
  deity password, but Imperium keeps at most 9 and drops a connection
  given a longer one, so the one in `deity.txt` could not log in. It is 8
  characters now, like the computer empires' passwords.

## Packaging

- `.github/workflows/asciiverse-release.yml`: static Linux builds
  (x86-64 and ARM64, Alpine, musl) and macOS (x86-64 and ARM64), each
  smoke tested; no Windows (the game talks through named pipes). The
  archive holds `imperium` at the top and the game in `imperium-game/`
  (ImpServ, ImpCre, ConImp, TelImp, ImpShut, ImpCtrl, help, doc).
- `asciiverse/stage.sh` lays out that archive; `asciiverse/smoke.sh`
  hosts a fresh universe from it, joins it over the network as a second
  player, and checks that the host's leaving stops the server.
- `asciiverse/smoke.sh` looks for the universe where the launcher keeps
  it on each system: the state folder on Linux, Application Support on
  macOS (it checked only the Linux path, so macOS failed its smoke test
  while playing correctly).
- `asciiverse/smoke.sh` makes its scratch folder under `IMPSMOKE_TMP`
  (default /tmp), not `TMPDIR`: macOS's TMPDIR is too long a path for a
  game folder.
- `asciiverse/imperium.cast`: the catalog preview, a first session as a
  new player in a fresh private universe, typed by `asciiverse/drive.sh`.
  Never recorded against the public game.
- `asciiverse/README.md`: how to play, shipped in the archive.
