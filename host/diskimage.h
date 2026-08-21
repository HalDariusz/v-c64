/*
 * host/diskimage.h - lightweight host-side read/write access to disk
 * (.d64) and tape (.t64) images.
 *
 * This is NOT an emulation of the 1541 drive or a datasette (no IEC, no
 * GCR, no signal timing on the CASSETTE port) - it's a shortcut similar
 * in spirit to the existing fast-loader: the host itself parses/modifies
 * the image structure (directory, BAM, sector chains) and hands
 * data to/from the guest as if it were a plain .prg file from disk/.
 *
 * Scope:
 *   - .d64 (standard 35-track, 40-track also tolerated on read): full
 *     read and WRITE support (SAVE appends/overwrites a file on the
 *     single mounted .d64 image in the disk directory).
 *   - .t64 ("tape image" - an emulator-specific format, no GCR): READ
 *     ONLY - in practice used to distribute ready-made dumps, not
 *     designed as a writable format (see diskimage.c).
 *   - directory listing (LOAD"$",8 + LIST) for both formats.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Searches every .d64/.t64 file (also .D64/.T64) in disk_dir for an entry
 * named "name" (a trailing "*" wildcard is supported - matches the rest,
 * also as plain "*" = "first file" - and "?" anywhere as a single
 * arbitrary character).
 *
 * On a hit, returns an allocated (malloc) buffer in .prg format (a
 * 2-byte little-endian load address followed by the file data) - exactly
 * the layout the rest of the fast-loader expects. The caller must free()
 * it. *out_len is the total buffer size (header + data). Returns NULL if
 * nothing matches in any image found in the directory. */
uint8_t *diskimage_find_prg(const char *disk_dir, const char *name, size_t *out_len);

/* Builds a directory listing (a ready-made BASIC "program" for LIST, in
 * exactly the same byte layout the real KERNAL produces for LOAD"$",8)
 * for the FIRST .d64 or .t64 image found in disk_dir. "pattern" is an
 * optional name filter (as in LOAD"$:S*",8) - can be NULL or an empty
 * string, in which case all files are shown.
 *
 * Returns an allocated (malloc) buffer in .prg format (the 2-byte $0801
 * address + data), which the caller must free. Returns NULL if disk_dir
 * contains no recognized image. */
uint8_t *diskimage_directory_listing(const char *disk_dir, const char *pattern, size_t *out_len);

/* Writes file "name" (SAVE) to the ONE .d64 image found in disk_dir -
 * only works when there is exactly one .d64 file in the directory
 * (otherwise returns 0, and the caller should fall back to a plain .prg
 * file write on the host, as before - this way existing workflows keep
 * their previous behavior unless the user deliberately mounts exactly
 * one writable image). If a file with this name already exists on the
 * image, it is overwritten (the old data sectors are freed in the BAM).
 *
 * "data" is a buffer in .prg format (2-byte LE load address + raw data),
 * data_len is its total size. Returns 1 on success (the image on the
 * host disk was actually overwritten), 0 if the write failed (no/multiple
 * .d64 images, the image isn't a standard 35-track D64, no free space
 * for the data or in the directory, or a bad name). */
int diskimage_save_prg(const char *disk_dir, const char *name, const uint8_t *data, size_t data_len);
