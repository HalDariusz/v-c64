#!/usr/bin/env python3
"""tools/demo/asm.py - a minimal two-pass 6502 assembler, implementing
only the instructions needed for the sprite+SID test cartridge.

Part of v-c64 - a bare-metal Commodore 64 unikernel running directly on
Linux /dev/kvm, with no QEMU involved.

Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
"""

OPS_IMPLIED = {
    'SEI': 0x78, 'CLD': 0xD8, 'TXS': 0x9A, 'RTS': 0x60, 'RTI': 0x40,
    'INX': 0xE8, 'INY': 0xC8, 'DEX': 0xCA, 'DEY': 0x88, 'NOP': 0xEA,
}
OPS_IMM = {  # opcode nn
    'LDA': 0xA9, 'LDX': 0xA2, 'LDY': 0xA0, 'CMP': 0xC9, 'CPX': 0xE0,
    'CPY': 0xC0, 'AND': 0x29,
}
OPS_ZP = {  # opcode nn (adresowanie strony zerowej)
    'LDA': 0xA5, 'STA': 0x85, 'INC': 0xE6, 'DEC': 0xC6, 'CMP': 0xC5,
    'LDX': 0xA6, 'LDY': 0xA4, 'STX': 0x86, 'STY': 0x84,
}
OPS_ABS = {  # opcode lo hi
    'LDA': 0xAD, 'STA': 0x8D, 'STX': 0x8E, 'STY': 0x8C, 'INC': 0xEE,
    'DEC': 0xCE, 'CMP': 0xCD, 'JMP': 0x4C, 'JSR': 0x20,
}
OPS_ABSX = {'LDA': 0xBD, 'STA': 0x9D}
OPS_BRANCH = {
    'BNE': 0xD0, 'BEQ': 0xF0, 'BCC': 0x90, 'BCS': 0xB0, 'BPL': 0x10, 'BMI': 0x30,
}


class Asm:
    def __init__(self, origin):
        self.origin = origin
        self.lines = []  # (kind, ...)
        self.labels = {}

    def label(self, name):
        self.lines.append(('label', name))

    def op(self, mnemonic, mode=None, arg=None):
        self.lines.append(('op', mnemonic, mode, arg))

    def byte(self, *vals):
        self.lines.append(('byte', list(vals)))

    def word_lo_table(self, name, values):
        self.lines.append(('bytelist', values))

    def _size_of(self, item):
        if item[0] == 'label':
            return 0
        if item[0] == 'byte':
            return len(item[1])
        if item[0] == 'bytelist':
            return len(item[1])
        _, mnemonic, mode, arg = item
        if mode == 'impl':
            return 1
        if mode in ('imm', 'zp', 'branch'):
            return 2
        if mode in ('abs', 'absx'):
            return 3
        raise ValueError(item)

    def assemble(self):
        # przebieg 1: policz adresy etykiet
        addr = self.origin
        for item in self.lines:
            if item[0] == 'label':
                self.labels[item[1]] = addr
            else:
                addr += self._size_of(item)
        end_addr = addr

        # przebieg 2: emituj bajty
        out = bytearray()
        addr = self.origin
        for item in self.lines:
            if item[0] == 'label':
                continue
            if item[0] == 'byte':
                out.extend(item[1])
                addr += len(item[1])
                continue
            if item[0] == 'bytelist':
                out.extend(item[1])
                addr += len(item[1])
                continue

            _, mnemonic, mode, arg = item
            if mode == 'impl':
                out.append(OPS_IMPLIED[mnemonic])
                addr += 1
            elif mode == 'imm':
                out.append(OPS_IMM[mnemonic])
                out.append(arg & 0xFF)
                addr += 2
            elif mode == 'zp':
                out.append(OPS_ZP[mnemonic])
                out.append(arg & 0xFF)
                addr += 2
            elif mode == 'abs':
                target = self.labels[arg] if isinstance(arg, str) else arg
                out.append(OPS_ABS[mnemonic])
                out.append(target & 0xFF)
                out.append((target >> 8) & 0xFF)
                addr += 3
            elif mode == 'absx':
                target = self.labels[arg] if isinstance(arg, str) else arg
                out.append(OPS_ABSX[mnemonic])
                out.append(target & 0xFF)
                out.append((target >> 8) & 0xFF)
                addr += 3
            elif mode == 'branch':
                target = self.labels[arg]
                offset = target - (addr + 2)
                if not (-128 <= offset <= 127):
                    raise ValueError(f"branch out of range: {mnemonic} {arg} offset={offset}")
                out.append(OPS_BRANCH[mnemonic])
                out.append(offset & 0xFF)
                addr += 2
            else:
                raise ValueError(item)

        assert addr == end_addr
        return bytes(out), self.labels
