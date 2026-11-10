# HomePod mini (1st gen) as the Mac mini's default speaker

Noah's setup, 2026-11-10. The HomePod ("Kitchen", AudioAccessory5,1, under the
monitor) shows up in Sound settings as **Kitchen (HomePod)**, a real CoreAudio
device, so it can be the default output. Apple only allows that natively on
the HomePod mini 2 and HomePod 2.

Measured with the Samson Q2U (`measure_latency.py`): **~200 ms** behind the
Mac mini's own speaker, vs ~2 s for normal AirPlay. The HomePod's internal
buffer sets that floor; no sender can go lower.

## What's here

- The AirBeam HAL plugin (upstream ChenKS12138/AirBeam via boidushya's fork),
  patched on branch `noah`:
  - works with HomePod software 27.x (no `Audio-Latency` header any more)
  - keeps the stream running with silence; the old code let its clock fall
    behind and the HomePod dropped everything after the first pause
  - reconnects when the session drops, but only once the Mac has real audio
    to play, so it doesn't fight a phone that took the HomePod
  - no SIGPIPE crash when the HomePod hangs up; TCP timeouts
  - lists only HomePods, with readable names
  - latency default 0 (`AIRBEAM_DEFAULT_LATENCY`; 11025 gave ~520 ms)
- `install.sh [latency]`: build, install to `/Library/Audio/Plug-Ins/HAL`,
  restart coreaudiod.
- `homepod-default.sh` + LaunchAgent `com.noahairmet.homepod-default`: puts
  output back on the HomePod when its device (re)appears and macOS has fallen
  back to the Mac mini speakers or the monitor. It never overrides AirPods etc.
  Pause with `touch ~/.homepod-default-pause`. Log
  `~/Library/Logs/homepod-default.log`.
- `measure_latency.py`: `uv run noah/measure_latency.py` (beeps on both).
- Plugin log: `/tmp/airbeam.log`.

## Known quirks

- While connected, the Mac streams silence to the HomePod continuously.
- The first sound after a phone took the HomePod is dropped while the Mac
  reconnects.
- Uninstall: `sudo rm -rf /Library/Audio/Plug-Ins/HAL/AirBeamASP.driver &&
  sudo killall coreaudiod`, then
  `launchctl bootout gui/$(id -u)/com.noahairmet.homepod-default`.
