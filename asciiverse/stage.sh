#!/bin/sh
# stage.sh DIR: lay out a built tree the way the release archive holds it:
# the launcher at the top, where av looks for it, and the game beside it.
set -eu
d=$1
rm -rf "$d"
mkdir -p "$d/imperium-game/help" "$d/imperium-game/doc"
cp imperium COPYING "$d/"
cp asciiverse/README.md "$d/README.md"
cp Clients/ConImp Clients/TelImp Servers/ImpServ Servers/ImpShut \
   Servers/ImpCtrl ImpCre/ImpCre "$d/imperium-game/"
cp Help/*.hlp "$d/imperium-game/help/"
cp Doc/*.doc "$d/imperium-game/doc/"
