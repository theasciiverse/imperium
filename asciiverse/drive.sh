#!/bin/bash
# drive.sh SESSION: a first session as a new player, typed at a human pace
# into `imperium host --alone` running in tmux. Recorded
# asciiverse/imperium.cast against a fresh private universe, never against
# the public game:
#   XDG_STATE_HOME=$(mktemp -d) asciinema rec --window-size 100x30 \
#     --command "imperium host --alone" imperium.cast
S=$1
type_line() { local s=$1 i; for ((i=0; i<${#s}; i++)); do tmux send-keys -t "$S" -l "${s:i:1}"; sleep 0.07; done; sleep 0.3; tmux send-keys -t "$S" Enter; }
sleep 3
type_line Nova; sleep 1
type_line y; sleep 1
type_line add.me; sleep 1
type_line stardust; sleep 0.6
type_line stardust; sleep 2.5
type_line 3; sleep 3
for c in "player" "ship status" "census pop ?civ>0" "census geo ?civ>0" "census prod ?civ>0" "scan sr 0" "headlines"; do
  type_line "$c"; sleep 3
done
sleep 1.5
type_line bye
sleep 2
