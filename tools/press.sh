#!/usr/bin/env bash
# press.sh <tokens> [hold_seconds]
f=/home/deadinside/Games/bloodborne/game_files/native_probe/out/pad
echo "$1" > $f; sleep ${2:-0.3}; : > $f
