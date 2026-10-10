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



#include "cu_vfat2.h"
#include "cu_vfat2dir.h"

#ifdef CU_VFAT2



/* Sectors held in the dirty store (section 4.4). 2 MBytes of payload. */
#ifndef CU_VFAT2_DIRTY_MAX
#define CU_VFAT2_DIRTY_MAX 4096U
#endif

/* Frames the SD bus must be quiet for before an idle reconcile (section 5.7).
** A frame counter rather than emulated cycles: it works while the emulation is
** paused or held by the debugger, at any emulation speed, and keeps the cycle
** count out of the backend seam. 12 frames is about 200ms at 60Hz. */
#ifndef CU_VFAT2_IDLE_FR
#define CU_VFAT2_IDLE_FR   12U
#endif

/* Reconciles a sector may survive without an owner before it is given up on
** (section 6.7). Eight is about 1.6 seconds of idle: by then the guest has
** either named the cluster in its FAT or is never going to. */
#ifndef CU_VFAT2_ORPHAN_MAX
#define CU_VFAT2_ORPHAN_MAX 8U
#endif

/* With "--sdhost-rescan", frames between two looks at the card directory while
** the bus is quiet: about half a second */
#ifndef CU_VFAT2_SCAN_FR
#define CU_VFAT2_SCAN_FR   30U
#endif

/* Free and tombstone markers in the dirty store's hash */
#define VFAT2_DIRTY_FREE   0xFFFFFFFFU
#define VFAT2_DIRTY_GONE   0xFFFFFFFEU



/* Master Boot Record (Note: Records are Little Endian!). vfat2's own copy, so
** cu_vfat.c stays inert (section 4.1). Byte for byte what the legacy backend
** emits, the volume label included -- a ROM must not be able to tell which
** backend it is talking to by reading the boot sector. Not included are the
** signature bytes (0x55, 0xAA) on the end of the 512 byte sector. */
#define VFAT2_MBR_SIZE     0x3EU
static const uint8 vfat2_data_mbr[VFAT2_MBR_SIZE] = {
 0xEBU, 0x3CU, 0x90U,        /* Jump to bootstrap (short jump + NOP). FatFs
                             ** check_fs() tests this byte for 0xEB, 0xE9 or
                             ** 0xE8 before looking at anything else */
 'C',   'U',   'Z',   'E',
 'B',   'O',   'X',   ' ',   /* OEM ID */
 0x00U, 0x02U,               /* 512 bytes / sector */
 0x40U,                      /* 64 sectors / cluster */
 0x01U, 0x00U,               /* Reserved sectors (1: Boot sector) */
 0x02U,                      /* Number of FATs */
 (VFAT2_ROOT_SIZE & 0xFFU), (VFAT2_ROOT_SIZE >> 8), /* No. of root directory entries */
 0x00U, 0x00U,               /* Number of sectors (indicates > 65535 sectors) */
 0xF8U,                      /* Media descriptor: Fixed disk */
 0x00U, 0x01U,               /* 256 sectors per FAT (One FAT uses 16 bit * 64K clusters) */
 0x00U, 0x00U,               /* CHS geometry sectors / track */
 0x00U, 0x00U,               /* CHS geometry number of heads */
 0x00U, 0x00U, 0x00U, 0x00U, /* Hidden sectors */
 0xA1U, 0xFDU, 0x3FU, 0x00U, /* 0x3FFDA1 sectors total size (~2 GBytes) */
 0x81U,                      /* Drive number (second HDD) */
 0x00U,                      /* Reserved */
 0x29U,                      /* Extended boot signature present */
 0xEFU, 0xBEU, 0xADU, 0xDEU, /* Serial number */
 'U',   'Z',   'E',   'B',
 'O',   'X',   'G',   'A',
 'M',   'E',   'S',          /* Volume label */
 'F',   'A',   'T',   '1',
 '6',   ' ',   ' ',   ' '};  /* File system type */


/* FAT seed: the media descriptor and the end of chain marker occupying the
** first two cluster slots (Note: Records are Little Endian!) */
#define VFAT2_FATSEED_SIZE 4U
static const uint8 vfat2_data_fat[VFAT2_FATSEED_SIZE] = {
 0xFFU, 0xF8U,
 0xFFU, 0xFFU,
};



/* The volume's system area: MBR, both FATs and the root directory, fully
** guest writable (section 4.1) */
static uint8 vfat2_sys[VFAT2_SYS_SIZE];

/* C, the committed state: what vfat2 believes is on the host right now. Built
** by the reset from the card directory listing, and updated by every
** successful reconcile. */
static cu_vfat2_ent_t vfat2_c[VFAT2_ROOT_SIZE];

/* Whether the cluster ownership map cu_vfat2dir.c holds still matches sys[].
** Set by every write that could have moved a cluster, cleared by the rebuild,
** so the read path pays for the walk once per FAT change rather than once per
** sector. */
static boole  vfat2_ownstale;


/* One sector held back from the host (section 4.4) */
typedef struct{
 auint lba;        /* Sector, or VFAT2_DIRTY_FREE / VFAT2_DIRTY_GONE   */
 auint age;        /* Reconciles survived unplaced (section 6.7)       */
 auint seq;        /* Insertion stamp, ordering eviction               */
 auint plim;       /* The size field it was last held back against     */
 boole past;       /* Held back: lies past its file's size (section 6.6) */
 uint8 data[512];
}cu_vfat2_dirty_t;

/* Every accepted data block not yet written to the host. Doubles as the
** orphan pool: a sector whose owning file could not be worked out at the last
** reconcile stays here and gets another chance next round, which is the actual
** fix for the write ordering problem. It also holds sectors a file owns but
** which lie past its size field, until the size reaches them (section 6.6). */
static cu_vfat2_dirty_t vfat2_dirty[CU_VFAT2_DIRTY_MAX];
static auint vfat2_dirty_cnt;
static auint vfat2_dirty_seq;
static auint vfat2_dirty_past;   /* Of those, held back past a file's size */


/* The block in flight (section 4.5). One at a time is all the SD protocol
** allows. All writes stage here, the system area included, which is what lets
** a CRC-corrupt FAT block be refused instead of landing in sys[]. */
static uint8 vfat2_stage[512];
static auint vfat2_stage_lba;
static auint vfat2_stage_len;
static boole vfat2_stage_act;


/* Where the card is built from, kept for the re-derivation at every reset */
static char  vfat2_game[1024];
static char  vfat2_dirname[256];

/* Set once cu_vfat2_init() has run */
static boole vfat2_ready = FALSE;

/* Card failed state (section 5.5): every read and write answers with an error
** token, so the guest gets FR_DISK_ERR rather than a card that looks fine and
** empty. Re-evaluated at every reset. */
static boole vfat2_failed = TRUE;

/* Something is waiting to reach the host */
static boole vfat2_pending;

/* Frames since the last SD access, for the idle reconcile and for the
** quiescence question section 6.8 asks at reset and at exit */
static auint vfat2_idlefr;

/* Frames to wait before reconciling again, so a round that could not place
** everything does not turn into one reconcile per frame */
static auint vfat2_recwait;

/* Blocks refused to the guest this session, or kept despite a bad CRC. Each is
** said on the console the first time only, so a ROM refused thousands of times
** cannot drown it, and the totals are given at exit so the rate limit hides
** nothing (section 8.4). The log gets every one. */
static auint vfat2_cnt_crc   = 0U;
static auint vfat2_cnt_dir   = 0U;
static auint vfat2_cnt_range = 0U;
static auint vfat2_cnt_short = 0U;

/* "--sdhost-crc=ignore": keep blocks with a bad CRC, as the legacy backend
** does */
static boole vfat2_crcignore = FALSE;

/* "--sdhost-rescan" (cu_vfat2_setrescan): the card directory's signature as of
** the last build or reconcile, and frames since it was last compared */
static boole vfat2_rescan = FALSE;
static auint vfat2_sig    = 0U;
static auint vfat2_scanfr = 0U;

/* Reconcile count. It names the temporary a rename cycle needs, so one left
** behind by a failed round cannot be mistaken for this round's, and the log
** heads each round with it. */
static auint vfat2_recno = 0U;


/* Reconcile scratch (section 6.4 and 6.5). Which D entry is which C entry,
** both ways round; which C entries keep their slot because the operation that
** would have removed them was deferred or failed; and how far into each file
** this round's writeback reached, which is what section 6.6 needs to tell an
** append in progress from a truncate. */
static uint16 vfat2_dmatch[VFAT2_ROOT_SIZE];
static uint16 vfat2_cmatch[VFAT2_ROOT_SIZE];
static boole  vfat2_retain[VFAT2_ROOT_SIZE];
static auint  vfat2_high  [VFAT2_ROOT_SIZE];

/* Each file's end by its size field, where the field fits the chain, or
** VFAT2_NOLIMIT: sectors past it are held back rather than written (6.6) */
#define VFAT2_NOLIMIT 0xFFFFFFFFU
static auint  vfat2_limit [VFAT2_ROOT_SIZE];

/* A new file on the first cluster of a file the guest deleted, which starts as
** a copy of that file's bytes (section 6.4): the deleted C entry to copy from,
** by D slot; whether a C entry is such a source, so its delete waits until the
** copy is made; and whether making it failed, so the source is kept. */
static uint16 vfat2_dcopy[VFAT2_ROOT_SIZE];
static boole  vfat2_csrc [VFAT2_ROOT_SIZE];
static boole  vfat2_cfail[VFAT2_ROOT_SIZE];

/* A rename put off because its target name still belongs to a copy source.
** C keeps the old FAT name for it, so the next round sees the rename again. */
static boole  vfat2_keepname[VFAT2_ROOT_SIZE];
static uint8  vfat2_oldname [VFAT2_ROOT_SIZE][11];


/* One sector's place in the writeback, so the store can be walked once and
** then visited in an order that opens each file once and writes it mostly
** forwards (section 6.5, step 4) */
typedef struct{
 auint  off;       /* Byte offset in the host file      */
 uint16 own;       /* Slot owning it                    */
 uint16 idx;       /* Where it sits in the dirty store  */
}cu_vfat2_wb_t;

static cu_vfat2_wb_t vfat2_wb[CU_VFAT2_DIRTY_MAX];



/*
** Hashes a sector to its home slot in the dirty store. The store is a power of
** two in size, so the modulo is a mask.
*/
static auint cu_vfat2_dhash(auint lba)
{
 return ((lba * 2654435761U) >> 8) & (CU_VFAT2_DIRTY_MAX - 1U);
}



/*
** Finds a sector in the dirty store, NULL if it is not held. O(1), which is
** what keeps the read path cheap.
*/
static cu_vfat2_dirty_t* cu_vfat2_dfind(auint lba)
{
 auint pos = cu_vfat2_dhash(lba);
 auint i;

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){

  if (vfat2_dirty[pos].lba == lba){ return &(vfat2_dirty[pos]); }
  if (vfat2_dirty[pos].lba == VFAT2_DIRTY_FREE){ return NULL; }
  /* A tombstone is passed over: an entry inserted after the one removed here
  ** may still lie further along the probe */

  pos = (pos + 1U) & (CU_VFAT2_DIRTY_MAX - 1U);

 }

 return NULL;
}



/*
** Empties the dirty store.
*/
static void cu_vfat2_dclear(void)
{
 auint i;

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){
  vfat2_dirty[i].lba = VFAT2_DIRTY_FREE;
  vfat2_dirty[i].age = 0U;
  vfat2_dirty[i].seq = 0U;
  vfat2_dirty[i].past = FALSE;
 }

 vfat2_dirty_cnt  = 0U;
 vfat2_dirty_seq  = 0U;
 vfat2_dirty_past = 0U;
}



/*
** The cluster a sector lies in, or 0 for the system area.
*/
static auint cu_vfat2_lbaclus(auint lba)
{
 auint pos = lba << 9;

 if (pos < VFAT2_DATA){ return 0U; }

 return ((pos - VFAT2_DATA) >> VFAT2_CLU_SHIFT) + 2U;
}



/* Sectors being given up on, gathered into one line per cluster. Freeing one
** cluster strands 64 sectors at once, and 64 lines say nothing the one line
** with the sector range does not. The log lists up to VFAT2_LOST_CLUS
** clusters, the console the first VFAT2_LOST_CON of them. */
#define VFAT2_LOST_CLUS 256U
#define VFAT2_LOST_CON  16U
typedef struct{
 auint n;                           /* Clusters listed                  */
 auint over;                        /* Sectors in clusters not listed   */
 uint16 clu[VFAT2_LOST_CLUS];       /* Cluster                          */
 uint16 cnt[VFAT2_LOST_CLUS];       /* Its sectors given up on          */
 auint lo [VFAT2_LOST_CLUS];        /* Its lowest sector given up on    */
 auint hi [VFAT2_LOST_CLUS];        /* Its highest                      */
 auint age[VFAT2_LOST_CLUS];        /* Oldest of them, in reconciles    */
}cu_vfat2_lost_t;

static cu_vfat2_lost_t vfat2_lost;



/*
** Counts one sector given up on towards its cluster's line.
*/
static void cu_vfat2_lost_add(cu_vfat2_lost_t* l, auint lba, auint age)
{
 auint clus = cu_vfat2_lbaclus(lba);
 auint j;

 for (j = 0U; j < l->n; j ++){
  if (l->clu[j] == clus){ break; }
 }

 if (j == l->n){
  if (l->n == VFAT2_LOST_CLUS){ l->over ++; return; }
  l->clu[j] = (uint16)(clus);
  l->cnt[j] = 0U;
  l->lo [j] = lba;
  l->hi [j] = lba;
  l->age[j] = age;
  l->n ++;
 }

 l->cnt[j] ++;
 if (lba < l->lo[j]){ l->lo[j] = lba; }
 if (lba > l->hi[j]){ l->hi[j] = lba; }
 if (age > l->age[j]){ l->age[j] = age; }
}



/*
** Reports the gathered sectors: data loss, so every time, with the sectors and
** the cluster (section 8.4).
*/
static void cu_vfat2_lost_say(cu_vfat2_lost_t const* l, char const* how)
{
 auint j;

 for (j = 0U; j < l->n; j ++){
  cu_vfat2hf_log("orphan  cluster 0x%04X  %u sector(s) in lba 0x%06X..0x%06X, unplaced, age %u, %s",
                 l->clu[j], l->cnt[j], l->lo[j], l->hi[j], l->age[j], how);
  if (j < VFAT2_LOST_CON){
   cu_vfat2hf_console("%u sector(s) of cluster 0x%04X, lba 0x%06X..0x%06X, could not be placed, %s",
                      l->cnt[j], l->clu[j], l->lo[j], l->hi[j], how);
  }
 }

 if (l->n > VFAT2_LOST_CON){
  cu_vfat2hf_console("and sectors of %u further cluster(s), likewise %s (see --sdhost-log)",
                     l->n - VFAT2_LOST_CON, how);
 }

 if (l->over != 0U){
  cu_vfat2hf_report(TRUE, "and %u sector(s) of yet more clusters, likewise %s", l->over, how);
 }
}



/*
** Drops the oldest entry of the dirty store, so a new one can go in. Only
** reached when reconcile could not free anything, which means the guest wrote
** more than CU_VFAT2_DIRTY_MAX sectors without ever naming them in the FAT.
** The block is lost, exactly as it would be on a card whose write cache
** overflowed -- but it is said out loud rather than being dropped silently,
** which is what the legacy backend does to every such sector.
*/
static void cu_vfat2_devict(void)
{
 auint i;
 auint use = CU_VFAT2_DIRTY_MAX;

 /* Sectors held back past a file's size go first: no directory entry says
 ** the guest has kept them. Then the oldest orphan. */

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){
  if ( (vfat2_dirty[i].lba == VFAT2_DIRTY_FREE) ||
       (vfat2_dirty[i].lba == VFAT2_DIRTY_GONE) ){ continue; }
  if (use == CU_VFAT2_DIRTY_MAX){ use = i; continue; }
  if (vfat2_dirty[i].past != vfat2_dirty[use].past){
   if (vfat2_dirty[i].past){ use = i; }
   continue;
  }
  if (vfat2_dirty[i].age > vfat2_dirty[use].age){ use = i; continue; }
  if ( (vfat2_dirty[i].age == vfat2_dirty[use].age) &&
       (vfat2_dirty[i].seq <  vfat2_dirty[use].seq) ){ use = i; }
 }

 if (use == CU_VFAT2_DIRTY_MAX){ return; }

 /* Data loss, so said every time (section 8.4). One sector at a time arrives
 ** here, so there is nothing to gather into a line per cluster. */

 cu_vfat2hf_report(TRUE, "Sector store full: lba 0x%06X cluster 0x%04X, held for %u reconcile(s), evicted and lost",
                   vfat2_dirty[use].lba, cu_vfat2_lbaclus(vfat2_dirty[use].lba),
                   vfat2_dirty[use].age);

 vfat2_dirty[use].lba = VFAT2_DIRTY_GONE;
 vfat2_dirty_cnt --;
}



/*
** Takes the staging buffer into the dirty store under the given sector,
** replacing whatever was held for it.
*/
static void cu_vfat2_dinsert(auint lba)
{
 auint pos;
 auint i;
 auint use = CU_VFAT2_DIRTY_MAX;

 /* An overwrite of a held sector must find the existing entry rather than
 ** adding a second one, so the probe runs to a free slot even when a
 ** tombstone offered room earlier */

 pos = cu_vfat2_dhash(lba);
 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){

  if (vfat2_dirty[pos].lba == lba){ use = pos; break; }

  if ( (use == CU_VFAT2_DIRTY_MAX) &&
       (vfat2_dirty[pos].lba == VFAT2_DIRTY_GONE) ){ use = pos; }

  if (vfat2_dirty[pos].lba == VFAT2_DIRTY_FREE){
   if (use == CU_VFAT2_DIRTY_MAX){ use = pos; }
   break;
  }

  pos = (pos + 1U) & (CU_VFAT2_DIRTY_MAX - 1U);

 }

 if (use == CU_VFAT2_DIRTY_MAX){ return; } /* Cannot happen: caller made room */

 if (vfat2_dirty[use].lba != lba){
  vfat2_dirty[use].age = 0U;
  vfat2_dirty_cnt ++;
 }

 vfat2_dirty[use].past = FALSE;       /* New content: judged afresh */

 vfat2_dirty[use].lba = lba;
 vfat2_dirty[use].seq = vfat2_dirty_seq;
 vfat2_dirty_seq ++;
 memcpy(&(vfat2_dirty[use].data[0]), &(vfat2_stage[0]), 512U);
}



/*
** Rebuilds the cluster ownership map from sys[] for the read path. The walk
** itself lives in cu_vfat2dir.c, so the read path and the reconcile see one
** ownership map rather than two implementations of it that can drift apart.
**
** Quietly: a guest reading over a FAT it is halfway through writing must not
** put anything on the console. The next reconcile says it once.
*/
static void cu_vfat2_ownmap(void)
{
 cu_vfat2dir_scan(&(vfat2_sys[0]), FALSE);

 /* The map says which slot of the guest's directory owns a cluster, and the
 ** host file behind it is whatever C has for that same file -- which is not
 ** necessarily C's entry at the same slot number. A slot the guest has reused
 ** since the last reconcile holds a different file in each table, and reading
 ** through to C's entry by slot would hand the guest another file's bytes. So
 ** the same identity match the reconcile uses resolves it here too. */

 cu_vfat2dir_match(&(vfat2_c[0]), &(vfat2_dmatch[0]), &(vfat2_cmatch[0]),
                   &(vfat2_dcopy[0]), TRUE);

 vfat2_ownstale = FALSE;
}



/*
** Removes a sector from the dirty store.
*/
static void cu_vfat2_ddrop(auint idx)
{
 if ( (vfat2_dirty[idx].lba == VFAT2_DIRTY_FREE) ||
      (vfat2_dirty[idx].lba == VFAT2_DIRTY_GONE) ){ return; }

 vfat2_dirty[idx].lba = VFAT2_DIRTY_GONE;
 vfat2_dirty_cnt --;
}



/*
** Orders the writeback: by owning file first, so each is opened once, then by
** offset within it, so it is written mostly forwards.
*/
static int cu_vfat2_wbcmp(void const* a, void const* b)
{
 cu_vfat2_wb_t const* wa = a;
 cu_vfat2_wb_t const* wb = b;

 if (wa->own != wb->own){ return (wa->own < wb->own) ? -1 : 1; }
 if (wa->off != wb->off){ return (wa->off < wb->off) ? -1 : 1; }

 return 0;
}



/*
** Turns a sector into the file that owns it and the offset it belongs at, or
** FALSE if nothing owns its cluster yet.
**
** This is the single place where the volume's geometry becomes a host file
** offset, and the one arithmetic detail that matters is that the in-cluster
** part is taken relative to the start of the data area. That area begins at
** 0x44200, which is 0x4200 into a cluster, so masking the absolute position
** would put every sector 16896 bytes into the wrong part of the file.
** Section 6.5 of the spec writes it the wrong way, as section 5.1 did.
*/
static boole cu_vfat2_place_sector(auint lba, auint* own, auint* off)
{
 auint pos = lba << 9;
 auint clus;
 auint o;

 if (pos < VFAT2_DATA){ return FALSE; }

 clus = ((pos - VFAT2_DATA) >> VFAT2_CLU_SHIFT) + 2U;
 if (clus >= VFAT2_DATA_END){ return FALSE; }

 o = cu_vfat2dir_owner(clus);
 if (o >= VFAT2_ROOT_SIZE){ return FALSE; }

 *own = o;
 *off = (cu_vfat2dir_cidx(clus) << VFAT2_CLU_SHIFT) +
        ((pos - VFAT2_DATA) & (VFAT2_CLU_SIZE - 1U));

 return TRUE;
}



/*
** Step 1 of section 6.5: files the guest took out of its directory.
**
** Deletes go first because they free host names a rename is about to want.
** Under a non-destructive reconcile they are skipped and the C entry is kept,
** so the next round retries: a file that should have gone staying a little
** longer is recoverable, and a file removed on the strength of a directory
** the guest was halfway through writing is not.
*/
static void cu_vfat2_delete_one(cu_vfat2_ent_t const* d, auint s, boole destructive,
                                char const* why, auint* defer)
{
 char  nstr[16];
 char  where[CU_VFAT2_HNAME_SIZE + 32U];

 cu_vfat2dir_show(&(vfat2_c[s].name[0]), &(nstr[0]));

 /* A C entry can only be carried forward if the guest has not put something
 ** else in its slot. When it has, the deferral cannot be remembered, and
 ** saying so is better than dropping it in silence. */

 if (!destructive){
  if ((d[s].flags & VFAT2_F_USED) == 0U){
   vfat2_retain[s] = TRUE;
   cu_vfat2hf_report(TRUE, "Deferred deleting \"%s\" (%s)",
                     &(vfat2_c[s].host[0]), why);
  }else{
   cu_vfat2hf_report(TRUE, "Deferred deleting \"%s\", but its slot was reused, so it will not be retried (%s)",
                     &(vfat2_c[s].host[0]), why);
  }
  (*defer) ++;
  return;
 }

 cu_vfat2hf_why("guest deleted \"%s\" from slot %u", &(nstr[0]), s);

 if (cu_vfat2hf_delete(&(vfat2_c[s].host[0]), &(where[0]), sizeof(where))){
  cu_vfat2hf_log("delete  slot %u clus 0x%04X  \"%s\" -> %s",
                 s, vfat2_c[s].fclus, &(vfat2_c[s].host[0]), &(where[0]));
 }else{
  /* Left in C unchanged, so the next reconcile tries again (section 7.6).
  ** Unless the guest has already put another file in the slot: then there is
  ** nowhere to remember it, and the file stays on the host for good. */
  if ((d[s].flags & VFAT2_F_USED) == 0U){
   vfat2_retain[s] = TRUE;
  }else{
   cu_vfat2hf_report(TRUE, "\"%s\" stays on the host: its slot was reused, so the delete will not be retried",
                     &(vfat2_c[s].host[0]));
  }
  (*defer) ++;
 }

 cu_vfat2hf_why("%s", "");
}



static void cu_vfat2_apply_deletes(cu_vfat2_ent_t const* d, boole destructive,
                                   char const* why, auint* defer)
{
 auint s;

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){

  if ((vfat2_c[s].flags & VFAT2_F_USED) == 0U){ continue; }
  if (vfat2_cmatch[s] != VFAT2_NONE){ continue; }
  if (vfat2_c[s].host[0] == 0){ continue; }

  /* A new file is about to be made as a copy of this one, so it goes after
  ** the creates instead (cu_vfat2_apply_srcdeletes()) */

  if (vfat2_csrc[s]){ continue; }

  cu_vfat2_delete_one(d, s, destructive, why, defer);

 }
}



/*
** The rest of step 1, for the deleted files a new file was copied from in step
** 3. Their bytes had to outlive the create, so their delete comes after it.
** If the copy could not be made the file is kept for this round rather than
** lost with it; the failure has been reported already.
*/
static void cu_vfat2_apply_srcdeletes(cu_vfat2_ent_t const* d, boole destructive,
                                      char const* why, auint* defer)
{
 auint s;

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){

  if (!vfat2_csrc[s]){ continue; }

  if (vfat2_cfail[s]){
   if ((d[s].flags & VFAT2_F_USED) == 0U){ vfat2_retain[s] = TRUE; }
   (*defer) ++;
   continue;
  }

  cu_vfat2_delete_one(d, s, destructive, why, defer);

 }
}



/*
** Step 2 of section 6.5: files whose name changed. Every matched entry leaves
** here carrying its host name in D, renamed or not, so the writeback that
** follows has somewhere to put its bytes.
**
** Only a destructive reconcile has renames to do. One that may not remove
** anything matches no renames (section 6.4): the entry under its new name is
** made as a copy in step 3 instead, and the old file stays as it is, with its
** delete deferred. Its data therefore never goes into the old host file.
*/
static void cu_vfat2_apply_renames(cu_vfat2_ent_t* d, auint* defer)
{
 auint s;
 auint t;
 auint swap = 0U;
 char  to  [CU_VFAT2_HNAME_SIZE];
 char  tmp [CU_VFAT2_HNAME_SIZE];
 char  nstr[16];

 /* Carry every matched entry's host name over first. The rename below has to
 ** be able to ask what any other entry is currently called, including ones it
 ** has not reached yet. */

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){

  if ((d[s].flags & VFAT2_F_USED) == 0U){ continue; }
  if (vfat2_dmatch[s] == VFAT2_NONE){ continue; }

  strncpy(&(d[s].host[0]), &(vfat2_c[vfat2_dmatch[s]].host[0]),
          CU_VFAT2_HNAME_SIZE - 1U);
  d[s].host[CU_VFAT2_HNAME_SIZE - 1U] = 0;

  if (d[s].host[0] == 0){ d[s].flags |= VFAT2_F_NOHOST; }

 }

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){

  if ((d[s].flags & VFAT2_F_USED) == 0U){ continue; }
  if (vfat2_dmatch[s] == VFAT2_NONE){ continue; }
  if (d[s].host[0] == 0){ continue; }

  /* Section 7.5, in two parts.
  **
  ** The first is the one that matters: a rename has happened only if the FAT
  ** name is not the one vfat2 itself last recorded for this file. C is the
  ** authority on that, and nothing else is. The spec states the guard as "does
  ** the host name still derive this FAT name", which is not sufficient -- the
  ** dedup of section 7.4 means the FAT name vfat2 handed out is often *not*
  ** the plain derivation. "MyLongReport.txt" is given MYLONG~1.TXT because
  ** MyLongRecord.txt took MYLONGRE.TXT first, so the derivation test fails
  ** against vfat2's own naming and renames the file to "mylong~1.txt" the
  ** first time the guest so much as rewrites the entry. Which is precisely
  ** what section 7.5 exists to prevent.
  **
  ** The second is the spec's test, kept as well: if the host name already
  ** derives the FAT name the guest now has, there is nothing to do even if the
  ** name did change. That is what keeps "Hello.bas" from flattening to
  ** "HELLO.BAS". */

  if (memcmp(&(d[s].name[0]), &(vfat2_c[vfat2_dmatch[s]].name[0]), 11U) == 0){ continue; }
  if (cu_vfat2hf_namematch(&(d[s].host[0]), &(d[s].name[0]))){ continue; }

  cu_vfat2hf_tohost(&(d[s].name[0]), &(to[0]), CU_VFAT2_HNAME_SIZE);
  if (to[0] == 0){ continue; }
  if (strcmp(&(to[0]), &(d[s].host[0])) == 0){ continue; }

  /* A deleted file a new one is still to be copied from may hold the name.
  ** Renaming onto it now would replace it before the copy is made, so the
  ** rename waits a round, by which time that file has gone through the delete
  ** policy. C keeps the old FAT name, so the next round sees the rename. */

  for (t = 0U; t < VFAT2_ROOT_SIZE; t ++){
   if (!vfat2_csrc[t]){ continue; }
   if (cu_vfat2hf_samename(&(vfat2_c[t].host[0]), &(to[0]))){ break; }
  }
  if (t < VFAT2_ROOT_SIZE){
   cu_vfat2hf_report(TRUE, "Deferred renaming \"%s\" to \"%s\": the guest deleted the file of that name this round, which goes first",
                     &(d[s].host[0]), &(to[0]));
   vfat2_keepname[s] = TRUE;
   memcpy(&(vfat2_oldname[s][0]), &(vfat2_c[vfat2_dmatch[s]].name[0]), 11U);
   (*defer) ++;
   continue;
  }

  cu_vfat2dir_show(&(d[s].name[0]), &(nstr[0]));
  cu_vfat2hf_why("guest renamed slot %u clus 0x%04X to \"%s\"", s, d[s].fclus, &(nstr[0]));

  /* If another entry still holds the name this one wants, move that one out
  ** of the way first. A straight swap -- A to B while B to A -- would
  ** otherwise overwrite one file with the other and lose it. */

  for (t = 0U; t < VFAT2_ROOT_SIZE; t ++){
   if (t == s){ continue; }
   if ((d[t].flags & VFAT2_F_USED) == 0U){ continue; }
   if (d[t].host[0] == 0){ continue; }
   if (!cu_vfat2hf_samename(&(d[t].host[0]), &(to[0]))){ continue; }
   snprintf(&(tmp[0]), sizeof(tmp), "_uzeswap%u_%u.tmp", vfat2_recno, swap);
   swap ++;
   if (cu_vfat2hf_rename(&(d[t].host[0]), &(tmp[0]))){
    print_message("cu_sdhost: Moved \"%s\" aside as \"%s\" to break a rename cycle\n",
                  &(d[t].host[0]), &(tmp[0]));
    cu_vfat2hf_log("rename  slot %u clus 0x%04X  \"%s\" -> \"%s\"  (out of the way of slot %u)",
                   t, d[t].fclus, &(d[t].host[0]), &(tmp[0]), s);
    strcpy(&(d[t].host[0]), &(tmp[0]));
   }
   break;
  }

  if (cu_vfat2hf_rename(&(d[s].host[0]), &(to[0]))){
   print_message("cu_sdhost: Renamed \"%s\" to \"%s\"\n", &(d[s].host[0]), &(to[0]));
   cu_vfat2hf_log("rename  slot %u clus 0x%04X  \"%s\" -> \"%s\"",
                  s, d[s].fclus, &(d[s].host[0]), &(to[0]));
   strcpy(&(d[s].host[0]), &(to[0]));
  }else{
   /* The FAT name is committed to C regardless, so this is not asked again.
   ** The file is still read and written under its old host name. */
   cu_vfat2hf_report(TRUE, "\"%s\" keeps that host name for the rest of the session, the rename is not retried",
                     &(d[s].host[0]));
   (*defer) ++;
  }

 }

 cu_vfat2hf_why("%s", "");
}



static boole cu_vfat2_size_fits(cu_vfat2_ent_t const* e);



/*
** How many bytes of a deleted file's host file a new entry on its first
** cluster starts with: the length the resize rule of section 6.6 is going to
** give the new entry, worked out ahead of the writeback that decides part of
** it. Copying more would leave the old file's tail on the new one whenever the
** trim is deferred, and the trim is exactly what a non-destructive reconcile
** defers.
*/
static auint cu_vfat2_copylen(cu_vfat2_ent_t const* d, auint s)
{
 auint i;
 auint own;
 auint off;
 auint high = 0U;
 auint claim;

 if (cu_vfat2_size_fits(&(d[s]))){ return d[s].size; }

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){
  if ( (vfat2_dirty[i].lba == VFAT2_DIRTY_FREE) ||
       (vfat2_dirty[i].lba == VFAT2_DIRTY_GONE) ){ continue; }
  if (!cu_vfat2_place_sector(vfat2_dirty[i].lba, &own, &off)){ continue; }
  if (own != s){ continue; }
  if ((off + 512U) > high){ high = off + 512U; }
 }

 claim = d[s].size;
 if (claim > (d[s].chain << VFAT2_CLU_SHIFT)){ claim = 0U; }

 return (claim > high) ? claim : high;
}



/*
** Step 3 of section 6.5: entries with no host file behind them yet.
**
** Usually that means the guest's directory has an entry C does not, which is a
** new file. It also catches an entry that was quarantined when it was new, so
** got no host file then, and whose chain has since become valid: it is matched
** by now rather than new, but it still needs creating. Keying the step on "has
** no host name" rather than on "is unmatched" is what lets a file recover from
** a broken FAT without a reset, which section 6.3 promises and nothing else
** here would deliver.
**
** The host name is derived from the FAT name and nothing else, which is what
** makes it collision free for any directory a guest could legitimately write:
** 8.3 names are unique within a FAT directory and the derivation is a function
** of the name alone. A duplicate can only come from a corrupt directory, and is
** refused rather than given a second entry pointing at one file.
*/
static void cu_vfat2_apply_creates(cu_vfat2_ent_t* d)
{
 auint s;
 auint t;
 auint src;
 auint len;
 char  nstr[16];

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){

  if ((d[s].flags & VFAT2_F_USED) == 0U){ continue; }
  if (d[s].host[0] != 0){ continue; }   /* Carried over by the match */

  /* Until the copy is made, its source counts as failed, so that every way
  ** out of this iteration short of success keeps the source on the host */

  src = vfat2_dcopy[s];
  if (src != VFAT2_NONE){ vfat2_cfail[src] = TRUE; }

  /* A quarantined entry gets no host file at all. Section 6.3 takes its
  ** writeback away for the round, and creating the file regardless would put
  ** an empty one on the card for a subdirectory entry or a looping chain --
  ** which is worse than doing nothing, because it looks like it worked. */

  if ((d[s].flags & VFAT2_F_QUARANTINE) != 0U){
   d[s].flags |= VFAT2_F_NOHOST;
   continue;
  }

  cu_vfat2hf_tohost(&(d[s].name[0]), &(d[s].host[0]), CU_VFAT2_HNAME_SIZE);

  if (d[s].host[0] == 0){
   d[s].flags |= VFAT2_F_NOHOST | VFAT2_F_QUARANTINE;
   continue;
  }

  for (t = 0U; t < VFAT2_ROOT_SIZE; t ++){
   if (t == s){ continue; }
   if ((d[t].flags & VFAT2_F_USED) == 0U){ continue; }
   if (d[t].host[0] == 0){ continue; }
   if (strcmp(&(d[t].host[0]), &(d[s].host[0])) != 0){ continue; }
   cu_vfat2hf_report(TRUE, "Slots %u and %u both name \"%s\", slot %u not written back",
                     t, s, &(d[s].host[0]), s);
   d[s].flags |= VFAT2_F_NOHOST | VFAT2_F_QUARANTINE;
   d[s].host[0] = 0;
   break;
  }

  if (d[s].host[0] == 0){ continue; }   /* Refused just above */

  cu_vfat2dir_show(&(d[s].name[0]), &(nstr[0]));
  cu_vfat2hf_why("guest created \"%s\" in slot %u", &(nstr[0]), s);

  if (!cu_vfat2hf_create(&(d[s].host[0]))){
   d[s].flags |= VFAT2_F_NOHOST;
   d[s].host[0] = 0;
   continue;
  }

  d[s].flags &= (uint8)(~VFAT2_F_NOHOST);

  if (src == VFAT2_NONE){
   print_message("cu_sdhost: Created \"%s\"\n", &(d[s].host[0]));
   cu_vfat2hf_log("create  slot %u clus 0x%04X  \"%s\"", s, d[s].fclus, &(d[s].host[0]));
   continue;
  }

  /* On the first cluster of a file the guest deleted: start from that file's
  ** bytes, as far as this entry can use them. Its own sectors go on top in
  ** step 4. */

  len = cu_vfat2_copylen(d, s);

  if (cu_vfat2hf_copy(&(vfat2_c[src].host[0]), &(d[s].host[0]), len)){
   vfat2_cfail[src] = FALSE;
   print_message("cu_sdhost: Created \"%s\" from \"%s\", whose first cluster it has\n",
                 &(d[s].host[0]), &(vfat2_c[src].host[0]));
   cu_vfat2hf_log("create  slot %u clus 0x%04X  \"%s\" as a copy of \"%s\" up to %u bytes  (first cluster reused, not a rename)",
                  s, d[s].fclus, &(d[s].host[0]), &(vfat2_c[src].host[0]), len);
  }else{
   cu_vfat2hf_report(TRUE, "\"%s\" was created without the bytes of \"%s\", which is kept on the host",
                     &(d[s].host[0]), &(vfat2_c[src].host[0]));
  }

 }

 cu_vfat2hf_why("%s", "");
}



/*
** Step 4 of section 6.5: the data itself.
**
** A sector whose cluster nothing owns is not dropped here, which is the whole
** of the write ordering problem. The guest is entitled to write the data
** before the FAT entry naming the cluster -- full FatFs does exactly that --
** and the sector waits in the store for the round where its owner appears.
** The legacy backend discards it the instant it arrives while the SPI layer
** answers "accepted", so nothing anywhere learns the write vanished.
*/
static void cu_vfat2_apply_writeback(cu_vfat2_ent_t const* d)
{
 auint i;
 auint n = 0U;
 auint own = 0U;
 auint off = 0U;
 auint cnt = 0U;
 auint lo  = 0U;
 auint hi  = 0U;
 auint lim;
 auint len;
 boole hold;
 boole fail = FALSE;
 char  nstr[16];
 cu_vfat2_dirty_t* ds;

 /* Where each file's size field puts its end, when it can be trusted to: the
 ** field fits the chain (section 6.6). Sectors past it are held back rather
 ** than written. */

 for (i = 0U; i < VFAT2_ROOT_SIZE; i ++){
  vfat2_limit[i] = VFAT2_NOLIMIT;
  if ((d[i].flags & VFAT2_F_USED) == 0U){ continue; }
  if (d[i].size > (d[i].chain << VFAT2_CLU_SHIFT)){ continue; }
  if (!cu_vfat2_size_fits(&(d[i]))){ continue; }
  vfat2_limit[i] = d[i].size;
 }

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){

  if ( (vfat2_dirty[i].lba == VFAT2_DIRTY_FREE) ||
       (vfat2_dirty[i].lba == VFAT2_DIRTY_GONE) ){ continue; }

  if (!cu_vfat2_place_sector(vfat2_dirty[i].lba, &own, &off)){ continue; }

  if ((d[own].flags & VFAT2_F_USED) == 0U){ continue; }
  if ((d[own].flags & VFAT2_F_QUARANTINE) != 0U){ continue; }
  if ((d[own].flags & VFAT2_F_NOHOST) != 0U){ continue; }
  if (d[own].host[0] == 0){ continue; }

  vfat2_wb[n].off = off;
  vfat2_wb[n].own = (uint16)(own);
  vfat2_wb[n].idx = (uint16)(i);
  n ++;

 }

 qsort(&(vfat2_wb[0]), n, sizeof(cu_vfat2_wb_t), &cu_vfat2_wbcmp);

 for (i = 0U; i < n; i ++){

  own = vfat2_wb[i].own;
  off = vfat2_wb[i].off;

  /* The sort puts each file's sectors together, so the log gets one line per
  ** file, given when its run ends */

  if ((i == 0U) || (vfat2_wb[i - 1U].own != own)){
   cnt  = 0U;
   lo   = off;
   hi   = off;
   fail = FALSE;
   cu_vfat2dir_show(&(d[own].name[0]), &(nstr[0]));
   cu_vfat2hf_why("guest wrote \"%s\" in slot %u", &(nstr[0]), own);
  }

  /* After one failure the rest of the file waits for the next round too. A
  ** file that cannot be opened would otherwise report once per sector. */

  /* A sector past the file's size field is held back, not written: the
  ** guest has not said it keeps those bytes, and may be about to say it does
  ** by syncing a larger size, or may never. Writing it and then shortening the
  ** file to the size field -- which is what makes the file's length right --
  ** would lose it for good, and a later sync would zero-fill the gap where
  ** the guest's data was. Held, it stays readable and goes out once the size
  ** reaches it. A sector the size field ends inside has the part up to that
  ** end written, and is held for the rest. */

  ds   = &(vfat2_dirty[vfat2_wb[i].idx]);
  lim  = vfat2_limit[own];
  len  = 512U;
  hold = FALSE;

  if ((lim != VFAT2_NOLIMIT) && ((off + 512U) > lim)){
   hold = TRUE;
   len  = (off >= lim) ? 0U : (lim - off);
   if ((ds->past) && (ds->plim == lim)){ len = 0U; } /* Written that far already */
  }

  ds->past = hold;
  ds->plim = lim;

  if ((!fail) && (len != 0U)){

   if (cu_vfat2hf_put(&(d[own].host[0]), off, &(ds->data[0]), len)){

    if ((off + len) > vfat2_high[own]){ vfat2_high[own] = off + len; }
    if (cnt == 0U){ lo = off; }
    hi = off + len;
    cnt ++;

    if (!hold){ cu_vfat2_ddrop(vfat2_wb[i].idx); }

   }else{

    fail = TRUE;                       /* Held back, and tried again next round */

   }

  }

  /* Not written this round, for a failure on this file: whatever part of it is
  ** due has to be written next round, not taken as done */

  if ((fail) && (len != 0U)){ ds->plim = VFAT2_NOLIMIT; }

  if ( (cnt != 0U) &&
       ((i == (n - 1U)) || (vfat2_wb[i + 1U].own != own)) ){
   cu_vfat2hf_log("write   \"%s\"  %u sector(s) @ %u..%u",
                  &(d[own].host[0]), cnt, lo, hi - 1U);
  }

 }

 cu_vfat2hf_why("%s", "");
}



/*
** Whether an entry's size field is consistent with the number of clusters its
** chain actually holds: the last one partly or fully used, and no more chain
** than the size needs. A field that does not fit is one the guest has not
** brought up to date yet.
*/
static boole cu_vfat2_size_fits(cu_vfat2_ent_t const* e)
{
 if (e->chain == 0U){ return (e->size == 0U); }

 if (e->size <= ((e->chain - 1U) << VFAT2_CLU_SHIFT)){ return FALSE; }
 if (e->size >  ( e->chain       << VFAT2_CLU_SHIFT)){ return FALSE; }

 return TRUE;
}



/*
** Step 5 of section 6.5, under the rule of section 6.6.
**
** During f_write() the directory entry's size field is stale -- FatFs updates
** it at f_sync() or f_close() only -- while the data sectors are already going
** out. A field that fits the chain is taken as the length all the same,
** because the writeback held back whatever lies past it instead of writing it,
** so the cut costs nothing the guest may still commit. A field that does not
** fit is an append into new clusters under way, and then the host length is
** only pulled down by an observed decrease in the field.
**
** It also settles a quieter defect. The legacy backend writes 512 byte blocks
** and never applies the directory size at all, so every file a ROM creates
** ends up rounded up to a multiple of 512 with trailing rubbish. Here a file
** the guest closed is exactly the length the guest says it is.
*/
static void cu_vfat2_apply_resize(cu_vfat2_ent_t const* d, boole destructive,
                                  char const* why, auint* defer)
{
 auint s;
 char  nstr[16];

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){

  auint       hlen;
  auint       target;
  auint       claim;
  char const* rsn;

  if ((d[s].flags & VFAT2_F_USED) == 0U){ continue; }
  if ((d[s].flags & VFAT2_F_QUARANTINE) != 0U){ continue; }
  if ((d[s].flags & VFAT2_F_NOHOST) != 0U){ continue; }
  if (d[s].host[0] == 0){ continue; }

  hlen = cu_vfat2hf_size(&(d[s].host[0]));
  if (hlen == 0xFFFFFFFFU){ continue; }

  /* A size field claiming more than the chain holds is not a length to extend
  ** the host file to. Taken at its word, a single corrupt or not yet linked
  ** entry would pad the host file with zeros up to whatever it says -- up to
  ** 4 GBytes. Such an entry counts as claiming nothing, so the bytes actually
  ** written decide, until its chain catches up or its field comes back down. */

  claim = d[s].size;
  if (claim > (d[s].chain << VFAT2_CLU_SHIFT)){ claim = 0U; }

  target = claim;
  rsn    = "size field";

  if (vfat2_limit[s] != VFAT2_NOLIMIT){

   /* The size field fits the chain -- the last cluster partly or wholly used,
   ** as a synced or closed file has it -- so it is the length, for a file the
   ** backend already knew as much as for a new one. Anything the guest wrote
   ** past it was held back in the sector store by the writeback rather than
   ** written, so shortening the host file to it cannot lose a byte the guest
   ** may yet commit. That is what lets a closed file be exactly its size
   ** rather than padded to the end of its last sector. */

   if ( (vfat2_dmatch[s] != VFAT2_NONE) &&
        (d[s].size < vfat2_c[vfat2_dmatch[s]].size) ){ rsn = "size field decreased"; }

  }else if (vfat2_dmatch[s] == VFAT2_NONE){

   /* A new file whose size field does not fit its chain is mid-append: FatFs
   ** leaves the field at 0 until the first f_sync(), so the bytes written are
   ** what to go by */

   if (vfat2_high[s] > target){ target = vfat2_high[s]; rsn = "written data, size field not set"; }

  }else if ( (claim == d[s].size) &&
             (d[s].size < vfat2_c[vfat2_dmatch[s]].size) ){

   /* Observed shrink: the guest truncated, and only this says so */

   rsn = "size field decreased";

  }else{

   /* A known file whose size field does not fit its chain: an append into new
   ** clusters in progress, not yet synced. Nothing is taken off it. */

   if (vfat2_high[s] > target){ target = vfat2_high[s]; rsn = "written data past size field"; }
   if (hlen         > target){ target = hlen; }

  }

  if (target == hlen){ continue; }

  if ((target < hlen) && (!destructive)){
   cu_vfat2hf_report(TRUE, "Deferred shortening \"%s\" to %u bytes (%s)",
                     &(d[s].host[0]), target, why);
   (*defer) ++;
   continue;
  }

  cu_vfat2dir_show(&(d[s].name[0]), &(nstr[0]));
  cu_vfat2hf_why("\"%s\" in slot %u, %s", &(nstr[0]), s, rsn);

  if (cu_vfat2hf_truncate(&(d[s].host[0]), target)){
   print_message("cu_sdhost: \"%s\" is now %u bytes, was %u\n",
                 &(d[s].host[0]), target, hlen);
   cu_vfat2hf_log("resize  \"%s\"  %u -> %u  (%s)",
                  &(d[s].host[0]), hlen, target, rsn);
  }else{
   (*defer) ++;
  }

 }

 cu_vfat2hf_why("%s", "");
}



/*
** Ages what is still held back and gives up on what is never going to be
** claimed (section 6.7). A discarded sector is data loss, and is said so every
** time with the sector and the cluster, because the alternative is the legacy
** backend's behaviour of losing it without a word.
*/
static void cu_vfat2_age_orphans(void)
{
 auint i;
 auint held   = 0U;
 auint oldest = 0U;
 auint past   = 0U;
 auint gone   = 0U;
 auint own;
 auint off;

 memset(&vfat2_lost, 0, sizeof(vfat2_lost));

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){

  if ( (vfat2_dirty[i].lba == VFAT2_DIRTY_FREE) ||
       (vfat2_dirty[i].lba == VFAT2_DIRTY_GONE) ){ continue; }

  /* Held back past a file's size (section 6.6): not an orphan while the file
  ** still owns its cluster, so it does not age. Once the file no longer does
  ** -- deleted, or truncated off that cluster -- the bytes were never part of
  ** any file the guest kept, and go without a console line. */

  if (vfat2_dirty[i].past){
   if (cu_vfat2_place_sector(vfat2_dirty[i].lba, &own, &off)){ past ++; continue; }
   cu_vfat2_ddrop(i);
   gone ++;
   continue;
  }

  vfat2_dirty[i].age ++;

  if (vfat2_dirty[i].age < CU_VFAT2_ORPHAN_MAX){
   held ++;
   if (vfat2_dirty[i].age > oldest){ oldest = vfat2_dirty[i].age; }
   continue;
  }

  cu_vfat2_lost_add(&vfat2_lost, vfat2_dirty[i].lba, vfat2_dirty[i].age);
  cu_vfat2_ddrop(i);

 }

 cu_vfat2_lost_say(&vfat2_lost, "discarded");

 if (held != 0U){
  cu_vfat2hf_log("held    %u sector(s) still unplaced, oldest age %u of %u",
                 held, oldest, CU_VFAT2_ORPHAN_MAX);
 }
 if (past != 0U){
  cu_vfat2hf_log("held    %u sector(s) past the size field of their file", past);
 }
 if (gone != 0U){
  cu_vfat2hf_log("dropped %u sector(s) held past a file's size, whose cluster it no longer owns", gone);
 }

 vfat2_dirty_past = past;
}



/*
** Reconciles the model against the host directory: section 6, whole.
**
** "destructive" is the gate of section 6.8. With it clear only the additive
** half runs -- creates, data writeback and extends -- and nothing is removed
** or shortened. Every skipped operation is benign and self correcting: a file
** that should have been deleted survives until the next round, a file that
** should have been shortened keeps its tail. Every operation still applied is
** additive and cannot destroy what was already there. That asymmetry is the
** point, because a spurious delete driven by a directory the guest was halfway
** through writing is permanent and silent.
*/
static void cu_vfat2_reconcile(boole destructive, char const* trig)
{
 cu_vfat2_ent_t* d;
 auint           s;
 auint           defer = 0U;
 auint           ms;
 char            why[64];

 if (!vfat2_ready){ return; }
 if (vfat2_failed){ return; }

 /* The common case, costing one branch per frame */

 if ((!vfat2_pending) && (vfat2_dirty_cnt == vfat2_dirty_past)){ return; }

 vfat2_recno ++;

 /* What started this round and whether it may remove anything, which is what
 ** the log heads it with and what every deferral names as its reason. Frames
 ** are shown as milliseconds at 60Hz, clamped so the product stays in 32 bits;
 ** an hour is as good as forever for this. */

 ms = (vfat2_idlefr > 216000U) ? 216000U : vfat2_idlefr;
 ms = (ms * 50U) / 3U;

 if (strcmp(trig, "idle") == 0){
  snprintf(&(why[0]), sizeof(why), "idle %ums", ms);
 }else if (destructive){
  snprintf(&(why[0]), sizeof(why), "%s, bus quiet %ums", trig, ms);
 }else if (strcmp(trig, "pressure") == 0){
  snprintf(&(why[0]), sizeof(why), "%s, sector store full", trig);
 }else{
  snprintf(&(why[0]), sizeof(why), "%s, last SD access %ums ago", trig, ms);
 }

 cu_vfat2hf_logrec("reconcile #%u  (%s: %s)", vfat2_recno,
                   destructive ? "destructive" : "non-destructive", &(why[0]));

 d = cu_vfat2dir_table();
 cu_vfat2dir_scan(&(vfat2_sys[0]), TRUE);
 vfat2_ownstale = FALSE;

 /* Renames only where something may be removed. Anywhere else a name change
 ** is made as a copy under the new name, and the old file is left alone. */

 cu_vfat2dir_match(&(vfat2_c[0]), &(vfat2_dmatch[0]), &(vfat2_cmatch[0]),
                   &(vfat2_dcopy[0]), destructive);

 memset(&(vfat2_retain  [0]), 0, sizeof(vfat2_retain));
 memset(&(vfat2_high    [0]), 0, sizeof(vfat2_high));
 memset(&(vfat2_csrc    [0]), 0, sizeof(vfat2_csrc));
 memset(&(vfat2_cfail   [0]), 0, sizeof(vfat2_cfail));
 memset(&(vfat2_keepname[0]), 0, sizeof(vfat2_keepname));

 /* Which deleted files a new one is to be copied from. Not for an entry that
 ** gets no host file this round anyway, nor from a file that has none. */

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){
  if (vfat2_dcopy[s] == VFAT2_NONE){ continue; }
  if ( ((d[s].flags & VFAT2_F_QUARANTINE) != 0U) ||
       (vfat2_c[vfat2_dcopy[s]].host[0] == 0) ){
   vfat2_dcopy[s] = VFAT2_NONE;
   continue;
  }
  vfat2_csrc[vfat2_dcopy[s]] = TRUE;
 }

 /* The order is load bearing, because host names are a namespace these
 ** operations move each other around in (section 6.5) */

 cu_vfat2_apply_deletes   (d, destructive, &(why[0]), &defer);
 cu_vfat2_apply_renames   (d, &defer);
 cu_vfat2_apply_creates   (d);
 cu_vfat2_apply_srcdeletes(d, destructive, &(why[0]), &defer);
 cu_vfat2_apply_writeback (d);
 cu_vfat2_apply_resize    (d, destructive, &(why[0]), &defer);

 /* C := D, carrying each matched entry's host name forward. A C entry whose
 ** removal was deferred or failed keeps its slot, so the next reconcile still
 ** knows which host file it has to deal with. */

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){
  if (vfat2_retain[s]){ continue; }
  vfat2_c[s] = d[s];
  if (vfat2_keepname[s]){ memcpy(&(vfat2_c[s].name[0]), &(vfat2_oldname[s][0]), 11U); }
 }

 /* C and D now agree slot for slot, so the read path's D-to-C mapping is the
 ** identity until the guest next touches its directory */

 for (s = 0U; s < VFAT2_ROOT_SIZE; s ++){
  vfat2_dmatch[s] = (uint16)(s);
  vfat2_dcopy [s] = VFAT2_NONE;
 }

 cu_vfat2_age_orphans();

 /* Anything left over earns another round */

 vfat2_pending = (defer != 0U) || (vfat2_dirty_cnt != vfat2_dirty_past);

 if (defer != 0U){
  cu_vfat2hf_log("left    %u operation(s) for a later round", defer);
 }

 cu_vfat2hf_logend();

 /* Section 7.3: an open handle stops the user's own editor from touching a
 ** card file on Windows, so nothing is held between reconciles */

 cu_vfat2hf_closeall();
}



/*
** Writes a file's directory entry into sys[] and lays its cluster chain into
** both FATs, filling in the C table entry for it.
*/
static void cu_vfat2_place(auint slot, uint8 const* name, char const* host,
                           auint siz, auint fclus, auint chain)
{
 uint8* ent = &(vfat2_sys[VFAT2_ROOT + (slot * 32U)]);
 auint  cpos;
 auint  i;

 memcpy(&(ent[0]), name, 11U);
 ent[11] = 0x20U;                      /* Archive: an ordinary file */

 ent[26] = (uint8)( fclus        & 0xFFU);
 ent[27] = (uint8)((fclus >>  8) & 0xFFU);
 ent[28] = (uint8)( siz          & 0xFFU);
 ent[29] = (uint8)((siz   >>  8) & 0xFFU);
 ent[30] = (uint8)((siz   >> 16) & 0xFFU);
 ent[31] = (uint8)((siz   >> 24) & 0xFFU);

 for (i = 0U; i < chain; i ++){
  auint nxt = (i == (chain - 1U)) ? 0xFFFFU : (fclus + i + 1U);
  cpos = fclus + i;
  vfat2_sys[VFAT2_FAT0 + (cpos << 1) + 0U] = (uint8)( nxt       & 0xFFU);
  vfat2_sys[VFAT2_FAT0 + (cpos << 1) + 1U] = (uint8)((nxt >> 8) & 0xFFU);
  vfat2_sys[VFAT2_FAT1 + (cpos << 1) + 0U] = (uint8)( nxt       & 0xFFU);
  vfat2_sys[VFAT2_FAT1 + (cpos << 1) + 1U] = (uint8)((nxt >> 8) & 0xFFU);
 }

 vfat2_c[slot].slot  = (uint16)(slot);
 vfat2_c[slot].fclus = (uint16)(fclus);
 vfat2_c[slot].size  = siz;
 vfat2_c[slot].chain = chain;
 vfat2_c[slot].attr  = 0x20U;
 memcpy(&(vfat2_c[slot].name[0]), name, 11U);
 vfat2_c[slot].flags = VFAT2_F_USED;
 strncpy(&(vfat2_c[slot].host[0]), host, CU_VFAT2_HNAME_SIZE - 1U);
 vfat2_c[slot].host[CU_VFAT2_HNAME_SIZE - 1U] = 0;
}



/*
** Derives a FAT 8.3 name for a host file which no earlier slot already holds,
** appending the usual "~n" tail on a collision (section 7.4). Without this the
** FAT namespace would not be bijective with the host directory, and a delete
** or rename would be ambiguous about which host file it meant. The legacy
** backend does not dedup, which is harmless only because it never deletes
** anything. Returns FALSE if no free name could be derived.
*/
static boole cu_vfat2_uniquename(char const* host, auint upto, uint8* dest)
{
 uint8 base[11];
 auint n;
 auint i;

 cu_vfat2hf_tofat(host, &(base[0]));
 memcpy(dest, &(base[0]), 11U);

 for (n = 0U; n < 1000U; n ++){

  boole clash = FALSE;

  for (i = 0U; i < upto; i ++){
   if ((vfat2_c[i].flags & VFAT2_F_USED) == 0U){ continue; }
   if (memcmp(&(vfat2_c[i].name[0]), dest, 11U) == 0){ clash = TRUE; break; }
  }

  if (!clash){ return TRUE; }

  memcpy(dest, &(base[0]), 11U);
  if (!cu_vfat2hf_mangle(dest, n + 1U)){ return FALSE; }

 }

 return FALSE;
}



/*
** Builds the volume from the card directory, which is the whole of a reset.
*/
static void cu_vfat2_build(void)
{
 auint i;
 auint siz;
 auint chain;
 auint cpos = 0x0002U;                 /* Next free cluster */
 auint slot = 0U;
 uint8 name[11];
 char  host[CU_VFAT2_HNAME_SIZE];

 memset(&(vfat2_sys[0]), 0, sizeof(vfat2_sys));
 memset(&(vfat2_c  [0]), 0, sizeof(vfat2_c));
 cu_vfat2_dclear();

 vfat2_stage_act = FALSE;
 vfat2_stage_len = 0U;
 vfat2_stage_lba = 0U;
 vfat2_ownstale  = TRUE;
 vfat2_pending   = FALSE;
 vfat2_idlefr    = 0U;
 vfat2_recwait   = 0U;

 /* Master Boot Record */

 for (i = 0U; i < VFAT2_MBR_SIZE; i ++){
  vfat2_sys[i] = vfat2_data_mbr[i];
 }
 vfat2_sys[0x0001FEU] = 0x55U;
 vfat2_sys[0x0001FFU] = 0xAAU;

 /* FAT0, then FAT1 as its copy */

 for (i = 0U; i < VFAT2_FATSEED_SIZE; i ++){
  vfat2_sys[VFAT2_FAT0 + i] = vfat2_data_fat[i];
 }
 memcpy(&(vfat2_sys[VFAT2_FAT1]), &(vfat2_sys[VFAT2_FAT0]), VFAT2_ROOT - VFAT2_FAT1);

 /* Root directory, from the card directory's listing */

 cu_vfat2hf_find_reset();

 while (slot < VFAT2_ROOT_SIZE){

  siz = cu_vfat2hf_find_next(&(host[0]), CU_VFAT2_HNAME_SIZE);
  if (siz == 0xFFFFFFFFU){ break; }

  /* A zero length file gets no cluster at all, as a real FAT volume has it.
  ** The legacy backend allocates one regardless, which costs a cluster and
  ** gives the ownership map something to point at that holds nothing. */

  chain = (siz + (VFAT2_CLU_SIZE - 1U)) >> VFAT2_CLU_SHIFT;

  if ((cpos + chain) > VFAT2_DATA_END){
   cu_vfat2hf_report(TRUE, "\"%s\" does not fit on the card, left off it", &(host[0]));
   continue;
  }

  if (!cu_vfat2_uniquename(&(host[0]), slot, &(name[0]))){
   cu_vfat2hf_report(TRUE, "\"%s\" has no free 8.3 name, left off the card", &(host[0]));
   continue;
  }

  cu_vfat2_place(slot, &(name[0]), &(host[0]), siz,
                 (chain != 0U) ? cpos : 0U, chain);

  cpos += chain;
  slot ++;

 }

 /* The root directory is full. Anything not reached is still on the host, but
 ** not on the card, which would otherwise go unsaid. */

 if (slot == VFAT2_ROOT_SIZE){
  i = 0U;
  while (cu_vfat2hf_find_next(&(host[0]), CU_VFAT2_HNAME_SIZE) != 0xFFFFFFFFU){ i ++; }
  if (i != 0U){
   cu_vfat2hf_report(TRUE, "%u more file(s) left off the card, its root directory holds %u",
                     i, VFAT2_ROOT_SIZE);
  }
 }

 cu_vfat2hf_find_end();

 cu_vfat2hf_log("card    \"%s\"  %u file(s), %u cluster(s) in use",
                cu_vfat2hf_path(), slot, cpos - 2U);

 if (vfat2_rescan){ vfat2_sig = cu_vfat2hf_signature(); }
 vfat2_scanfr = 0U;
}



void  cu_vfat2_init(char const* game, char const* name)
{
 strncpy(&(vfat2_game[0]), game, sizeof(vfat2_game) - 1U);
 vfat2_game[sizeof(vfat2_game) - 1U] = 0;

 strncpy(&(vfat2_dirname[0]), name, sizeof(vfat2_dirname) - 1U);
 vfat2_dirname[sizeof(vfat2_dirname) - 1U] = 0;

 vfat2_ready = TRUE;
}



/*
** Says what is still held back and then lets it go, for the points where the
** model is about to be thrown away and there will be no next round.
*/
static void cu_vfat2_dgiveup(char const* why)
{
 auint i;
 auint past = 0U;
 char  how[32];

 memset(&vfat2_lost, 0, sizeof(vfat2_lost));
 snprintf(&(how[0]), sizeof(how), "lost at %s", why);

 for (i = 0U; i < CU_VFAT2_DIRTY_MAX; i ++){

  if ( (vfat2_dirty[i].lba == VFAT2_DIRTY_FREE) ||
       (vfat2_dirty[i].lba == VFAT2_DIRTY_GONE) ){ continue; }

  /* Bytes past a file's size field are not in the file as the guest left its
  ** directory, so letting them go is not data loss the console needs to
  ** hear about; the log still says so */

  if (vfat2_dirty[i].past){ past ++; continue; }

  cu_vfat2_lost_add(&vfat2_lost, vfat2_dirty[i].lba, vfat2_dirty[i].age);

 }

 cu_vfat2_lost_say(&vfat2_lost, &(how[0]));

 if (past != 0U){
  cu_vfat2hf_log("dropped %u sector(s) held past their file's size field, at %s", past, why);
 }

 cu_vfat2_dclear();
}



void  cu_vfat2_reset(void)
{
 if (!vfat2_ready){ return; }          /* Reset can arrive before init */

 /* Section 6.8, and it is worth being exact about why the reconcile happens
 ** at all here. Not reconciling loses everything the ROM wrote in the last
 ** fifth of a second -- in a workflow where reset gets hit constantly, that
 ** is precisely the data being tested. Reconciling destructively over a
 ** directory the guest was interrupted partway through writing deletes files
 ** that only looked deleted. So both happen, and the destructive half is
 ** gated on the bus having been quiet. In practice the gate passes: hitting
 ** reset within 200ms of the last SD byte takes deliberate effort. */

 if (!vfat2_failed){
  cu_vfat2_reconcile((vfat2_idlefr >= CU_VFAT2_IDLE_FR), "reset");
  cu_vfat2_dgiveup("reset");
 }

 vfat2_stage_act = FALSE;
 vfat2_stage_len = 0U;

 cu_vfat2hf_closeall();

 /* Whether a card can be served is decided again at every reset, so creating
 ** a missing card directory by hand and hitting reset recovers it without
 ** restarting the emulator (section 5.5) */

 vfat2_failed = !cu_vfat2hf_setup(&(vfat2_game[0]), &(vfat2_dirname[0]));

 if (vfat2_failed){
  memset(&(vfat2_sys[0]), 0, sizeof(vfat2_sys));
  memset(&(vfat2_c  [0]), 0, sizeof(vfat2_c));
  cu_vfat2_dclear();
  return;
 }

 cu_vfat2_build();
}



auint cu_vfat2_read_begin(auint lba)
{
 (void)(lba);

 vfat2_idlefr = 0U;

 if (vfat2_failed){ return 0x01U; }    /* Data error token: no data follows */

 return 0xFEU;                         /* Read data token */
}



auint cu_vfat2_read(auint pos)
{
 cu_vfat2_dirty_t* dir;
 auint             clus;
 auint             own;
 auint             ret;

 vfat2_idlefr = 0U;

 if (vfat2_failed){ return 0x00U; }

 if (pos < VFAT2_DATA){ return vfat2_sys[pos]; }

 /* (a) A sector the guest wrote but which has not reached the host yet is
 ** served from where it actually is. The legacy backend has no equivalent and
 ** hands back stale host bytes for any such read. */

 dir = cu_vfat2_dfind(pos >> 9);
 if (dir != NULL){ return dir->data[pos & 0x1FFU]; }

 /* (b) An explicit bounds check. cu_spisd.c masks the block address to
 ** 0x007FFFFF, so a guest can address 4 GBytes against a 2 GByte volume. */

 clus = ((pos - VFAT2_DATA) >> VFAT2_CLU_SHIFT) + 2U;
 if (clus >= VFAT2_DATA_END){ return 0x00U; }

 if (vfat2_ownstale){ cu_vfat2_ownmap(); }

 /* (c) An unallocated cluster reads as zeros without disturbing any global
 ** state. cu_vfat.c can latch vfat_broken from inside a read and so disable
 ** the entire data area for every file on the card. */

 own = cu_vfat2dir_owner(clus);
 if (own >= VFAT2_ROOT_SIZE){ return 0x00U; }

 /* Which committed entry that is. Resolved by identity rather than by slot
 ** number: a slot the guest has reused since the last reconcile holds one file
 ** in the guest's directory and another in C, and taking C's entry at the same
 ** index would read through to an unrelated host file. An owning slot with no
 ** committed entry behind it yet is a file whose data has not been placed, and
 ** reads as zeros. */

 /* A new file that will start as a copy of a deleted one reads as that file
 ** until the copy is made, so what the guest reads does not depend on whether
 ** a reconcile has happened in between */

 own = (vfat2_dmatch[own] != VFAT2_NONE) ? vfat2_dmatch[own] : vfat2_dcopy[own];
 if (own >= VFAT2_ROOT_SIZE){ return 0x00U; }
 if ((vfat2_c[own].flags & VFAT2_F_USED) == 0U){ return 0x00U; }
 if ((vfat2_c[own].flags & VFAT2_F_NOHOST) != 0U){ return 0x00U; }

 /* (d) Read through to the host file. A cluster ordinal of 65519 at 32 KBytes
 ** is 0x7FFF8000, so the offset stays inside 32 bits.
 **
 ** The offset within the cluster is taken relative to the start of the data
 ** area, not from the absolute position. Section 5.1 of the spec writes it as
 ** "pos & 0x7FFF", which only holds if the data area begins on a cluster
 ** boundary -- it begins at 0x44200, which is 0x4200 past one, so that form
 ** reads every file 16896 bytes late. */

 ret = cu_vfat2hf_getb(&(vfat2_c[own].host[0]),
                       (cu_vfat2dir_cidx(clus) << VFAT2_CLU_SHIFT) +
                       ((pos - VFAT2_DATA) & (VFAT2_CLU_SIZE - 1U)));
 if (ret == 0xFFFFFFFFU){ return 0x00U; }

 return ret;
}



void  cu_vfat2_write(auint pos, auint data)
{
 auint lba = pos >> 9;

 vfat2_idlefr = 0U;

 /* A block beginning while another is still in flight means the earlier one
 ** never reached its CRC, so a real card would have left it unprogrammed */

 if ((!vfat2_stage_act) || (vfat2_stage_lba != lba)){
  vfat2_stage_lba = lba;
  vfat2_stage_len = 0U;
  vfat2_stage_act = TRUE;
 }

 vfat2_stage[pos & 0x1FFU] = (uint8)(data);
 vfat2_stage_len ++;
}



/*
** Whether a root directory block holds a subdirectory entry: attribute bit
** 0x10, in an entry that is neither free, deleted, nor "." or "..". A long file
** name fragment has the attribute 0x0F and a volume label 0x08, neither of which
** has that bit, so both pass.
*/
static boole cu_vfat2_hasdir(uint8 const* blk)
{
 auint i;

 for (i = 0U; i < 512U; i += 32U){
  if (blk[i] == 0x00U){ continue; }
  if (blk[i] == 0xE5U){ continue; }
  if (blk[i] == 0x2EU){ continue; }
  if ((blk[i + 11U] & 0x10U) != 0U){ return TRUE; }
 }

 return FALSE;
}



auint cu_vfat2_write_end(boole crcok)
{
 auint lba = vfat2_stage_lba;
 auint base;
 auint len = vfat2_stage_len;
 boole act = vfat2_stage_act;

 vfat2_idlefr    = 0U;
 vfat2_stage_act = FALSE;
 vfat2_stage_len = 0U;

 if (vfat2_failed){ return 0x0DU; }    /* Write error (section 5.5) */

 /* A block cut short -- chip select released partway through it -- is never
 ** programmed, whatever the CRC setting. The legacy backend has already
 ** written its first bytes through by the time it would know. */

 if ((!act) || (len != 512U)){
  cu_vfat2hf_report((vfat2_cnt_short == 0U),
                    "Block 0x%06X was cut short at %u of 512 bytes, refused%s",
                    lba, act ? len : 0U,
                    (vfat2_cnt_short == 0U) ? " (further ones are counted, total at exit)" : "");
  vfat2_cnt_short ++;
  return 0x0BU;
 }

 /* A block with a bad CRC is refused, and so reaches neither the host nor the
 ** system area. The legacy backend reports the error but has already written
 ** the block, so a corrupt FAT block stays in place and misroutes every data
 ** write after it. "--sdhost-crc=ignore" puts that behaviour back for a ROM
 ** that depends on it: the block is kept and the error token still sent, the
 ** same pair the legacy backend produces. */

 if (!crcok){
  cu_vfat2hf_report((vfat2_cnt_crc == 0U),
                    "Block 0x%06X has a bad CRC, %s%s", lba,
                    vfat2_crcignore ? "kept anyway (--sdhost-crc=ignore)" : "refused",
                    (vfat2_cnt_crc == 0U) ? " (further ones are counted, total at exit)" : "");
  vfat2_cnt_crc ++;
  if (!vfat2_crcignore){ return 0x0BU; }
 }

 base = lba << 9;

 if (base < VFAT2_DATA){

  /* A subdirectory is refused at the moment the guest registers it in the
  ** root directory, so f_mkdir() returns FR_DISK_ERR there and then. Merely
  ** ignoring it would give the ROM FR_OK for a directory that does not exist. */

  if ((base >= VFAT2_ROOT) && cu_vfat2_hasdir(&(vfat2_stage[0]))){
   cu_vfat2hf_report((vfat2_cnt_dir == 0U),
                     "Block 0x%06X would create a subdirectory, refused (not supported)%s", lba,
                     (vfat2_cnt_dir == 0U) ? " (further ones are counted, total at exit)" : "");
   vfat2_cnt_dir ++;
   return (crcok ? 0x0DU : 0x0BU);
  }

  memcpy(&(vfat2_sys[base]), &(vfat2_stage[0]), 512U);

  if ( (base >= VFAT2_ROOT) ||
       ((base >= VFAT2_FAT0) && (base < VFAT2_FAT1)) ){ /* Root dir, or FAT0 */
   vfat2_ownstale = TRUE;
   vfat2_pending  = TRUE;
  }

  return (crcok ? 0x05U : 0x0BU);

 }

 /* Past the end of the volume. The block address is only masked to 23 bits,
 ** so a guest can address twice the card; such a block has nowhere to go and
 ** would otherwise sit in the sector store until it was given up on. */

 if (lba >= VFAT2_SEC_TOTAL){
  cu_vfat2hf_report((vfat2_cnt_range == 0U),
                    "Block 0x%06X is past the end of the card, refused%s", lba,
                    (vfat2_cnt_range == 0U) ? " (further ones are counted, total at exit)" : "");
  vfat2_cnt_range ++;
  return 0x0DU;
 }

 /* Pressure (section 6.1). Never destructive: the guest is mid-burst by
 ** definition, so its directory is the last thing to take a delete from. */

 if (vfat2_dirty_cnt >= CU_VFAT2_DIRTY_MAX){
  cu_vfat2_reconcile(FALSE, "pressure");
  if (vfat2_dirty_cnt >= CU_VFAT2_DIRTY_MAX){ cu_vfat2_devict(); }
 }

 cu_vfat2_dinsert(lba);
 vfat2_pending = TRUE;

 return (crcok ? 0x05U : 0x0BU);       /* Accepted, or kept despite its CRC */
}



void  cu_vfat2_setcrc(boole ignore)
{
 vfat2_crcignore = ignore;
}



void  cu_vfat2_setrescan(boole on)
{
 vfat2_rescan = on;
}



/*
** "--sdhost-rescan": a file edited, added or removed on the host while the ROM
** runs is otherwise invisible until reset, and worse than invisible when it is
** edited -- the directory keeps the old size while reads go through to the new
** bytes. So once the bus has been quiet and nothing the guest wrote is still
** held, the card directory is compared against what the volume was built from,
** and on any difference the volume is rebuilt exactly as a reset builds it.
*/
static void cu_vfat2_rescan_tick(void)
{
 if (!vfat2_rescan){ return; }
 if (vfat2_idlefr < CU_VFAT2_IDLE_FR){ return; }
 /* Sectors held only for bytes past a file's size (dirty_past) do not stop it:
 ** a reset drops them the same way */
 if ((vfat2_dirty_cnt != vfat2_dirty_past) || vfat2_stage_act){ return; }

 vfat2_scanfr ++;
 if (vfat2_scanfr < CU_VFAT2_SCAN_FR){ return; }
 vfat2_scanfr = 0U;

 if (cu_vfat2hf_signature() == vfat2_sig){ return; }

 cu_vfat2hf_report(TRUE, "card directory changed on the host, card rebuilt");
 cu_vfat2_dgiveup("rescan");
 cu_vfat2hf_closeall();
 cu_vfat2_build();
}



void  cu_vfat2_write_abort(void)
{
 vfat2_stage_act = FALSE;
 vfat2_stage_len = 0U;
}



void  cu_vfat2_frametick(void)
{
 if (!vfat2_ready){ return; }
 if (vfat2_failed){ return; }

 /* Counted unconditionally, because this is what section 6.8 asks at reset
 ** and at exit: how long the SD bus has been quiet. Every read and write
 ** zeroes it. */

 if (vfat2_idlefr < 0xFFFFFFFFU){ vfat2_idlefr ++; }

 if (vfat2_recwait != 0U){ vfat2_recwait --; return; }

 if (!vfat2_pending){ cu_vfat2_rescan_tick(); return; }

 if (vfat2_idlefr >= CU_VFAT2_IDLE_FR){

  cu_vfat2_reconcile(TRUE, "idle");

  /* The reconcile's own host writes are not a change made on the host */
  if (vfat2_rescan){ vfat2_sig = cu_vfat2hf_signature(); }

  /* Whatever is left waits its turn, and waits noticeably longer if the last
  ** round could not finish the job: a host operation failing for a reason
  ** outside the emulator -- the file open in an editor, the directory read
  ** only -- would otherwise retry and report five times a second. */

  vfat2_recwait = vfat2_pending ? (CU_VFAT2_IDLE_FR * 5U) : CU_VFAT2_IDLE_FR;

 }
}



void  cu_vfat2_quit(void)
{
 if (!vfat2_ready){ return; }

 /* The same rule and the same reasoning as reset: the user may well have
 ** closed the window mid-write (section 6.8) */

 if (!vfat2_failed){
  cu_vfat2_reconcile((vfat2_idlefr >= CU_VFAT2_IDLE_FR), "exit");
  cu_vfat2_dgiveup("exit");
 }

 cu_vfat2hf_closeall();

 /* The totals behind the once per session console lines (section 8.4). Given
 ** whenever there is anything to count, including a count of one, so a run
 ** always ends with the same summary for the same refusals. */

 if (vfat2_cnt_crc != 0U){
  cu_vfat2hf_report(TRUE, "%u block(s) with a bad CRC this session, %s",
                    vfat2_cnt_crc, vfat2_crcignore ? "all kept" : "all refused");
 }
 if (vfat2_cnt_dir != 0U){
  cu_vfat2hf_report(TRUE, "%u subdirectory creation(s) refused this session", vfat2_cnt_dir);
 }
 if (vfat2_cnt_range != 0U){
  cu_vfat2hf_report(TRUE, "%u block(s) past the end of the card refused this session", vfat2_cnt_range);
 }
 if (vfat2_cnt_short != 0U){
  cu_vfat2hf_report(TRUE, "%u block(s) cut short refused this session", vfat2_cnt_short);
 }

 cu_vfat2hf_logclose();

 vfat2_ready = FALSE;                  /* atexit() must not run this twice */
}



#endif /* CU_VFAT2 */
