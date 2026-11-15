#!/bin/sh
# smoke.sh DIR: host a fresh universe from a staged tree (stage.sh), join
# it over the network as a second player, and check that both are in the
# game, that the computer empires have joined it, and that the server and
# the computer empires stop when the host leaves.
set -eu
d=$(cd "$1" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/impsmoke.XXXXXX")
export XDG_STATE_HOME="$tmp/state" IMPERIUM_TELNET_PORT=13458
export HOME="$tmp"

# the host sits at ConImp's name prompt while the second player joins
mkfifo "$tmp/hostin"
"$d/imperium" host <"$tmp/hostin" >"$tmp/host.out" 2>&1 &
host=$!
exec 3>"$tmp/hostin"
i=0
until grep -q "Enter player name" "$tmp/host.out" 2>/dev/null; do
  i=$((i + 1)); [ $i -lt 100 ] || { cat "$tmp/host.out"; echo "host did not start" >&2; exit 1; }
  sleep 0.2
done

{ for l in Froggy y add.me frog frog 3 player bye; do sleep 1; echo "$l"; done; sleep 2; } |
  "$d/imperium" join 127.0.0.1:13458 >"$tmp/join.out" 2>&1
cat "$tmp/join.out"
grep -q "Froggy" "$tmp/join.out" && grep -q "Active" "$tmp/join.out" ||
  { echo "the joined player did not get into the game" >&2; exit 1; }
grep -q "Vorlath" "$tmp/join.out" ||
  { echo "the computer empires did not join the game" >&2; exit 1; }

# the host types bye; the launcher stops the server and TelImp
for l in Toady y add.me toad toad 1 bye; do echo "$l" >&3; sleep 1; done
exec 3>&-
wait $host
cat "$tmp/host.out" | tail -5
grep -q "Your universe is saved" "$tmp/host.out" || { echo "host did not finish" >&2; exit 1; }
sleep 1
if pgrep -f "$d/imperium-game/ImpBot" >/dev/null 2>&1; then
  echo "the computer empires kept running after the host left" >&2; exit 1
fi
# the launcher keeps the universe in the state folder on Linux and in
# Application Support on macOS
world="$tmp/state/imperium/data/imp.world"
[ "$(uname -s)" = Darwin ] && world="$tmp/Library/Application Support/imperium/data/imp.world"
[ -f "$world" ] || { echo "no universe at $world" >&2; exit 1; }
rm -rf "$tmp"
echo "smoke test passed"
