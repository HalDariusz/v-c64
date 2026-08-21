#!/bin/bash
# c64-run.sh - buduje i uruchamia cale rozwiazanie, potem kvm_host (bez
# debugera, bezposrednio). Patrz README.md, sekcja "Uruchomienie z
# podgladem i dzwiekiem (c64-run.sh)".
#
# Jesli kvm_host zbudowal sie z SDL2 (patrz Makefile - wymaga libsdl2-dev),
# to jest to jedyne, co robi ten skrypt: SDL2 daje wtedy PRAWDZIWE
# interaktywne okno - obraz, klawiatura i dzwiek razem w jednym miejscu,
# bez zadnego dodatkowego zonglowania terminalami/podgladem/aplayem.
#
# Bez SDL2 (headless fallback) skrypt odtwarza to samo doswiadczenie z
# oddzielnych klockow: nowe okno terminala do wpisywania na klawiaturze,
# ImageMagick "display -update" do podgladu obrazu, "aplay" na FIFO do
# dzwieku na zywo.
#
# Uzycie: ./c64-run.sh [katalog_dyskietki]   (domyslnie: disk)

set -e
cd "$(dirname "$0")"

DISK_DIR="${1:-disk}"

echo "[c64-run] budowanie..."
make all

mkdir -p build "$DISK_DIR"
sudo chmod a+rw /dev/kvm 2>/dev/null || true

if ldd kvm_host 2>/dev/null | grep -q libSDL2; then
    echo "[c64-run] kvm_host ma SDL2 - otwieram jedno interaktywne okno (obraz + klawiatura + dzwiek)."
    exec ./kvm_host build/c64_guest.bin "$DISK_DIR"
fi

echo "[c64-run] kvm_host bez SDL2 (brak libsdl2-dev przy budowaniu) - headless fallback:"
echo "[c64-run]   nowe okno terminala (klawiatura) + display -update (obraz) + aplay (dzwiek)."

BG_PIDS=()
cleanup() {
    for pid in "${BG_PIDS[@]}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    rm -f build/audio.fifo
}
trap cleanup EXIT

# --- zywy dzwiek: FIFO + aplay. Musi wystartowac PRZED kvm_host, bo
# kvm_host otwiera FIFO do zapisu w trybie nonblock - to sie uda tylko
# jesli po drugiej stronie juz czeka czytelnik (aplay). Bez tego kroku
# dzialanie i tak jest poprawne, tylko bez zywego odsluchu (jest za to
# zawsze kompletny build/audio.wav po zakonczeniu - patrz README). ---
if command -v aplay >/dev/null 2>&1; then
    rm -f build/audio.fifo
    mkfifo build/audio.fifo
    aplay -q -t raw -f S16_LE -r 44100 -c 1 build/audio.fifo >/dev/null 2>&1 &
    BG_PIDS+=("$!")
    echo "[c64-run] zywy dzwiek: aplay na build/audio.fifo (PID $!)"
else
    echo "[c64-run] 'aplay' nie znaleziony - pomijam zywy dzwiek (zostanie tylko build/audio.wav)"
fi

# --- zywy podglad obrazu: ImageMagick "display -update", odswieza sam
# siebie gdy build/frame.ppm sie zmieni. ---
if command -v display >/dev/null 2>&1 && [ -n "$DISPLAY" ]; then
    ( sleep 1; exec display -update 0.5 build/frame.ppm ) &
    BG_PIDS+=("$!")
    echo "[c64-run] zywy podglad obrazu: display -update (PID $!)"
else
    echo "[c64-run] 'display' (ImageMagick) niedostepny albo brak \$DISPLAY - pomijam zywy podglad obrazu"
fi

# --- kvm_host w nowym oknie terminala (do wpisywania na klawiaturze C64) ---
RUN_CMD="./kvm_host build/c64_guest.bin '$DISK_DIR'; echo; echo '[c64-run] gosc zakonczyl dzialanie. Enter zamyka okno.'; read"

TERM_BIN=""
for t in gnome-terminal xterm alacritty x-terminal-emulator; do
    if command -v "$t" >/dev/null 2>&1; then TERM_BIN="$t"; break; fi
done

if [ -z "$TERM_BIN" ]; then
    echo "[c64-run] brak znanego emulatora terminala w PATH - uruchamiam kvm_host tutaj, w biezacym oknie."
    bash -c "$RUN_CMD"
    exit 0
fi

echo "[c64-run] otwieram kvm_host w nowym oknie ($TERM_BIN)..."
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
