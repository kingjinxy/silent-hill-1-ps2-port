#!/bin/sh
# Plays the Silent Hill TAS in BizHawk 2.11.1 with a Lua script. Usage: run_bizhawk.sh [script.lua]
# Setup: docs/TAS Testing Plan.md, Phase 1. Logs go to ~/emu/sh-tas/.
HERE=$(dirname "$(readlink -f "$0")")
LUA=$(readlink -f "${1:-$HERE/synccheck.lua}")
ROOT=$(readlink -f "$HERE/../../..")
cd ~/emu/bizhawk-2.11.1 || exit 1
exec ./EmuHawkMono.sh --mono-no-redirect "--lua=$LUA" --movie=$HOME/emu/sh-tas/sh.bk2 \
    "$ROOT/rom/image/SLUS-00707.cue" > ~/emu/sh-tas/emuhawk.log 2>&1
