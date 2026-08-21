# v-c64 - a virtual Commodore 64 on bare `/dev/kvm` (no QEMU)

A full Commodore 64 (6502/6510, VIC-II, SID, CIA, 128 KB REU, Simons'
BASIC) reborn as a tiny bare-metal unikernel: it boots as its own x86
guest straight on `/dev/kvm`, driven by a minimal custom hypervisor
(`kvm_host.c`) - no QEMU, no other emulator underneath, just your CPU's
virtualization extensions doing the real work.

Load a game from a `.d64` image, type BASIC, listen to SID music, all
with one command.

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
`./c64-run.sh` instead of `make run`:

```sh
sudo apt install libsdl2-dev   # one-time
./c64-run.sh
```

## Learn more

The full manual - architecture, the KVM hypervisor configuration, the
PLA/6510 memory map, audio/video output, keyboard mapping, the
fast-loader, disk/tape image handling, cartridge autostart, ROMs, and
the documented simplifications - lives in
[`docs/manual.md`](docs/manual.md).
