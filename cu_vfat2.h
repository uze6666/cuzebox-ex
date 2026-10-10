/*
 *  Host-directory FAT16 filesystem ("vfat2")
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
** A FAT16 volume over a subdirectory of the ROM's directory, in which what the
** guest writes is modelled rather than resolved on the spot, and reconciled
** against the host directory at defined points. See
** doc/spec-sd-enhanced-vfat.md.
**
** The legacy backend (cu_vfat.c) decides at the moment of each write which
** host file a sector belongs to, and drops it if it cannot tell -- which is
** every sector written before the FAT entry naming it arrives. This one
** accepts the block first and works out where it goes later, so the guest's
** write ordering stops mattering.
**
** Reached only through cu_sdb.c; cu_spisd.c never names it.
*/


#ifndef CU_VFAT2_H
#define CU_VFAT2_H


#include "types.h"
#include "cu_vfat2hf.h"


/*
** Volume layout, byte offsets into sys[]. Identical to the legacy backend's
** (cu_vfat.c:34-60), deliberately: the emulated BPB is the same constant, so
** a guest cannot tell the two apart by looking at the volume.
**
** Sector     Cluster   Size (Sec)
** 0x000000   ------    0x000001    MBR
** 0x000001   ------    0x000100    FAT0
** 0x000101   ------    0x000100    FAT1
** 0x000201   ------    0x000020    Root directory (512 entries)
** 0x000221   0x0002    0x3FFB80    Data area
** 0x3FFDA1   0xFFF0    --------    End of disk
**
** Shared with cu_vfat2dir.c, which reads the same image: two files disagreeing
** about where the FAT is would be a silent and total corruption.
*/
#define VFAT2_FAT0         0x000200U
#define VFAT2_FAT1         0x020200U
#define VFAT2_ROOT         0x040200U
#define VFAT2_DATA         0x044200U
#define VFAT2_SYS_SIZE     VFAT2_DATA

/* Root directory entries */
#define VFAT2_ROOT_SIZE    512U

/* Size of data area in clusters, and end cluster + 1 */
#define VFAT2_DATA_SIZE    0xFFEEU
#define VFAT2_DATA_END     (VFAT2_DATA_SIZE + 2U)

/* Bytes per cluster (64 sectors), and the shift for it */
#define VFAT2_CLU_SIZE     0x8000U
#define VFAT2_CLU_SHIFT    15U

/* Sectors on the volume, from the MBR's total size field */
#define VFAT2_SEC_TOTAL    0x3FFDA1U

/* No such slot: an unowned cluster, or an entry nothing matched */
#define VFAT2_NONE         0xFFFFU


/* A directory entry as the backend models it (section 4.2). Two tables of
** these exist: C, what vfat2 believes is on the host right now, and D, what
** the guest's sys[] asks for, parsed fresh at each reconcile. Matching one
** against the other is the whole of section 6. */
typedef struct{
 uint16 slot;                       /* Root directory slot, 0 to 511        */
 uint16 fclus;                      /* First cluster, 0 if unallocated      */
 auint  size;                       /* Directory entry size field           */
 auint  chain;                      /* Clusters actually walked             */
 uint8  attr;                       /* Directory entry attribute byte       */
 uint8  ctime[5];                   /* Creation time and date, bytes 13-17  */
 uint8  name[11];                   /* FAT 8.3 name, as stored              */
 uint8  flags;                      /* VFAT2_F_* below                      */
 char   host[CU_VFAT2_HNAME_SIZE];  /* Host file name, no path component    */
}cu_vfat2_ent_t;

#define VFAT2_F_QUARANTINE 0x01U    /* Chain invalid this round (section 6.3) */
#define VFAT2_F_NOHOST     0x02U    /* No host file backs this entry yet      */
#define VFAT2_F_USED       0x04U    /* Slot carries an entry at all           */


/*
** Brings the backend up. "game" is the path the emulator was given, which
** locates the ROM's directory, and "name" is the card directory's name below
** it. Neither is used until the first reset, so this has no ordering
** dependency beyond being called before it.
*/
void  cu_vfat2_init(char const* game, char const* name);


/*
** Rebuilds the volume from the card directory. Called on startup and on every
** in-emulator reset. Whether the card can be served at all is re-evaluated
** here, so creating a missing card directory and hitting reset recovers
** without restarting.
*/
void  cu_vfat2_reset(void);


/*
** Returns the token opening the read of a sector: 0xFE normally, or 0x01 if
** the card has failed (section 5.5), after which no data phase follows and
** the guest's f_mount() gets FR_DISK_ERR rather than an empty volume.
*/
auint cu_vfat2_read_begin(auint lba);


/*
** Reads a byte of the volume (section 5.1). Serves the committed system area
** directly, then blocks still held in the dirty store, then reads through to
** the host file owning the cluster.
*/
auint cu_vfat2_read(auint pos);


/*
** Takes a byte of the block in flight into staging (section 5.2). Nothing is
** decided here: no FAT walk, no host I/O, and no way to drop the byte.
*/
void  cu_vfat2_write(auint pos, auint data);


/*
** Completes the block in flight and returns the SD data response token for it
** (section 5.3). A block reaches the model exactly when a real card would
** program it: whole, and CRC clean.
*/
auint cu_vfat2_write_end(boole crcok);


/*
** Discards the block in flight, chip select having been released before it
** completed (section 5.6).
*/
void  cu_vfat2_write_abort(void);


/*
** Counts an emulator frame, and reconciles once the SD bus has been quiet for
** long enough (section 5.7). This is the only trigger not needing the guest
** to do something further, and it is why a file the ROM wrote appears on the
** host within a fifth of a second rather than at exit.
*/
void  cu_vfat2_frametick(void);


/*
** Chooses what happens to a block with a bad CRC: refused (the default), or,
** with "ignore", kept while the error token is still sent, which is what the
** legacy backend does. Only matters while the guest has CRC checking on.
*/
void  cu_vfat2_setcrc(boole ignore);


/*
** "--sdhost-rescan": while the guest leaves the card alone, notice a file
** added, removed or rewritten in the card directory on the host and rebuild
** the volume as a reset would, without resetting the ROM. Only safe for a ROM
** that keeps no directory or FAT state and no file open across the quiet spell
** -- a guest holding a cached FAT would write it back over the new layout.
*/
void  cu_vfat2_setrescan(boole on);


/*
** Tears the backend down.
*/
void  cu_vfat2_quit(void);


#endif
