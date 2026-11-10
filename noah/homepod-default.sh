#!/usr/bin/env bash
# Puts audio back on the HomePod whenever its AirBeam device (re)appears
# (login, coreaudiod restart, HomePod back on Wi-Fi) and macOS has fallen back
# to a built-in output. It never overrides a choice like AirPods: it only acts
# on the moment the device shows up, and only from the fallback outputs.
# Pause: touch ~/.homepod-default-pause
DEVICE="${HOMEPOD_DEVICE:-Kitchen (HomePod)}"
FALLBACK=("Mac mini Speakers" "LS27H80xEF")
SAS=/opt/homebrew/bin/SwitchAudioSource
was_present=0
last_pid=""
while true; do
  if $SAS -a -t output | grep -qxF "$DEVICE"; then present=1; else present=0; fi
  # A coreaudiod restart re-creates the device faster than we poll, so treat a
  # new coreaudiod as the device reappearing.
  pid=$(pgrep -x coreaudiod)
  [ "$pid" != "$last_pid" ] && was_present=0
  last_pid=$pid
  if [ $present = 1 ] && [ $was_present = 0 ] && [ ! -f "$HOME/.homepod-default-pause" ]; then
    cur=$($SAS -c -t output)
    for f in "${FALLBACK[@]}"; do
      if [ "$cur" = "$f" ]; then
        $SAS -s "$DEVICE" -t output >/dev/null && $SAS -s "$DEVICE" -t system >/dev/null
        echo "$(date '+%F %T') switched $cur -> $DEVICE"
      fi
    done
  fi
  was_present=$present
  sleep 3
done
