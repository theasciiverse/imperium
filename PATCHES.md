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
    ImpCre makes a 10x10 galaxy for up to eight players with a ten
    minute Imperium Time Unit, a random deity password (in `deity.txt`)
    and the creation password `add.me`. It starts ImpServ and TelImp
    (so players on the network can join; `--alone` leaves TelImp out),
    runs ConImp, and stops both with ImpShut when the host leaves.
  - `imperium join HOST[:PORT]` plays a game someone else hosts.
  - `imperium stop` stops a hosted game; `imperium` alone shows a menu.
  - The game folder must be a short path: a client names its reply pipe
    in a 133-byte request (`Include/Request.h`), so the launcher refuses
    a folder over 100 characters.

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
- `asciiverse/imperium.cast`: the catalog preview, a first session as a
  new player in a fresh private universe, typed by `asciiverse/drive.sh`.
  Never recorded against the public game.
- `asciiverse/README.md`: how to play, shipped in the archive.
