# ROMs

This directory must contain 4 binary files (not included in the
repository, since they are copyrighted material):

| File             | Size    | Contents                                     |
|------------------|---------|-----------------------------------------------|
| `kernal.bin`     | 8192 B  | Commodore 64 KERNAL ROM                       |
| `basic.bin`      | 8192 B  | Commodore 64 BASIC 2.0 ROM                    |
| `chargen.bin`    | 4096 B  | Commodore 64 Character Generator ROM          |
| `simons.bin`     | 16384 B | Simons' BASIC (16 KB cartridge)               |

`make` automatically creates zero-filled placeholders of the correct
size for any missing files, so the rest of the project (CPU, VIC-II,
CIA, SID, REU, KVM hypervisor) can be built and run right away. Without
the real KERNAL/BASIC dumps, the 6502 will execute nothing but zeros
(effectively all `BRK` instructions) - you'll see this in the
diagnostic log on the host terminal.

Replace these files with your own, legally obtained dumps (e.g. from a
C64 you physically own) to get a fully working system with a real
BASIC and KERNAL.
