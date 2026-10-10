# SD card served from a host folder (`--sdhost`)

With `--sdhost` the emulated SD card is a folder beside the game, and what the
ROM does to the card happens to the files in that folder: a file the ROM
creates appears on the PC, one it deletes goes to a trash folder, a rename is a
rename, and a file the ROM extends or truncates ends at exactly the size the
ROM gave it. It is meant for software that uses the card as a real file system
(FatFs, UzeFS, a BASIC that saves programs) and it reports everything it does,
refuses or loses.

Without `--sdhost` nothing changes: the card is the virtual FAT16 volume built
over the game's own directory, with its `SdAllowNewFiles` setting.

It is built with `FLAG_VFAT2=1` (the default). `FLAG_VFAT2_ON=1` makes
`--sdhost` the default for a build. It is not in the Emscripten or the
self-contained build.

```sh
./cuzebox game.hex --sdhost            # the card is the folder  uzedisk/  beside game.hex
./cuzebox game.hex --sdhost=mycard     # ...or  mycard/
```

## Command line

| Option | Effect |
| --- | --- |
| `--sdhost[=dir]` | Serve the card from a subfolder of the ROM's folder (default `uzedisk`) |
| `--no-sdhost` | Use the virtual volume, in a build where `--sdhost` is the default |
| `--sdhost-delete=trash\|unlink\|off` | What a file the ROM deletes becomes. Default `trash`: moved to `.uzetrash/` inside the card folder. `unlink` deletes it; `off` leaves the PC's file alone |
| `--sdhost-crc=strict\|ignore` | Refuse a block whose CRC is wrong (default), or store it anyway as the virtual volume does |
| `--sdhost-rescan` | Rebuild the card, without a reset, when a file in its folder is added, removed or rewritten on the PC while the card is not being accessed |
| `--sdhost-log[=file]` | Log every change made to the PC's files and everything refused or lost, to the file or to stderr |

## What the ROM sees

A FAT16 volume with the same geometry as the virtual volume: 32 KB clusters,
512 root directory entries, about 2 GB. The files of the card folder are in its
root directory under their 8.3 names; two long names that shorten to the same
8.3 name get `~1`, `~2`.

## When the PC's files change

The card is not written to the PC sector by sector. The emulator keeps the
volume in memory and brings the PC's folder in line with it once the ROM has
left the card alone for about a fifth of a second, and again at reset and at
exit. This is what lets it tell a rename from a delete followed by a create,
and survive a ROM that writes a file's data before its directory entry or its
allocation table, in either order.

At a reset or exit in the middle of a write, nothing is deleted or truncated on
the PC: files being created or extended are kept, and the log says what was
deferred.

## Limits

- No subdirectories. A ROM's attempt to create one is refused with a write
  error and reported.
- No long file names: the ROM sees 8.3 names only.
- At most 512 files, the size of a FAT16 root directory.
- The card is the one folder; nothing outside it can be reached.
- The emulator's save states and netplay rollback do not include this card. A
  state saved and loaded across card writes leaves the ROM's idea of the card
  and the card itself out of step, as it would on real hardware.
- The SD tools of the debugger (sector snapshots, file-system history) describe
  the virtual volume, not this card.

## What was tested

- `fattest`, a ROM running full FatFs R0.16 through create, multi-block write,
  append, truncate, rename, delete, deferred allocation-table writes, a
  corrupted allocation table, bad CRCs, aborted blocks and out-of-range
  blocks: 55 of 55 steps, each compared byte for byte on the PC.
- `uzefstest`, the same for UzeFS: 220 of 220 steps and every file checked on
  the PC.

## Two fixes that also affect the virtual volume

- The volume's boot sector now starts with a jump instruction. FatFs and UzeFS
  check for one and refused to mount the card without it; PetitFatFs does not
  check.
- A multi-block write (CMD25) now advances to the next block after each one.
  Every block of a burst was written over the first.
