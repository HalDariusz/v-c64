#!/usr/bin/env python3
"""tools/demo/build_demo.py - assembles the sprite+SID test cartridge
(guest/README in this directory) using the minimal assembler in asm.py,
and writes the resulting binary to roms/sprite_sid_demo.bin.

Part of v-c64 - a bare-metal Commodore 64 unikernel running directly on
Linux /dev/kvm, with no QEMU involved.

Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
"""
import sys, os, math
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, SCRIPT_DIR)
from asm import Asm

ORIGIN = 0x8009  # right after the 9-byte cartridge header ($8000-$8008)
a = Asm(ORIGIN)

DIR_X = 0x02
DIR_Y = 0x03
NOTE_INDEX = 0x04

a.label('START')
a.op('SEI', 'impl')
a.op('LDX', 'imm', 0xFF)
a.op('TXS', 'impl')
a.op('CLD', 'impl')

a.op('LDA', 'imm', 0)
a.op('STA', 'zp', DIR_X)
a.op('STA', 'zp', DIR_Y)
a.op('STA', 'zp', NOTE_INDEX)

# -- clear the screen (space) and color memory (blue, matching the background) - 4 pages at once
a.op('LDX', 'imm', 0)
a.label('CLR_SCREEN')
a.op('LDA', 'imm', 32)
a.op('STA', 'absx', 0x0400)
a.op('STA', 'absx', 0x0500)
a.op('STA', 'absx', 0x0600)
a.op('STA', 'absx', 0x0700)
a.op('LDA', 'imm', 6)
a.op('STA', 'absx', 0xD800)
a.op('STA', 'absx', 0xD900)
a.op('STA', 'absx', 0xDA00)
a.op('STA', 'absx', 0xDB00)
a.op('INX', 'impl')
a.op('BNE', 'branch', 'CLR_SCREEN')

# -- copy the sprite bitmap (63 B) from ROM to RAM $3000 (VIC bank 0)
a.op('LDX', 'imm', 0)
a.label('COPY_SPRITE')
a.op('LDA', 'absx', 'SPRITE_DATA')
a.op('STA', 'absx', 0x3000)
a.op('INX', 'impl')
a.op('CPX', 'imm', 63)
a.op('BNE', 'branch', 'COPY_SPRITE')

# -- sprite 0 pointer (screen $0400 + $3F8 = $07F8), data at $3000 -> $3000/64=$C0
a.op('LDA', 'imm', 0xC0)
a.op('STA', 'abs', 0x07F8)

# -- VIC-II: background/border colors, enable sprite 0
a.op('LDA', 'imm', 6)
a.op('STA', 'abs', 0xD021)
a.op('LDA', 'imm', 0)
a.op('STA', 'abs', 0xD020)
a.op('LDA', 'imm', 1)
a.op('STA', 'abs', 0xD015)
a.op('LDA', 'imm', 1)
a.op('STA', 'abs', 0xD027)
a.op('LDA', 'imm', 100)
a.op('STA', 'abs', 0xD000)
a.op('LDA', 'imm', 100)
a.op('STA', 'abs', 0xD001)
a.op('LDA', 'imm', 0)
a.op('STA', 'abs', 0xD010)
a.op('STA', 'abs', 0xD017)   # no Y-expand
a.op('STA', 'abs', 0xD01D)   # no X-expand
a.op('STA', 'abs', 0xD01C)   # no multicolor

# -- SID: reset filter/volume, voice 1 ADSR
a.op('LDA', 'imm', 15)
a.op('STA', 'abs', 0xD418)   # max volume, filter off
a.op('LDA', 'imm', 0x00)
a.op('STA', 'abs', 0xD417)   # no resonance/filter routing
a.op('LDA', 'imm', 0x59)     # attack=5 (~56ms), decay=9 (~300ms)
a.op('STA', 'abs', 0xD405)
a.op('LDA', 'imm', 0x00)     # sustain=0, release=0 (quick fade-out)
a.op('STA', 'abs', 0xD406)

a.label('MAINLOOP')

# --- X movement (step 3, bounce 24..240) ---
a.op('LDA', 'zp', DIR_X)
a.op('BEQ', 'branch', 'MOVE_RIGHT')
a.op('DEC', 'abs', 0xD000)
a.op('DEC', 'abs', 0xD000)
a.op('DEC', 'abs', 0xD000)
a.op('LDA', 'abs', 0xD000)
a.op('CMP', 'imm', 24)
a.op('BNE', 'branch', 'SKIP_X')
a.op('LDA', 'imm', 0)
a.op('STA', 'zp', DIR_X)
a.op('JMP', 'abs', 'SKIP_X')
a.label('MOVE_RIGHT')
a.op('INC', 'abs', 0xD000)
a.op('INC', 'abs', 0xD000)
a.op('INC', 'abs', 0xD000)
a.op('LDA', 'abs', 0xD000)
a.op('CMP', 'imm', 240)
a.op('BNE', 'branch', 'SKIP_X')
a.op('LDA', 'imm', 1)
a.op('STA', 'zp', DIR_X)
a.label('SKIP_X')

# --- Y movement (step 1, bounce 50..200) ---
a.op('LDA', 'zp', DIR_Y)
a.op('BEQ', 'branch', 'MOVE_DOWN')
a.op('DEC', 'abs', 0xD001)
a.op('LDA', 'abs', 0xD001)
a.op('CMP', 'imm', 50)
a.op('BNE', 'branch', 'SKIP_Y')
a.op('LDA', 'imm', 0)
a.op('STA', 'zp', DIR_Y)
a.op('JMP', 'abs', 'SKIP_Y')
a.label('MOVE_DOWN')
a.op('INC', 'abs', 0xD001)
a.op('LDA', 'abs', 0xD001)
a.op('CMP', 'imm', 200)
a.op('BNE', 'branch', 'SKIP_Y')
a.op('LDA', 'imm', 1)
a.op('STA', 'zp', DIR_Y)
a.label('SKIP_Y')

# --- rainbow sprite color ---
a.op('INC', 'abs', 0xD027)
a.op('LDA', 'abs', 0xD027)
a.op('CMP', 'imm', 16)
a.op('BNE', 'branch', 'SKIP_COL')
a.op('LDA', 'imm', 1)
a.op('STA', 'abs', 0xD027)
a.label('SKIP_COL')

# --- next arpeggio note (gate off->on = envelope retrigger) ---
a.op('LDA', 'imm', 0b00010000)   # triangle, gate=0
a.op('STA', 'abs', 0xD404)
a.op('LDX', 'zp', NOTE_INDEX)
a.op('LDA', 'absx', 'NOTE_FREQ_LO')
a.op('STA', 'abs', 0xD400)
a.op('LDA', 'absx', 'NOTE_FREQ_HI')
a.op('STA', 'abs', 0xD401)
a.op('LDA', 'imm', 0b00010001)   # triangle, gate=1
a.op('STA', 'abs', 0xD404)
a.op('INX', 'impl')
a.op('CPX', 'imm', 5)
a.op('BNE', 'branch', 'STORE_IDX')
a.op('LDX', 'imm', 0)
a.label('STORE_IDX')
a.op('STX', 'zp', NOTE_INDEX)

# --- delay loop (tempo) ---
a.op('LDX', 'imm', 40)
a.label('DELAY1')
a.op('LDY', 'imm', 0)
a.label('DELAY2')
a.op('DEY', 'impl')
a.op('BNE', 'branch', 'DELAY2')
a.op('DEX', 'impl')
a.op('BNE', 'branch', 'DELAY1')

a.op('JMP', 'abs', 'MAINLOOP')

a.label('NMI_HANDLER')
a.op('RTI', 'impl')

# --- data: SID frequencies for the C-major arpeggio (C4 E4 G4 C5 E5), PAL ---
SID_CLOCK = 985248.0
notes_hz = [261.63, 329.63, 392.00, 523.25, 659.25]
regs = [round(f * 16777216.0 / SID_CLOCK) for f in notes_hz]
a.label('NOTE_FREQ_LO')
a.byte(*[r & 0xFF for r in regs])
a.label('NOTE_FREQ_HI')
a.byte(*[(r >> 8) & 0xFF for r in regs])

# --- sprite bitmap: filled 24x21 diamond ---
a.label('SPRITE_DATA')
W, H = 24, 21
cx, cy = W / 2.0, H / 2.0
sprite_bytes = []
for y in range(H):
    bits = []
    for x in range(W):
        dx = abs(x - cx) / (W / 2.0)
        dy = abs(y - cy) / (H / 2.0)
        bits.append(1 if (dx + dy) <= 1.0 else 0)
    for b in range(0, 24, 8):
        byteval = 0
        for i in range(8):
            byteval = (byteval << 1) | bits[b + i]
        sprite_bytes.append(byteval)
a.byte(*sprite_bytes)

code, labels = a.assemble()
print(f"code: {len(code)} bytes, from ${ORIGIN:04X} to ${ORIGIN+len(code):04X}", file=sys.stderr)
print(f"NMI_HANDLER=${labels['NMI_HANDLER']:04X} SPRITE_DATA=${labels['SPRITE_DATA']:04X}", file=sys.stderr)

# --- assemble the full 16 KB cartridge image with a CBM80 header ---
rom = bytearray(16384)
start_addr = labels['START']
nmi_addr = labels['NMI_HANDLER']
rom[0] = start_addr & 0xFF
rom[1] = (start_addr >> 8) & 0xFF
rom[2] = nmi_addr & 0xFF
rom[3] = (nmi_addr >> 8) & 0xFF
rom[4:9] = b'CBM80'
off = ORIGIN - 0x8000
rom[off:off+len(code)] = code

with open(os.path.join(REPO_ROOT, 'roms', 'sprite_sid_demo.bin'), 'wb') as f:
    f.write(rom)
print("wrote roms/sprite_sid_demo.bin", file=sys.stderr)
