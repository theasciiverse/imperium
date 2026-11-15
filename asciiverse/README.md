# Imperium for Asciiverse

Imperium is a game of interstellar exploration, trade and war, played by
many people at once on a server that keeps the universe running. This
build adds `imperium`, a launcher with three ways in:

- `imperium online`: the public game at empiredirectory.net, port 3458
- `imperium host`: a universe of your own, kept in your state folder
  (`~/.local/state/imperium`, or `~/Library/Application Support/imperium`
  on macOS), with computer empires to play against, open to players on
  your network; `imperium host --alone` keeps it to this computer
- `imperium join HOST[:PORT]`: a game someone else hosts

A new universe starts with three computer empires. They log in over the
network the way any player does, see only what their own scans show, and
are bound by the same BTUs, money and production as you: they explore,
settle planets, build ships and torpedo tubes, defend what they hold and,
once strong enough, attack their neighbours. They play while your game is
running and stop when you leave. Choose how many and how they play:

    imperium host --bots 5            five computer empires (0 to 7)
    imperium host --level hard        easy, normal or hard

Their names, passwords and levels are in `bots.txt` in the game folder,
their maps and logs in `bots/`.

Run `imperium` alone for a menu. New players join a game with the
creation password `add.me`. In the game, `help` lists the commands and
`doc intro` is the place to start.

The game is in `imperium-game/` beside the launcher. Upstream is
https://github.com/fstltna/Imperium (remote `upstream`); what Asciiverse
changes is listed in `PATCHES.md`.
