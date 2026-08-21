# v-c64 - manual

This is the detailed reference for **v-c64**: architecture, the
hypervisor and its KVM configuration, the physical and 6510/PLA memory
maps, audio/video output, keyboard mapping, the fast-loader, disk/tape
image handling, cartridge autostart, ROMs, the 6502 core, and the
documented simplifications. For a quick start, see the top-level
[README.md](../README.md).

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

## The hypervisor: KVM configuration (`host/kvm_host.c`)

`kvm_host.c` is a plain, single-threaded C program - no libvirt, no
QEMU device models, no BIOS/firmware blob. It talks to `/dev/kvm`
directly via `ioctl()` and implements just enough of the PC platform to
get one vCPU from the x86 reset vector into `guest/boot.s`, plus a
handful of custom I/O ports for keyboard/disk/audio.

### Requirements and setup

- **Access to `/dev/kvm`**: the running user needs read/write
  permission on the device node - either through membership in the
  `kvm` group (`sudo usermod -aG kvm $USER`, then log in again), or via
  `sudo`. `make run` itself also tries `sudo chmod a+rw /dev/kvm` as a
  convenience before launching `kvm_host`.
- **API version check**: right after `open("/dev/kvm", ...)`,
  `KVM_GET_API_VERSION` must return `12` - the stable KVM userspace API
  version present on any reasonably current Linux kernel; anything else
  aborts immediately rather than risk driving an incompatible ioctl ABI.
- **VM and vCPU creation**: `KVM_CREATE_VM` creates the virtual
  machine, `KVM_CREATE_VCPU` creates a single vCPU (id `0`) - v-c64
  never needs more than one, since the real C64 has exactly one CPU.
  `KVM_GET_VCPU_MMAP_SIZE` + `mmap()` maps the `struct kvm_run` page
  shared between the kernel and `kvm_host`, used to inspect/drive every
  `KVM_RUN` exit.
- **Command-line arguments**: `kvm_host [guest_binary] [disk_dir]` -
  defaults are `build/c64_guest.bin` and `disk`. `c64-run.sh` and
  `make run` both invoke it with explicit arguments.

### Physical memory map (KVM memory slots)

Three guest-physical memory regions are registered via
`KVM_SET_USER_MEMORY_REGION`, each backed by an anonymous, zeroed
`mmap()` region on the host (`map_region()`):

This is the *x86* address space as seen by the hypervisor and the vCPU -
not to be confused with the *6502* address space described in the next
section, which is a completely separate, software-managed mapping built
on top of the 512 KB RAM region below.

| Slot | Range               | Size    | Purpose                                   |
|------|---------------------|---------|--------------------------------------------|
| 0    | `0x00000-0x7FFFF`    | 512 KB  | guest RAM (`c64_guest.bin` @ `0x00000`)    |
| 1    | `0xA0000-0xBFFFF`    | 128 KB  | "VESA" 320x200x8bpp framebuffer            |
| 2    | `0xF0000-0xFFFFF`    | 64 KB   | "BIOS": reset vector at `0xFFFF0`          |

The guest binary (`c64_guest.bin`) is `fread()` straight into slot 0's
backing memory before the vCPU ever runs. Slot 2's only content is a
hand-written 5-byte `jmp far 0000:0000` (bytes `EA 00 00 00 00`) poked
at `0xFFFF0` - there is no real BIOS ROM image anywhere in this project.

There is no real VBE BIOS here - none is needed: the hypervisor itself
guarantees a linear 320x200x8bpp buffer at `0xA0000` simply by registering
that memory region, so `kernel.c` just writes to it directly
(`memcpy` through the physical address, flat segments, no paging).

### vCPU initial state

The vCPU is set up to start exactly where a real PC's CPU starts after
reset, in 16-bit real mode:

- **Segment registers** (`KVM_SET_SREGS`): CS = selector `0xF000`, base
  `0x000F0000` (so CS:IP `F000:FFF0` = physical `0xFFFF0`); DS/ES/FS/GS/SS
  all selector `0x0000`, base `0x00000000` - all six built as flat,
  64 KB-limited segments (`setup_flat_segment()`), code segment type
  `0x9B` (present, execute/read, accessed), data segments type `0x93`
  (present, read/write, accessed).
- **Control registers**: `CR0 = 0x10` (`ET=1`, required by KVM even in
  real mode; `PE=0` - protected mode is off, `boot.s` enables it itself
  by setting `CR0.PE` after boot), `CR3 = 0`, `CR4 = 0`, `EFER = 0` - a
  plain, unpaged, non-64-bit CPU, matching a freshly reset x86.
- **General registers** (`KVM_SET_REGS`): `RIP = 0xFFF0` (so execution
  starts at the reset vector above), `RFLAGS = 0x2` (only the reserved,
  always-set bit 1 - interrupts still disabled at this point).

From here the CPU executes the `jmp far 0000:0000` in slot 2, landing
in `guest/boot.s` at physical `0x00000` (slot 0), which switches to
protected mode itself (flat GDT, `CR0.PE=1`) before calling
`kernel_main()`. KVM/the host never touches the guest's registers again
after this initial setup - everything past this point is the guest's
own doing.

### The `KVM_RUN` loop and host<->guest I/O protocol

The host runs a simple synchronous loop: `ioctl(vcpu_fd, KVM_RUN, 0)`
blocks until the vCPU exits for some reason, `kvm_host` reacts, then
loops back into `KVM_RUN`. Because this is synchronous, the guest is
only ever "running" while the host is inside that one `ioctl()` call -
which is also what makes the real-time pacing below possible without
any timer interrupt machinery.

Exit reasons handled:

| `exit_reason`        | Handling |
|-----------------------|----------|
| `KVM_EXIT_HLT`         | Clean shutdown (the guest executed `HLT`, e.g. after a fatal condition or a deliberate halt) |
| `KVM_EXIT_IO`          | Dispatched by port number, see the table below |
| `KVM_EXIT_SHUTDOWN`    | Triple fault - logged and aborted (a genuine guest bug, not a designed code path) |
| anything else          | Logged and aborted - no other exit reason is expected in this project |

All actual communication between the guest and the host happens through
a small, custom set of x86 I/O ports (`IN`/`OUT` instructions in
`kernel.c`, decoded here via `run->io.port`/`run->io.direction`/
`run->io.data_offset`):

| Port     | Direction | Purpose |
|----------|-----------|---------|
| `0x60`   | IN        | Next queued keyboard byte (matrix scancode, make=`0x00-0x7F`/break=`0x80-0xFF`, or `0xFF` = RESTORE sentinel) - `0` if the queue is empty |
| `0x5001` | OUT       | Debug console: byte written straight to the host's `stderr` |
| `0x5006` | OUT       | One 16-bit signed SID audio sample - written to `audio.wav`, the live FIFO, and (SDL2 build) queued to the audio device; also drives the real-time pacing clock (see below) |
| `0x5007` | IN/OUT    | Fast-loader control/status: OUT selects a command (`0x01` LOAD filename-start, `0x02` LOAD-execute, `0x03` SAVE filename-start, `0x04` SAVE header/data-start, `0x05` SAVE-execute, `0x06` SIDLOAD filename-start, `0x07` SIDLOAD-execute); IN reads back the last operation's status byte (`1`=ok, `0`=failed/not found) |
| `0x5008` | IN/OUT    | Fast-loader data: OUT streams filename bytes or SAVE header/data bytes (meaning depends on the `0x5007` command in progress); IN streams back the loaded file's response bytes one at a time |

Keyboard input itself is gathered from two independent sources every
time port `0x60` is read: `poll_host_stdin()` (raw terminal input, see
"Keyboard" below) and, in SDL2 builds, `sdl_pump_events()` (the SDL
window's own keyboard/text events) - both push into the same
`g_kbd_queue` ring buffer, so typing in either place works
interchangeably. `LOAD`/`SAVE`/`SIDLOAD` handling itself
(`fastload_execute()`, `fastload_save_execute()`,
`fastload_sid_execute()`) lives entirely on the host side, backed by
`host/diskimage.c` for `.d64`/`.t64` images - see "Fast-loader" and
"Disk and tape images" below for the file-format details.

### Real-time pacing

The vCPU runs at full hardware virtualization speed, far faster than a
real C64's 985248 Hz 6510 clock, and this project deliberately doesn't
emulate a PIT/RTC to throttle it directly. Instead it relies on a fact
already true of `kernel.c`'s main loop: it emits exactly one SID audio
sample every ~22.35 6502 cycles (`985248 Hz / 44100 Hz`) via port
`0x5006`. `audio_pace()` therefore throttles *audio sample production*
to wall-clock time - every `AUDIO_PACING_BATCH` (256) samples, it
compares elapsed real time against `samples_so_far / 44100.0` and
`nanosleep()`s off any surplus. Since `KVM_RUN` is synchronous (the vCPU
literally does not execute while the host sleeps), throttling the
sample rate indirectly throttles the whole machine to real-C64 speed -
no separate CPU-cycle counting needed.

Video output is paced separately and only for observability, not
correctness: `build/frame.ppm` is rewritten from the VRAM slot roughly
every 200 ms, and (SDL2 builds) the window texture is redrawn roughly
every 40 ms (~25 FPS) - both independent of the audio-driven pacing
above.

### Terminal and signal handling

`setup_terminal()` always makes stdin non-blocking
(`fcntl(...,O_NONBLOCK)`) regardless of whether it's a real terminal -
a blocking `read()` on port `0x60`'s keyboard poll would otherwise
freeze the whole hypervisor (guest execution and audio pacing
included) until a key was pressed. When stdin *is* a terminal, it's
additionally switched to raw mode (`ICANON`/`ECHO` off, `VMIN=0`/
`VTIME=0`) so keypresses are delivered immediately without waiting for
Enter or being echoed back - `ISIG` is deliberately left on, so `Ctrl+C`
still raises `SIGINT` normally. `SIGINT`/`SIGTERM` are caught
(`on_signal()`) to set a shutdown flag checked at the top of the
`KVM_RUN` loop, so the original terminal settings and WAV file header
are always restored/finalized on exit, even on Ctrl+C.

### Build-time configuration (SDL2)

The `Makefile` probes for SDL2 via `pkg-config --cflags/--libs sdl2`;
if found, `HOST_CFLAGS` gains `-DUSE_SDL2` and `kvm_host.c` compiles in
the single-window SDL2 path (`sdl_init()`/`sdl_render()`/
`sdl_pump_events()`/`sdl_shutdown()`) described in "Audio/video output"
below. Without `libsdl2-dev` installed, none of that code is compiled
in at all - `kvm_host` builds and behaves exactly as the headless
fallback, with no runtime feature-detection or missing-library errors.

## The 6510 PLA memory map (`guest/memory_pla.c`)

The emulated CPU is a 6510, not a plain 6502: besides the ALU/registers
modeled by `fake6502.[ch]`, it has a built-in 6-bit I/O port at
addresses `$0000` (data direction register, DDR) and `$0001` (port
register, PR) that on a real C64 feeds directly into the PLA
(a single 82S100 PLA chip) to bank RAM, ROM, I/O and the cartridge in
and out of the CPU's 64 KB address space. `guest/memory_pla.c`
reimplements that same banking logic entirely in software, inside the
`fake6502_mem_read()`/`fake6502_mem_write()` hooks that `fake6502.c`
calls for every memory access.

### The CPU port ($00/$01)

| Address | Register | Meaning |
|---------|----------|---------|
| `$0000` | DDR (`cpu_port_ddr`) | Per-bit direction: `1` = output, `0` = input |
| `$0001` | PR (`cpu_port_pr`)   | Per-bit output level (only meaningful where DDR=1) |

Reading `$0001` never returns `cpu_port_pr` directly - it returns the
**effective** port value computed by `cpu_port_effective()`:

```c
(cpu_port_pr & cpu_port_ddr) | (~cpu_port_ddr)
```

i.e. bits configured as inputs (`DDR=0`) "float" high (pull-up), exactly
like the real 6510's undriven I/O pins. Three bits of the effective
value drive the banking decisions below:

| Bit | Mask | Name     | Meaning when `1` |
|-----|------|----------|-------------------|
| 0   | `CPU_PORT_LORAM`  | LORAM  | BASIC ROM enabled at `$A000-$BFFF` (together with HIRAM) |
| 1   | `CPU_PORT_HIRAM`  | HIRAM  | KERNAL ROM enabled at `$E000-$FFFF` |
| 2   | `CPU_PORT_CHAREN` | CHAREN | I/O area visible at `$D000-$DFFF` instead of CHARGEN ROM |

`pla_reset()` sets `cpu_port_ddr = 0x2F` and `cpu_port_pr = 0x37`, the
same values a real KERNAL reset leaves behind - LORAM=HIRAM=CHAREN=1,
so at power-on the CPU sees BASIC+KERNAL+CHARGEN/IO, exactly like a
stock C64.

Two other inputs come from the expansion port, read straight from
`cartridge.c`: `cartridge_game_line()` (/GAME) and
`cartridge_exrom_line()` (/EXROM). With no cartridge loaded both read
as inactive (`true`/high), which is the "everything defaults to RAM"
case below.

### Read-side bank selection (`fake6502_mem_read`)

| Address range     | Condition                          | Source                              |
|-------------------|-------------------------------------|--------------------------------------|
| `$0000-$0001`      | always                               | `cpu_port_ddr` / `cpu_port_effective()` |
| `$0002-$7FFF`      | always                               | `c64_ram` |
| `$8000-$9FFF`      | `!GAME \|\| !EXROM` (cartridge ROMLO active) | `cartridge_read_lorom()` |
|                    | otherwise                            | `c64_ram` |
| `$A000-$BFFF`      | `!GAME && EXROM` (cartridge 16K mode) | `cartridge_read_hirom()` |
|                    | else `LORAM && HIRAM`                | `rom_basic` |
|                    | otherwise                            | `c64_ram` |
| `$C000-$CFFF`      | always                               | `c64_ram` (never banked, same as real hardware) |
| `$D000-$DFFF`      | `CHAREN`                             | I/O space (`io_read()`, see below) |
|                    | otherwise                            | `rom_chargen` |
| `$E000-$FFFF`      | `HIRAM`                              | `rom_kernal`, with the autostart override below |
|                    | otherwise                            | `c64_ram` |

Cartridge banking mirrors `cartridge.c`'s two Simons' BASIC modes: 16K
mode (`GAME=0`/`EXROM=1`) maps both LOROM (`$8000-$9FFF`) and HIROM
(`$A000-$BFFF`); 8K mode (`GAME=1`/`EXROM=0`) maps only LOROM, leaving
`$A000-$BFFF` to BASIC/RAM as usual.

**Cartridge autostart override:** while `HIRAM` is set and a cartridge
with a valid `"CBM80"` signature is loaded, reads of `$FFFC`/`$FFFD`
(the 6502 RESET vector) are intercepted and redirected to
`cartridge_autostart_vector()`'s cold-start address instead of the
KERNAL's own reset vector - the same redirection a real KERNAL performs
after detecting a cartridge, done here directly in the memory hook
instead of by executing KERNAL code.

### The I/O space ($D000-$DFFF, `io_read`/`io_write`)

Visible only when `CHAREN=1`; dispatched purely by address sub-range:

| Address range      | Device                          | Notes |
|--------------------|----------------------------------|-------|
| `$D000-$D3FF`      | VIC-II (`vic2_reg_*`)            | register index = `address & 0x3F` (mirrored every 64 bytes) |
| `$D400-$D7FF`      | SID (`sid_reg_*`)                | register index = `address & 0x1F` (mirrored every 32 bytes) |
| `$D800-$DBFF`      | Color RAM (`color_ram`)          | only the low 4 bits are real; reads OR in `0xF0`, writes mask to `0x0F` |
| `$DC00-$DCFF`      | CIA 1 (`cia1_reg_*`)              | register index = `address & 0x0F` |
| `$DD00-$DDFF`      | CIA 2 (`cia2_reg_*`)              | register index = `address & 0x0F` |
| `$DE00-$DEFF`      | Cartridge I/O1 (`cartridge_io1_*`)| |
| `$DF00-$DF1F`      | REU (`reu_reg_*`)                 | register index = `address - 0xDF00` |
| `$DF20-$DFFF`      | unassigned                        | reads as `0xFF` (open bus), writes ignored |

### Write-side bank selection (`fake6502_mem_write`)

Writes are much simpler than reads: **RAM underneath a banked-in
ROM/cartridge window is always writable**, exactly like a real C64 -
the PLA's read/write asymmetry only shadows the *read* side with
ROM/cartridge data, it never blocks writes to the underlying RAM cell.
So the write path only special-cases two things:

1. `$0000`/`$0001` - writes update `cpu_port_ddr`/`cpu_port_pr`
   respectively, *and* are also mirrored into `c64_ram[address]` so
   that raw RAM readers (e.g. the REU's DMA engine, which bypasses PLA
   banking entirely) see the same byte a 6502 `LDA $00`/`LDA $01` would.
2. `$D000-$DFFF` while `CHAREN=1` - routed to `io_write()` (same
   sub-range dispatch table as `io_read`) instead of RAM.

Everything else - including `$8000-$BFFF` under active cartridge ROM,
and `$E000-$FFFF` under KERNAL - writes straight through to `c64_ram`.
There is no writable cartridge RAM in this emulator (Simons' BASIC is a
plain ROM cartridge), so a write to a cartridge-mapped address is only
ever visible again once the cartridge ROM is banked back out.

## Audio/video output

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

## Keyboard

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

## Fast-loader (.PRG) - LOAD and SAVE

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

## Disk (.d64) and tape (.t64) images

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

## Cartridge autostart

If `roms/simons.bin` contains a valid `"CBM80"` cold-start signature at
offset `$8004-$8008` (the standard C64 cartridge header format),
`memory_pla.c` redirects the 6502 RESET vector (`$FFFC/$FFFD`) to the
cartridge's cold-start vector (`$8000/$8001`) - exactly as the real
KERNAL would do when detecting a cartridge at startup. See "The 6510
PLA memory map" above for the full read-path banking logic this relies
on.

## ROMs

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
