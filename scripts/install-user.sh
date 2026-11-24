#!/bin/sh
# Link MBot's commands into ~/.local/bin (make sure that's on your PATH).
#   mbot   - the robot tool / brain (build first: cmake --build build)
#   vtalk  - talk to the running brain: conversation, or /maintenance commands
#   vssh   - root SSH to the robot (only if you created ~/.config/mbot/vssh, see docs/ROBOT_MODIFICATIONS.md)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
bin="$HOME/.local/bin"
mkdir -p "$bin"
ln -sfn "$here/build/mbot" "$bin/mbot"
ln -sfn "$here/tools/vtalk.py" "$bin/vtalk"
[ -x "$HOME/.config/mbot/vssh" ] && ln -sfn "$HOME/.config/mbot/vssh" "$bin/vssh"
echo "installed into $bin:"; ls -la "$bin" | grep -E ' (mbot|vtalk|vssh) '
case ":$PATH:" in *":$bin:"*) ;; *) echo "note: add $bin to your PATH";; esac
