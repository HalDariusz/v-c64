#!/usr/bin/env python3
"""tools/diskutil/mkd64.py - creates an empty, correctly formatted D64
image (standard 35-track, 174848 B), ready to drop into disk/ and be
written to via SAVE (see host/diskimage.c:diskimage_save_prg - it only
writes to an image with a valid BAM in the layout produced here).

Usage:
    python3 tools/diskutil/mkd64.py disk/blank.d64 ["DISK NAME"] [ID]

Part of v-c64 - a bare-metal Commodore 64 unikernel running directly on
Linux /dev/kvm, with no QEMU involved.

Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
"""
import sys

SPT = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5  # sektorow na sciezke, sciezki 1-35


def ts_offset(track, sector):
    return sum(SPT[:track - 1]) * 256 + sector * 256


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "disk/blank.d64"
    disk_name = (sys.argv[2] if len(sys.argv) > 2 else "PUSTY DYSK").upper()[:16]
    disk_id = (sys.argv[3] if len(sys.argv) > 3 else "64").upper()[:2]

    total = sum(SPT) * 256
    img = bytearray(total)

    bam_off = ts_offset(18, 0)
    img[bam_off + 0] = 18    # sciezka pierwszego sektora katalogu
    img[bam_off + 1] = 1     # sektor pierwszego sektora katalogu
    img[bam_off + 2] = 0x41  # wersja formatu DOS 'A'

    for t in range(1, 36):
        spt = SPT[t - 1]
        used = [False] * spt
        if t == 18:
            used[0] = True  # sam BAM (T18/S0)
            used[1] = True  # pierwszy (na razie jedyny) sektor katalogu
        free_count = spt - sum(used)
        entry_off = bam_off + 4 + (t - 1) * 4
        img[entry_off] = free_count
        bitmap = 0
        for s in range(spt):
            if not used[s]:
                bitmap |= (1 << s)
        img[entry_off + 1] = bitmap & 0xFF
        img[entry_off + 2] = (bitmap >> 8) & 0xFF
        img[entry_off + 3] = (bitmap >> 16) & 0xFF

    name_bytes = disk_name.encode("ascii").ljust(16, b"\xA0")
    img[bam_off + 0x90:bam_off + 0x90 + 16] = name_bytes
    img[bam_off + 0xA0] = 0xA0
    img[bam_off + 0xA1] = 0xA0
    id_bytes = disk_id.encode("ascii").ljust(2, b" ")
    img[bam_off + 0xA2:bam_off + 0xA2 + 2] = id_bytes
    img[bam_off + 0xA4] = 0xA0
    img[bam_off + 0xA5:bam_off + 0xA5 + 2] = b"2A"
    for k in range(0xA7, 0xAB):
        img[bam_off + k] = 0xA0

    dir_off = ts_offset(18, 1)
    img[dir_off + 0] = 0x00  # brak nastepnego sektora katalogu
    img[dir_off + 1] = 0xFF  # (nieuzywane, gdy next_track==0 - konwencja formatowania)
    # 8 wpisow katalogowych po 32B (offsety 2..255) pozostaje wyzerowanych = puste

    with open(out_path, "wb") as f:
        f.write(img)
    print(f"wrote {out_path}: {len(img)} bytes, name='{disk_name}' id='{disk_id}'")


if __name__ == "__main__":
    main()
