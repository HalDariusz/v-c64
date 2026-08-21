# v-c64 - a virtual Commodore 64 on bare `/dev/kvm` (no QEMU)

Bare-metal (unikernel) Commodore 64 emulator (6502/6510, VIC-II, SID, CIA,
128 KB REU, Simons' BASIC) running as an x86 guest directly on
`/dev/kvm`, driven by its own minimal hypervisor (`kvm_host.c`) - no QEMU
or any other intermediary emulator involved.

## Building and running

```sh
make run
```

This builds the guest (`build/c64_guest.bin`), the hypervisor (`kvm_host`)
and immediately runs the whole thing. It requires access to `/dev/kvm`
(membership in the `kvm` group, or sudo - `make run` will itself try
`sudo chmod a+rw /dev/kvm`).

For a comfortable, fully interactive session (typing commands and watching
the result live, in one place) install `libsdl2-dev` and use
`./c64-run.sh` instead of `make run` - see the "Audio/video output"
section below:

```sh
sudo apt install libsdl2-dev   # one-time
./c64-run.sh
```

## Architecture

```
c64-run.sh            builds and runs everything; SDL2 (if present) = one window, otherwise fallback
host/kvm_host.c      hypervisor: /dev/kvm, KVM_RUN, I/O port handling
guest/boot.s          boot code: Multiboot (decorative) -> protected mode -> kernel_main
guest/linker_raw.ld   guest memory layout starting at 0x00000000 (512 KB limit)
guest/kernel.c        main loop: fake6502_step + VIC-II/CIA/SID timing
guest/fake6502.[ch]   6502/6510 CPU core (vendored, GPLv2 - see below)
guest/memory_pla.[ch] PLA banking (LORAM/HIRAM/CHAREN, /GAME, /EXROM)
guest/vic2.[ch]       VIC-II: text/bitmap/multicolor/ECM modes, 8 sprites, raster IRQ
guest/cia.[ch]        CIA 1/2: timers -> IRQ/NMI, keyboard matrix, VIC-II bank
guest/sid.[ch]        SID: 3 voices (ADSR), simplified SVF filter
guest/reu.[ch]        128 KB REU: DMA registers $DF00-$DF1F
guest/cartridge.[ch]  Simons' BASIC: /GAME, /EXROM, autostart, bank switching
```

### Physical memory map (KVM memory slots)

| Range               | Size    | Purpose                                   |
|---------------------|---------|--------------------------------------------|
| `0x00000-0x7FFFF`    | 512 KB  | guest RAM (`c64_guest.bin` @ `0x00000`)    |
| `0xA0000-0xBFFFF`    | 128 KB  | "VESA" 320x200x8bpp framebuffer            |
| `0xF0000-0xFFFFF`    | 64 KB   | "BIOS": reset vector at `0xFFFF0`          |

The vCPU starts in real mode with CS:IP = `F000:FFF0` (physical
`0xFFFF0`), where a 5-byte `jmp far 0000:0000` sits, leading into
`guest/boot.s` loaded at physical `0x00000`. `boot.s` switches to
protected mode (flat GDT, `CR0.PE`), builds a minimal
`multiboot_info_min_t` structure describing the VESA buffer, and calls
`kernel_main()`.

There is no real VBE BIOS here - none is needed: the hypervisor itself
guarantees a linear 320x200x8bpp buffer at `0xA0000` simply by registering
that memory region, so `kernel.c` just writes to it directly
(`memcpy` through the physical address, flat segments, no paging).

### Audio/video output

**With `libsdl2-dev` installed (recommended)** - `kvm_host` detects SDL2
at build time (`pkg-config sdl2`) and then opens **one real, single
window** combining video, keyboard and sound:

```sh
sudo apt install libsdl2-dev
make clean && make all
```

- video: an SDL window (320x200, displayed at 2x scale), refreshed ~25
  times/s,
- keyboard: focusing this window sends keystrokes straight to the C64
  matrix - `SDL_TEXTINPUT` for regular characters (exactly the same
  `ascii_to_key()` table as in terminal mode below) and `SDL_KEYDOWN`
  for arrows/Home/End/Delete/F1-F12/Ctrl+letter (the same scancodes as
  the "Keyboard" section below - see `sdl_pump_events()` in
  `kvm_host.c`), with no auto-repeat on key-hold (same as terminal mode -
  a "tap" rather than holding the key down),
- audio: the same SID samples as before, additionally queued directly to
  the audio device via `SDL_QueueAudio` - heard live, without any extra
  FIFO/`aplay`.

Typing in the terminal that launched `kvm_host` **still works in
parallel** (see the "Keyboard" section below) - you can type in the SDL
window or in the terminal, whichever is convenient.

**Without `libsdl2-dev` (headless fallback)** - `kvm_host` builds and
works exactly as before, with no `sudo apt install` dependency:

- video: `build/frame.ppm`, refreshed roughly every 200 ms (view it with,
  e.g., `feh --reload 0.2 build/frame.ppm`, `display -update 0.5
  build/frame.ppm` from ImageMagick, or any auto-refreshing image
  viewer),
- audio: `build/audio.wav`, streamed continuously, finalized (correct
  RIFF header) on a clean shutdown (Ctrl+C or `HLT`) - only playable
  *after* the run ends (while the file is growing, the header
  temporarily reports zero data length),
- live audio during the run: `kvm_host` also (optionally) streams the
  same samples to `build/audio.fifo` (raw PCM, S16LE mono, 44100 Hz) -
  if something is already reading from that FIFO at startup (e.g.
  `aplay -t raw -f S16_LE -r 44100 -c 1 build/audio.fifo`), you'll hear
  audio live through the speakers; if not, `kvm_host` simply doesn't
  notice and keeps running normally (only `audio.wav` remains after it
  ends).

### Running with preview and sound (`c64-run.sh`)

```sh
./c64-run.sh [disk_directory]     # default: disk
```

The most convenient way to run everything - it builds everything
(`make all`) and runs `kvm_host` **directly, without a debugger**:

- **if `kvm_host` has SDL2** (checked via `ldd kvm_host | grep
  libSDL2`): the script just launches `kvm_host` - full interactivity
  (video + keyboard + sound) is provided by the single SDL window
  described above, nothing else needs wiring up,
- **if there's no SDL2**: the script recreates the same experience from
  separate pieces - `aplay` on `build/audio.fifo` (must start *before*
  `kvm_host`, since it opens the FIFO non-blocking - a ready reader is
  needed), `display -update 0.5 build/frame.ppm` from ImageMagick as the
  video preview, and `kvm_host` in a new terminal window
  (`gnome-terminal --wait`, `xterm`, `alacritty`, or
  `x-terminal-emulator` - whichever is found first on `PATH`) as the
  place to type on the keyboard. If no known terminal emulator is
  present on the system, `kvm_host` instead starts directly in the
  current window (the same effect as `make run`).

Closing the window (SDL or terminal) also stops any background
`aplay`/`display` (in the non-SDL2 variant) and removes
`build/audio.fifo`.

### Keyboard

Typing directly in the terminal running `./kvm_host` reaches the virtual
C64 keyboard (8x8 matrix, handled in `cia.c`). The terminal is switched
to unbuffered, non-echoing mode (but `Ctrl+C` still works - it ends the
program cleanly). In SDL2 mode (see above) exactly the same mapping
applies, only the key source is the SDL window instead of the terminal -
both sources are active at the same time. The mapping aims for the
**full** C64 keyboard, not just letters/digits:

- Characters that need Shift on a real C64 keyboard: `"()<>?:!#%&` plus
  the dedicated `@`, `*`, `'`, `$` keys - the last two are in practice
  essential for disk commands (`LOAD"$",8`, `LOAD"NAME*",8`, filenames
  with an apostrophe such as `SIMON'S BASIC`).
- Two separate keys, `+` and `=` - just like on a real C64 (this is not
  one key with Shift, but two physically distinct matrix positions).
- Graphic keys with no ASCII equivalent, mapped using the convention
  known from VICE: `\` -> `£`, `^` -> `↑`/pi, `_` -> `←` (left arrow,
  the key next to `1`).
- Cursor keys (arrows), `Home`/`Clr` and `Delete` from the terminal
  (xterm/VT100 escape sequences, `ESC [ ... ` or `ESC O ...`) - handled
  by a dedicated parser (`parse_escape_seq` in `kvm_host.c`) recognizing
  standard CSI/SS3 codes. Limited to xterm/vt100-compatible terminals
  (which the whole project uses anyway); if an escape sequence is split
  across two `read()` calls, the remainder is silently dropped - at
  worst one arrow keypress is lost, a deliberate simplicity trade-off.
- `F1`-`F8` from the terminal keyboard (SS3 sequences for F1-F4, CSI `~`
  for F5-F8) mapped to the corresponding C64 function-key pairs
  (`F1/F2`, `F3/F4`, `F5/F6`, `F7/F8` - just like on a real C64, where
  Shift toggles within the pair).
- `F12` -> **RESTORE**. This is the only C64 key that isn't part of the
  8x8 matrix - on real hardware it's wired directly to the 6502/6510 NMI
  line. Here it's carried over the same channel as the rest of the
  keyboard (port `0x60`), but as a special sentinel byte intercepted
  before the matrix read in `kernel.c`, which calls `fake6502_nmi()`
  directly.
- `Ctrl`+letter (e.g. for controlling text color, as on a real C64) works
  through standard ASCII control codes 1-26 sent by the terminal -
  this does **not** cover `Ctrl`+digit: those combinations have no
  consistent, portable ASCII codes across terminals, so they are
  deliberately unsupported (an honest limitation, not an oversight).

### Fast-loader (.PRG) - LOAD and SAVE

The simplest case, with no disk image at all: `SAVE"PROGRAM",8` writes
straight to `disk/PROGRAM` (a `.prg` file on the host disk), and
`LOAD"PROGRAM",8` + `RUN` reads it back - it works out of the box, the
`disk/` directory is created automatically. (If exactly one `.d64` image
is mounted in `disk/`, `SAVE` instead writes onto that image - see
"Disk images" below.)

`.prg` files dropped into the `disk/` directory (the second argument to
`kvm_host`, `disk` by default) are loaded instantly: `kernel.c`
intercepts PC == `$F4A5` (inside the KERNAL LOAD routine) and instead of
emulating it byte-by-byte on the 6502, asks the host for the file's data
through ports `0x5007` (control/status) and `0x5008` (filename/data),
then simulates an `RTS` back to the caller (also updating `$2D/$2E` -
VARTAB - just like the real KERNAL). In BASIC: `LOAD"NAME",8` (without
`,1` -> load address taken from the file header).

`SAVE` at PC == `$F5DD` is intercepted analogously: `kernel.c` reads the
start address (zero-page pointer in the accumulator) and end address
(X/Y - the standard KERNAL SAVE calling convention), then sends the data
to the host, which writes it to `disk/NAME` in the standard `.prg`
format (2-byte address header + raw data) - the file can then be read
straight back with `LOAD`.

### Disk (.d64) and tape (.t64) images

**Quick start - saving and loading a program on a diskette:**

```sh
# 1. create an empty, correctly formatted .d64 diskette in the disk/ directory
python3 tools/diskutil/mkd64.py disk/work.d64 "MY DISKETTE" 01

# 2. run the emulator (any way you like - make run or ./c64-run.sh)
./c64-run.sh
```

In BASIC, inside the running C64:

```basic
10 PRINT "HELLO FROM C64"
SAVE"PROGRAM",8       REM saves onto the single mounted .d64 (disk/work.d64)
LOAD"$",8             REM loads the disk directory as a BASIC "program"...
LIST                  REM ...and LIST displays it - PROGRAM and free blocks are visible
LOAD"PROGRAM",8       REM loads it back from the diskette
RUN
```

The `disk/work.d64` diskette is genuinely modified on the host disk -
`PROGRAM` stays on it permanently, visible again the next time the
emulator is started. To load an existing image (e.g. a `.d64`/`.t64`
downloaded from the internet with a collection of games/programs), just
drop it into `disk/` and use `LOAD"NAME",8,1` (or `LOAD"NAME*",8,1` with
a wildcard, if the name is long/unknown - see wildcards below) + `RUN`,
with no prior "inserting into the drive" needed - mounting is automatic,
based simply on the contents of the `disk/` directory.

If a file with the requested name doesn't exist directly in `disk/`, the
fast-loader (see above) automatically searches for it inside every
`.d64`/`.t64` file found in that directory (`host/diskimage.c`). The
host itself parses the image's directory structure (BAM + sector chain
for D64, entry table for T64), extracts the matching PRG file's data,
and hands it to the guest exactly like a regular `.prg` file -
`LOAD"NAME",8,1` + `RUN` therefore works directly on a mounted image,
with no manual extraction. Wildcards work as in the real KERNAL:
`LOAD"NAME*",8` (and plain `LOAD"*",8` = the first file on the image)
and `?` as any single character.

**Directory listing** - `LOAD"$",8` + `LIST` shows the real directory of
the first mounted image (disk name/ID, files with block count and type,
`BLOCKS FREE.` at the end) - in exactly the same byte layout as the
BASIC pseudo-program built by the real KERNAL. Pattern filtering is
supported: `LOAD"$:S*",8`.

**Writing (SAVE) to a .d64 image** - if exactly one `.d64` file is
mounted in the disk directory, `SAVE"NAME",8` really writes onto it:
`host/diskimage.c` allocates free data sectors from the real BAM,
appends a directory entry (allocating a new directory sector on track
18 if needed), and, when saving under an already-existing name, first
frees the old sector chain (overwrite, just like on a real C64). The
image on the host disk is genuinely modified in the process. If there's
no mounted image in the directory, or more than one (it's unclear which
to write to), `SAVE` falls back to the previous behavior: a plain
`.prg` file in the host directory - so nothing breaks in existing
workflows (e.g. `tools/sidplayer/`), as long as you don't deliberately
mount exactly one writable disk.

An empty, correctly formatted D64 image (with a valid BAM and an empty
directory) is created by `tools/diskutil/mkd64.py`:

```sh
python3 tools/diskutil/mkd64.py disk/work.d64 "MY DISKETTE" 01
```

`.t64`, on the other hand, is deliberately **read-only** - this is a
format invented by emulator authors for distributing ready-made tape
dumps, with a fixed (usually very tight) number of directory slots in
advance; appending a new file would require shifting the rest of the
file's data, so it's not a realistic model of a writable medium - unlike
D64, there's no point pretending it can mirror SAVE.

This is **not** an emulation of the 1541 drive or a datasette - no IEC
protocol, no GCR, no signal timing on the CASSETTE port. It's a
lightweight shortcut: ready-made PRG data straight to/from the guest's
RAM, just like the rest of the fast-loader, plus direct manipulation of
D64 structures on the host. Software relying on real drive behavior
(copy protection, its own bit-level loading routines, the DOS command
channel via `OPEN15,8,15,"..."`) may not run under this - a deliberate
trade-off of the "lightweight" version. Writing is also limited to the
standard 35-track D64 layout (reading also tolerates the rarer
40-track variant) - the only BAM layout this code can safely modify.

### Cartridge autostart

If `roms/simons.bin` contains a valid `"CBM80"` cold-start signature at
offset `$8004-$8008` (the standard C64 cartridge header format),
`memory_pla.c` redirects the 6502 RESET vector (`$FFFC/$FFFD`) to the
cartridge's cold-start vector (`$8000/$8001`) - exactly as the real
KERNAL would do when detecting a cartridge at startup.

### ROMs

The real KERNAL/BASIC/CHARGEN/Simons' BASIC dumps are not part of this
repository (copyright) - see `roms/README.md`. Without them, `make`
creates zero-filled placeholders of the correct size, so the whole
pipeline (CPU, PLA, VIC-II, CIA, SID, REU, hypervisor) can be built and
run right away - the 6502 just won't have anything meaningful to
execute (all zeros = all `BRK` instructions).

## 6502 core (fake6502)

`guest/fake6502.[ch]` is a vendored, unmodified copy of a well-tested,
cycle-aware 6502 emulator (originally Mike Chambers, 2011, fork:
<https://github.com/omarandlorraine/fake6502>), GPLv2 license - full
text in `third_party_licenses/fake6502-GPLv2.LICENSE`. Configured with
`-DNMOS6502 -DDECIMALMODE` (matching a real 6510: BCD decimal mode
supported, unlike the NES's 2A03).

## Honest simplifications (deliberate, documented in the source)

- VIC-II: renders a whole frame at once (not cycle-by-cycle/line-by-line
  during raster scanning) - correct final image, but without effects
  like "raster bars mid-line" that depend on a register change during
  the drawing of that same line.
- SID: real-time software synthesis (waveforms/ADSR/filter) - physically
  plausible, but not a cycle-accurate analog reimplementation like
  reSID. The SVF filter uses a small-angle approximation instead of
  `sin()` (no libm in a freestanding environment).
- REU: DMA transfers are performed synchronously (no real CPU
  "cycle stealing"), directly on the RAM array (bypassing PLA banking -
  matching how most emulators model the REU).
- Fast-loader supports the basic case `LOAD"name",8` (address from the
  file header) and `LOAD"name",8,1` (address from `$C3/$C4`).
