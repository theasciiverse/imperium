# Imperium for Asciiverse

Imperium is a game of interstellar exploration, trade and war, played by
many people at once on a server that keeps the universe running. This
build adds `imperium`, a launcher with three ways in:

- `imperium online`: the public game at empiredirectory.net, port 3458
- `imperium host`: a universe of your own, kept in your state folder
  (`~/.local/state/imperium`, or `~/Library/Application Support/imperium`
  on macOS), open to players on your network; `imperium host --alone`
  keeps it to this computer
- `imperium join HOST[:PORT]`: a game someone else hosts

Run `imperium` alone for a menu. New players join a game with the
creation password `add.me`. In the game, `help` lists the commands and
`doc intro` is the place to start.

The game is in `imperium-game/` beside the launcher. Upstream is
https://github.com/fstltna/Imperium (remote `upstream`); what Asciiverse
changes is listed in `PATCHES.md`.
