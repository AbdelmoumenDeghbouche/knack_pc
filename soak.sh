#!/usr/bin/env bash
# soak2.sh <secs>: enters the level, rotates the camera now and then, reports survival.
cd /home/deadinside/Games/bloodborne/game_files/native_probe
unset BB_GPU_PROFILE BB_PRESET_FILE BB_SCENE_DEBUG
bash "${RESTART:?set RESTART to a script that starts the game and enters the level}" > /dev/null
end=$((SECONDS + $1))
while ((SECONDS < end)); do
    ./press.sh "rx=60" 2; sleep 8
    ./press.sh "rx=195" 2; sleep 8
    if ! pgrep -x bb-probe > /dev/null; then
        echo "DIED after $((SECONDS)) s: $(grep -m1 -E 'fault|Assert|Fault' out/session.log | cut -c1-160)"; exit 1
    fi
done
echo "alive after $1 s: $(grep '^Frame stats' out/session.log | tail -1 | cut -c1-40)"
