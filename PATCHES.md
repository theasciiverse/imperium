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
  What could still be better (from test games on local universes,
  2026-11-14/15):
  - A starting ship's single sensor array can't tell a star from a
    planet next to it, and about four in ten of those are stars: many
    computer empires lose their first ship this way (people do too).
    They start again as "Name II" while there is room in the game.
  - Fuel decides everything: only class D and M planets with enough air
    and water make fuel tanks, and an empire that settles none of them
    soon has ships it can't move. Those empires stay at one to three
    planets; the ones that find fuel grow to fifteen or more planets and
    ten ships in a game day or two.
  - Officers are scarce (five per player) and a ship or planet needs one
    to fire, so torpedo tubes on planets mostly can't fire back and
    only a few warships can attack. Hard and normal empires do attack
    colonies they find; most torpedoes miss.
  - Money piles up far beyond what is spent; it could buy more ships,
    research and technology.
  - A colony is never given up or defended actively (moving soldiers
    and officers to a threatened planet).
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
