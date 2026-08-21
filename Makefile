# Makefile - builds and runs the bare-metal virtual Commodore 64 on
# /dev/kvm, with no QEMU. `make run` does everything: build + start.
#
# Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
# on Linux /dev/kvm, with no QEMU involved.
#
# Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>

GUEST_CC      := gcc
GUEST_CFLAGS  := -m32 -ffreestanding -O2 -fno-pie -fno-stack-protector \
                 -Wall -Wextra -Iguest -Ibuild -DNMOS6502 -DDECIMALMODE
NASM          := nasm
NASMFLAGS     := -f elf32
LD            := ld
LDFLAGS       := -m elf_i386 -T guest/linker_raw.ld -nostdlib

HOST_CC       := gcc
HOST_CFLAGS   := -O2 -Wall -Wextra

# SDL2 (optional): if `pkg-config sdl2` is available, kvm_host gets a real
# interactive window (video + keyboard + audio in one window, see README
# "Running with preview and sound"). Without it, kvm_host builds and works
# exactly as before - fully headless (terminal + build/frame.ppm +
# build/audio.wav/fifo).
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
ifneq ($(SDL2_LIBS),)
    HOST_CFLAGS += -DUSE_SDL2 $(SDL2_CFLAGS)
endif

GUEST_OBJS := build/boot.o build/fake6502.o build/memory_pla.o build/vic2.o \
              build/cia.o build/sid.o build/reu.o build/cartridge.o build/kernel.o

ROM_HEADERS := build/rom_kernal.h build/rom_basic.h build/rom_chargen.h build/rom_simons.h

.PHONY: all run clean disk

all: kvm_host build/c64_guest.bin disk

build:
	mkdir -p build

disk:
	mkdir -p disk

# --- ROMs: converting roms/*.bin -> C arrays (xxd). If a ROM file wasn't
# provided by the user (see roms/README.md - the real KERNAL/BASIC/
# CHARGEN/Simons' BASIC dumps are copyrighted material and not part of
# this repository), a zero-filled placeholder of the correct size is
# created, so the rest of the pipeline can be built and run right away.

build/rom_kernal.h: roms/kernal.bin | build
	xxd -i -n rom_kernal_file roms/kernal.bin > $@

build/rom_basic.h: roms/basic.bin | build
	xxd -i -n rom_basic_file roms/basic.bin > $@

build/rom_chargen.h: roms/chargen.bin | build
	xxd -i -n rom_chargen_file roms/chargen.bin > $@

build/rom_simons.h: roms/simons.bin | build
	xxd -i -n rom_simons_file roms/simons.bin > $@

roms/kernal.bin:
	@echo "[Makefile] roms/kernal.bin missing - creating an 8 KB placeholder (all zeros)."
	@echo "[Makefile] Replace it with a real KERNAL ROM dump for the C64 to actually boot."
	dd if=/dev/zero of=$@ bs=1024 count=8 status=none

roms/basic.bin:
	@echo "[Makefile] roms/basic.bin missing - creating an 8 KB placeholder (all zeros)."
	dd if=/dev/zero of=$@ bs=1024 count=8 status=none

roms/chargen.bin:
	@echo "[Makefile] roms/chargen.bin missing - creating a 4 KB placeholder (all zeros)."
	dd if=/dev/zero of=$@ bs=1024 count=4 status=none

roms/simons.bin:
	@echo "[Makefile] roms/simons.bin missing - Simons' BASIC will not be loaded (cartridge inactive)."
	dd if=/dev/zero of=$@ bs=1024 count=16 status=none

# --- bare-metal guest --------------------------------------------------------

build/boot.o: guest/boot.s | build
	$(NASM) $(NASMFLAGS) guest/boot.s -o $@

build/kernel.o: guest/kernel.c $(ROM_HEADERS) | build
	$(GUEST_CC) $(GUEST_CFLAGS) -c guest/kernel.c -o $@

build/%.o: guest/%.c | build
	$(GUEST_CC) $(GUEST_CFLAGS) -c $< -o $@

build/c64_guest.elf: $(GUEST_OBJS) guest/linker_raw.ld
	$(LD) $(LDFLAGS) -o $@ $(GUEST_OBJS)

build/c64_guest.bin: build/c64_guest.elf
	objcopy -O binary $< $@
	@echo "[Makefile] c64_guest.bin: $$(stat -c%s $@) bytes (guest RAM limit: 524288)"

# --- host hypervisor ---------------------------------------------------------

kvm_host: host/kvm_host.c host/diskimage.c host/diskimage.h
	$(HOST_CC) $(HOST_CFLAGS) host/kvm_host.c host/diskimage.c -o $@ $(SDL2_LIBS)

# --- running -----------------------------------------------------------------

run: all
	@echo "[Makefile] attempting to grant permissions on /dev/kvm (may prompt for the sudo password)..."
	-sudo chmod a+rw /dev/kvm 2>/dev/null || true
	./kvm_host build/c64_guest.bin disk

clean:
	rm -rf build kvm_host
