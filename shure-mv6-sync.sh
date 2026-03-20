#!/bin/bash
# Bidirectional mute sync for Shure MV6:
#   hardware button → sysfs file → pactl (hardware→OS)
#   pactl/pipewire mute change → sysfs file (OS→hardware)

MUTE_FILE="/sys/bus/hid/devices/0003:14ED:1026.0007/mute"
SOURCE="alsa_input.usb-Shure_Inc_Shure_MV6_MV6_5-edba00d224cc9b57b966e8f75001686b-01.mono-fallback"

# Lock flag to prevent feedback loops
SYNCING=0

set_hw_mute() {
    echo "$1" > "$MUTE_FILE"
}

set_os_mute() {
    pactl set-source-mute "$SOURCE" "$1"
}

get_os_mute() {
    pactl get-source-mute "$SOURCE" 2>/dev/null | grep -c "yes"
}

# Watch hardware sysfs file → sync to OS
watch_hw() {
    local last=""
    while true; do
        current=$(cat "$MUTE_FILE" 2>/dev/null)
        if [ "$current" != "$last" ] && [ -n "$current" ]; then
            if [ "$SYNCING" -eq 0 ]; then
                SYNCING=1
                echo "[hw→os] mute=$current"
                set_os_mute "$current"
                SYNCING=0
            fi
            last="$current"
        fi
        sleep 0.2
    done
}

# Watch pactl subscribe → sync to hardware
watch_os() {
    pactl subscribe | grep --line-buffered "source" | while read -r line; do
        if echo "$line" | grep -q "change"; then
            if [ "$SYNCING" -eq 0 ]; then
                current=$(get_os_mute)
                hw=$(cat "$MUTE_FILE" 2>/dev/null)
                if [ "$current" != "$hw" ]; then
                    SYNCING=1
                    echo "[os→hw] mute=$current"
                    set_hw_mute "$current"
                    SYNCING=0
                fi
            fi
        fi
    done
}

echo "Shure MV6 mute sync started"
watch_hw &
HW_PID=$!
watch_os &
OS_PID=$!

trap "kill $HW_PID $OS_PID 2>/dev/null; exit" INT TERM
wait
