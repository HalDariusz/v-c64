# Test cartridge: animated sprite + SID tune

`build_demo.py` generates (using its own minimal assembler `asm.py`) a
hand-written 6502 program acting as an autostart cartridge (CBM80
signature at $8004-$8008) that:

- clears the screen/colors and disables text mode,
- draws sprite 0 (a filled 24x21 diamond) and animates it diagonally,
  bouncing off the edges,
- cyclically changes the sprite's color (register $D027),
- plays a simple C-major arpeggio (C4-E4-G4-C5-E5) on SID voice 1 with
  the ADSR envelope enabled (gate retriggered on each note).

Used to visually/audibly verify VIC-II and SID without relying on the
(unfinished in this build) Open ROMs BASIC expression evaluator - the
whole program runs completely bypassing the KERNAL/BASIC (the RESET
vector is redirected straight to the cartridge code via the autostart
mechanism in `memory_pla.c`).

## Usage

```sh
python3 tools/demo/build_demo.py        # generates roms/sprite_sid_demo.bin
cp roms/simons.bin roms/simons.bin.bak  # keep the original placeholder
cp roms/sprite_sid_demo.bin roms/simons.bin
make all
./kvm_host build/c64_guest.bin disk     # video: build/frame.ppm, audio: build/audio.wav
# after testing:
cp roms/simons.bin.bak roms/simons.bin
```
