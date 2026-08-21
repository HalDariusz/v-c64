#!/bin/bash
# c64-run.sh - builds and runs the whole solution, then kvm_host (no
# debugger, directly). See README.md, section "Running with preview and
# sound (c64-run.sh)".
#
# If kvm_host was built with SDL2 (see Makefile - requires libsdl2-dev),
# this is the only thing this script does: SDL2 then provides a REAL
# interactive window - video, keyboard, and sound together in one place,
# with no extra juggling of terminals/preview/aplay.
#
# Without SDL2 (headless fallback), the script recreates the same
# experience from separate pieces: a new terminal window for typing on
# the keyboard, ImageMagick's "display -update" for the video preview,
# "aplay" on a FIFO for live audio.
#
# Usage: ./c64-run.sh [disk_directory]   (default: disk)
#
# Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
# on Linux /dev/kvm, with no QEMU involved.
#
# Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>

set -e
cd "$(dirname "$0")"

DISK_DIR="${1:-disk}"

echo "[c64-run] building..."
make all

mkdir -p build "$DISK_DIR"
sudo chmod a+rw /dev/kvm 2>/dev/null || true

if ldd kvm_host 2>/dev/null | grep -q libSDL2; then
    echo "[c64-run] kvm_host has SDL2 - opening a single interactive window (video + keyboard + audio)."
    exec ./kvm_host build/c64_guest.bin "$DISK_DIR"
fi

echo "[c64-run] kvm_host without SDL2 (libsdl2-dev was missing at build time) - headless fallback:"
echo "[c64-run]   new terminal window (keyboard) + display -update (video) + aplay (audio)."

BG_PIDS=()
cleanup() {
    for pid in "${BG_PIDS[@]}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    rm -f build/audio.fifo
}
trap cleanup EXIT

# --- live audio: FIFO + aplay. Must start BEFORE kvm_host, since
# kvm_host opens the FIFO for writing in non-blocking mode - that only
# succeeds if a reader (aplay) is already waiting on the other end.
# Without this step, the run is still correct, just without live
# playback (a complete build/audio.wav is always produced after the run
# ends anyway - see README). ---
if command -v aplay >/dev/null 2>&1; then
    rm -f build/audio.fifo
    mkfifo build/audio.fifo
    aplay -q -t raw -f S16_LE -r 44100 -c 1 build/audio.fifo >/dev/null 2>&1 &
    BG_PIDS+=("$!")
    echo "[c64-run] live audio: aplay on build/audio.fifo (PID $!)"
else
    echo "[c64-run] 'aplay' not found - skipping live audio (only build/audio.wav will remain)"
fi

# --- live video preview: ImageMagick "display -update", refreshes
# itself when build/frame.ppm changes. ---
if command -v display >/dev/null 2>&1 && [ -n "$DISPLAY" ]; then
    ( sleep 1; exec display -update 0.5 build/frame.ppm ) &
    BG_PIDS+=("$!")
    echo "[c64-run] live video preview: display -update (PID $!)"
else
    echo "[c64-run] 'display' (ImageMagick) unavailable, or \$DISPLAY not set - skipping live video preview"
fi

# --- kvm_host in a new terminal window (for typing on the C64 keyboard) ---
RUN_CMD="./kvm_host build/c64_guest.bin '$DISK_DIR'; echo; echo '[c64-run] guest has shut down. Press Enter to close the window.'; read"

TERM_BIN=""
for t in gnome-terminal xterm alacritty x-terminal-emulator; do
    if command -v "$t" >/dev/null 2>&1; then TERM_BIN="$t"; break; fi
done

if [ -z "$TERM_BIN" ]; then
    echo "[c64-run] no known terminal emulator found in PATH - running kvm_host here, in the current window."
    bash -c "$RUN_CMD"
    exit 0
fi

echo "[c64-run] opening kvm_host in a new window ($TERM_BIN)..."
case "$TERM_BIN" in
    gnome-terminal)
        gnome-terminal --wait --title="v-c64" -- bash -c "$RUN_CMD"
        ;;
    xterm|x-terminal-emulator)
        "$TERM_BIN" -T "v-c64" -e bash -c "$RUN_CMD"
        ;;
    alacritty)
        alacritty -T "v-c64" -e bash -c "$RUN_CMD"
        ;;
esac
