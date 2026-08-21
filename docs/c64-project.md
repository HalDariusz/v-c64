You are the Lead Systems/Virtualization Engineer. Your goal is to implement
and run a complete virtual Commodore 64 microcomputer project based on the
MOS 6502/6510 CPU, Simons' Basic (16 KB ROM), VESA graphics, SID sound, and
a 128 KB REU memory expansion, running in bare-metal (unikernel) mode
directly on a physical x86 CPU via the Linux `/dev/kvm` module (Intel VT-x
/ AMD-V), entirely without QEMU or any other emulator.

The project must be 100% self-contained, modular, and ready to build and
run with a single `make run` command.

### SYSTEM ARCHITECTURE AND SPECIFICATION:

1. HOST HYPERVISOR (kvm_host.c):
   - A plain C program (Linux) talking directly to /dev/kvm via ioctl.
   - Creates the VM (KVM_CREATE_VM) and the vCPU (KVM_CREATE_VCPU).
   - Registers 3 memory regions (KVM_SET_USER_MEMORY_REGION):
     * RAM: 0x00000 - 0x7FFFF (512 KB) - loads the guest file c64_guest.bin at 0x00000.
     * VGA VRAM: 0xA0000 - 0xBFFFF (128 KB) - shared video memory.
     * BIOS: 0xF0000 - 0xFFFFF (64 KB) - x86 reset vector at 0xFFFF0 with a 'JMP FAR 0x0000:0x0000' instruction (bytes: 0xEA, 0x00, 0x00, 0x00, 0x00).
   - The main KVM_RUN loop handles:
     * KVM_EXIT_IO (IN/OUT direction): reading keyboard port 0x60, terminal output 0x5001, fast-transfer status/data ports for .PRG/D64 files (0x5007/0x5008), and the SID audio sample buffer.
     * KVM_EXIT_HLT (clean shutdown).

2. BARE-METAL GUEST BOOT (boot.s & linker_raw.ld):
   - boot.s (NASM): a Multiboot 1 header immediately requesting VESA VBE graphics mode at 640x480 in an 8-bit palette (256 colors). Initializes a 16 KB stack and passes a multiboot_info_t* pointer to kernel_main in C.
   - linker_raw.ld: lays out sections (.text, .rodata, .data, .bss) starting exactly at address 0x00000000.
   - The resulting c64_guest.bin file must be converted via 'objcopy -O binary' into a clean raw binary format (no ELF headers).

3. 6510 CPU EMULATION CORE, MEMORY MAPPING AND PLA LOGIC (memory_pla.c / fake6502.c):
   - Integration with a cycle-accurate MOS 6502/6510 emulator (e.g. fake6502.c).
   - Support for the CPU register at address $0001 (port direction register) and the LORAM (bit 0), HIRAM (bit 1), and CHAREN (bit 2) bits.
   - Implement the functions read6502(uint16_t addr) and write6502(uint16_t addr, uint8_t val), switching visibility between 64 KB RAM, ROM chips (BASIC $A000-$BFFF, KERNAL $E000-$FFFF, CHARGEN $D000-$DFFF), and the I/O register area ($D000-$DFFF).
   - Simons' Basic (16 KB ROM): plugged in as a cartridge at $8000-$BFFF (EXROM/GAME lines), with automatic autostart.
   - 128 KB REU expansion: emulation of the DMA controller at address $DF00-$DF1F.

4. C64 SUPPORT CHIPS:
   - VIC-II ($D000-$D3FF): text and graphics mode rasterizer, 8 hardware sprites, and scaling of the 320x200 output image directly into the hardware VESA buffer (0xA0000).
   - CIA 1 & 2 ($DC00, $DD00): IRQ interrupt timers (50/60 Hz for the C64's system clock and cursor blink) and an 8x8 keyboard matrix mapped onto x86 scancodes (port 0x60).
   - SID ($D400-$D7FF): 3-voice synthesis, ADSR, and analog filters, delivered to the host's audio buffer.
   - Fast-Loader: intercepting the KERNAL LOAD routine ($F4A5) for instant loading of .PRG files from the host disk.

### PLAN OF ACTION AND TASKS TO PERFORM:

Step 1: Generate the complete project directory structure and all source
files:
- `kvm_host.c` (x86 KVM hypervisor)
- `boot.s` (Multiboot VESA assembly boot code)
- `linker_raw.ld` (linker script for a clean .bin)
- `fake6502.h` / `fake6502.c` (CPU core)
- `memory_pla.h` / `memory_pla.c` (PLA layout, banking and C64 ROMs)
- `vic2.h` / `vic2.c` (graphics, sprites and VESA conversion)
- `cia.h` / `cia.c` (timers, interrupts and keyboard matrix)
- `sid.h` / `sid.c` (audio synthesizer)
- `reu.h` / `reu.c` (128 KB RAM expansion)
- `cartridge.h` / `cartridge.c` (Simons' Basic support)
- `kernel.c` (the bare-metal guest's main execution loop)

Step 2: Generate a `Makefile` that:
- Fetches or generates ROM header tables via `xxd` (Kernal, Basic, CharGen, Simons' Basic).
- Compiles the NASM assembly code into `elf32` format.
- Compiles the guest C code with the flags `-m32 -ffreestanding -O2 -fno-pie -fno-stack-protector`.
- Links the guest into an ELF file and generates a raw `c64_guest.bin` binary via `objcopy -O binary`.
- Compiles the `kvm_host.c` hypervisor (`gcc kvm_host.c -o kvm_host -O2`).
- Provides a `make run` target that grants access to /dev/kvm and runs `./kvm_host`.

Now generate the full, error-free source code for all files, ready to
paste in and run.
