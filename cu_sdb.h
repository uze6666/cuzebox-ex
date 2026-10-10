/*
 *  Emulated SD card storage backend seam
 *
 *  Copyright (C) 2026
 *    Alec Bourque (Uze)
 *  CUzeBox is copyright (C)
 *    Sandor Zsuga (Jubatian)
 *  Uzem (the base of CUzeBox) is copyright (C)
 *    David Etherton,
 *    Eric Anderton,
 *    Alec Bourque (Uze),
 *    Filipe Rinaldi,
 *    Sandor Zsuga (Jubatian),
 *    Matt Pandina (Artcfox)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



/*
** The only interface cu_spisd.c has to the card's contents. It exists so the
** SD protocol layer does not know which backend is behind it, and so adding
** one later costs no further change there.
**
** Backends:
**
** - cu_vfat.c, the original, synthesizing a FAT16 volume over the game's own
**   directory. Always present, and the default.
** - cu_vfat2.c ("--sdhost"), the same over a subdirectory of it but with full
**   FAT semantics, so create, write, delete, truncate and rename all reach the
**   host. See doc/spec-sd-enhanced-vfat.md.
**
** A third, storing the card as a fixed disk image (doc/spec-sd-vhd-image.md),
** is documented but not scheduled. It would slot in here rather than reopening
** cu_spisd.c.
**
** Three of the calls below carry no card data at all. They exist so a backend
** can answer the guest, which an SD card can only do in three places:
**
** - cu_sdb_read_begin() returns the token which opens a read: 0xFE, or an
**   error token, after which no data phase follows.
** - cu_sdb_write_end() returns the data response token which closes a write:
**   accepted, rejected on CRC, or write error.
** - cu_sdb_write_abort() reports that chip select went away mid-block, which
**   on a real card leaves that block unprogrammed.
**
** Between them they are the whole vocabulary the emulator has for saying "no"
** to a ROM. The legacy backend uses none of it.
**
** With CU_VFAT2 undefined every call reduces to exactly what cu_spisd.c did
** before the seam existed, so such a build is unchanged rather than merely
** equivalent, and no object is added to it.
*/



#ifndef CU_SDB_H
#define CU_SDB_H



#include "types.h"



#ifdef CU_VFAT2



/*
** Consumes the backend's own command line options from argv, compacting the
** array, and returns the new argument count. Shaped like
** cu_gdb_parseargs(), and called beside it. Recognised:
**
**   --sdhost[=dir]    Serve the card from a subdirectory of the ROM's
**                     directory (default uzedisk, or as built)
**   --no-sdhost       Use the legacy backend, in a build defaulting to --sdhost
**   --sdhost-delete=trash|unlink|off
**                     What a file the guest deletes becomes (default trash:
**                     moved to .uzetrash/ inside the card directory)
**   --sdhost-crc=strict|ignore
**                     Refuse blocks with a bad CRC (default), or keep them as
**                     the legacy backend does
**   --sdhost-rescan   Rebuild the card, without a reset, when a file in its
**                     directory is added, removed or rewritten on the host
**                     while the bus is quiet (cu_vfat2_setrescan)
**   --sdhost-log[=f]  Log every host file change and everything refused or
**                     lost, to f or by default to stderr. In builds with
**                     FLAG_NOCONSOLE or HEADLESS the console messages are
**                     compiled out, and a log file is the only record.
*/
auint cu_sdb_parseargs(int argc, char** argv);


/*
** Brings the selected backend up. "game" is the path the emulator was given,
** which locates everything the card is built from. Must be called after
** filesys_setpath() and before the first reset.
*/
void  cu_sdb_init(char const* game);


/*
** Points the backend at another game, loaded while the emulator runs: what
** the card held is settled into the directory it came from first, and the
** reset which follows the load builds the card from beside the new game.
*/
void  cu_sdb_setgame(char const* game);
/*
** Rebuilds the card's structures from whatever underlies it. Called on
** emulator startup and on every in-emulator reset.
*/
void  cu_sdb_reset(void);


/*
** Returns the token which opens the read of the given sector: 0xFE if the
** data phase may proceed, or an error token (0b0000XXXX), after which it must
** not.
*/
auint cu_sdb_read_begin(auint lba);


/*
** Reads a byte from a byte position in the card's (virtual) disk image. A
** FAT16 volume is at most 2^31 bytes, so 32 bits addresses all of it.
*/
auint cu_sdb_read(auint pos);


/*
** Writes a byte to a byte position. Whether it reaches the host, and when, is
** the backend's business.
*/
void  cu_sdb_write(auint pos, auint data);


/*
** Ends the block in flight, "crcok" telling whether its CRC16 matched (or was
** not checked). Returns the data response token to send back: 0x05 accepted,
** 0x0B rejected on CRC, 0x0D write error.
*/
auint cu_sdb_write_end(boole crcok);


/*
** Abandons the block in flight, chip select having been released before it
** completed.
*/
void  cu_sdb_write_abort(void);


/*
** Called once per emulator frame, whether or not the emulation is running, so
** a backend has a timer it can settle against.
*/
void  cu_sdb_frametick(void);


/*
** Tears the backend down. Registered with atexit() by cu_sdb_init(), so
** main() needs no line for it.
*/
void  cu_sdb_quit(void);



#else /* CU_VFAT2 */



#include "cu_vfat.h"

#define cu_sdb_parseargs(argc, argv) ((auint)(argc))
#define cu_sdb_init(game)            do{ (void)(game); }while(0)
#define cu_sdb_setgame(game)         do{ (void)(game); }while(0)
#define cu_sdb_reset()               cu_vfat_reset()
#define cu_sdb_read_begin(lba)       ((void)(lba), 0xFEU)
#define cu_sdb_read(pos)             cu_vfat_read(pos)
#define cu_sdb_write(pos, data)      cu_vfat_write((pos), (data))
#define cu_sdb_write_end(crcok)      ((crcok) ? 0x05U : 0x0BU)
#define cu_sdb_write_abort()         do{}while(0)
#define cu_sdb_frametick()           do{}while(0)
#define cu_sdb_quit()                do{}while(0)



#endif /* CU_VFAT2 */



#endif
