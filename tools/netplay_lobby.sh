#!/bin/sh
# Starts the netplay lobby with run/ as MAME's working directory (CLAUDE.md
# rule 11), from wherever it is invoked. Options pass through to the lobby.
#     tools/netplay_lobby.sh [--mame PATH]
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/run"
cd "$root/run" && exec python3 "$root/tools/netplay_lobby.py" "$@"
