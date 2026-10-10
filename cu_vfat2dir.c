/*
 *  Host-directory FAT16 filesystem ("vfat2"): directory model
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



#include "cu_vfat2dir.h"

#ifdef CU_VFAT2



/* D, the directory the guest's sys[] currently asks for (section 4.2) */
static cu_vfat2_ent_t vfat2dir_d[VFAT2_ROOT_SIZE];

/* Cluster ownership (section 4.3), and each cluster's ordinal within its
** file's chain, which is what turns a sector into a host file offset */
static uint16 vfat2dir_owner[0x10000];
static uint16 vfat2dir_cidx [0x10000];

/* Clusters reached during this walk. One bitmap across all 512 chains, so a
** loop or a cross-link terminates the walk structurally rather than on a retry
** budget, and the first file to reach a cluster keeps it. */
static uint8  vfat2dir_seen [0x10000 / 8U];



void  cu_vfat2dir_show(uint8 const* name, char* dest)
{
 auint i;

 for (i = 0U; i < 8U; i ++){
  dest[i] = ((name[i] >= 0x20U) && (name[i] < 0x7FU)) ? (char)(name[i]) : '?';
 }

 dest[8] = '.';

 for (i = 8U; i < 11U; i ++){
  dest[i + 1U] = ((name[i] >= 0x20U) && (name[i] < 0x7FU)) ? (char)(name[i]) : '?';
 }

 dest[12] = 0;
}



void  cu_vfat2dir_scan(uint8 const* sys, boole report)
{
 auint slot;
 auint clus;
 auint ord;
 auint fch;
 auint attr;
 char  nstr[16];

 memset(&(vfat2dir_owner[0]), 0xFF, sizeof(vfat2dir_owner));
 memset(&(vfat2dir_cidx [0]), 0x00, sizeof(vfat2dir_cidx));
 memset(&(vfat2dir_seen [0]), 0x00, sizeof(vfat2dir_seen));
 memset(&(vfat2dir_d    [0]), 0x00, sizeof(vfat2dir_d));

 /* Section 6.2: parse the root directory into D */

 for (slot = 0U; slot < VFAT2_ROOT_SIZE; slot ++){

  uint8 const* ent = &(sys[VFAT2_ROOT + (slot * 32U)]);

  fch  = ent[0];
  if (fch == 0x00U){ continue; }        /* Free */
  if (fch == 0xE5U){ continue; }        /* Deleted */
  if (fch == 0x2EU){ continue; }        /* "." or ".." */

  attr = ent[11];

  /* An LFN fragment is skipped rather than taken for a file. The legacy
  ** backend does not check, and an LFN-enabled guest driver would have it
  ** creating a "$"-laden host file for every fragment of every long name. */

  if ((attr & 0x0FU) == 0x0FU){ continue; }
  if ((attr & 0x08U) != 0U){ continue; } /* Volume label */

  vfat2dir_d[slot].slot  = (uint16)(slot);
  vfat2dir_d[slot].attr  = (uint8)(attr);
  vfat2dir_d[slot].fclus = (uint16)((auint)(ent[26]) | ((auint)(ent[27]) << 8));
  vfat2dir_d[slot].size  = (auint)(ent[28])         |
                           ((auint)(ent[29]) <<  8) |
                           ((auint)(ent[30]) << 16) |
                           ((auint)(ent[31]) << 24);
  memcpy(&(vfat2dir_d[slot].name[0]), &(ent[0]), 11U);
  memcpy(&(vfat2dir_d[slot].ctime[0]), &(ent[13]), 5U);
  vfat2dir_d[slot].flags = VFAT2_F_USED;

  /* A subdirectory cannot occur: section 5.4 refuses the root directory block
  ** that would register one. If one is here anyway the entry is kept out of
  ** every host operation rather than being acted on. */

  if ((attr & 0x10U) != 0U){
   vfat2dir_d[slot].flags |= VFAT2_F_QUARANTINE;
   if (report){
    cu_vfat2dir_show(&(vfat2dir_d[slot].name[0]), &(nstr[0]));
    cu_vfat2hf_report(TRUE, "\"%s\" in slot %u is a subdirectory entry, ignored",
                      &(nstr[0]), slot);
   }
  }

 }

 /* Section 6.3: walk every chain, building ownership */

 for (slot = 0U; slot < VFAT2_ROOT_SIZE; slot ++){

  if ((vfat2dir_d[slot].flags & VFAT2_F_USED) == 0U){ continue; }
  if ((vfat2dir_d[slot].flags & VFAT2_F_QUARANTINE) != 0U){ continue; }

  clus = vfat2dir_d[slot].fclus;
  ord  = 0U;

  while ((clus >= 2U) && (clus < VFAT2_DATA_END)){

   if ((vfat2dir_seen[clus >> 3] & (1U << (clus & 7U))) != 0U){

    /* Either this chain has looped back on itself, or it has run into a
    ** cluster an earlier file already holds. Both entries lose their
    ** writeback for this round -- and only for this round, since the
    ** quarantine is recomputed from scratch next reconcile and clears the
    ** moment the FAT makes sense again.
    **
    ** The legacy backend cannot say this much: it bounds the walk with a step
    ** budget, and on running out latches a global "broken" flag that disables
    ** the data area for every file on the card until a system area write
    ** happens to clear it. */

    auint own = vfat2dir_owner[clus];

    vfat2dir_d[slot].flags |= VFAT2_F_QUARANTINE;

    if (report){
     cu_vfat2dir_show(&(vfat2dir_d[slot].name[0]), &(nstr[0]));
     cu_vfat2hf_report(TRUE, "\"%s\" in slot %u: chain %s at cluster 0x%04X, not written back",
                       &(nstr[0]), slot,
                       (own == slot) ? "loops" : "cross-links", clus);
    }

    /* The earlier owner is said once however many later chains run into it,
    ** which is what "once per entry per reconcile" asks (section 8.4) */

    if ( (own < VFAT2_ROOT_SIZE) && (own != slot) &&
         ((vfat2dir_d[own].flags & VFAT2_F_QUARANTINE) == 0U) ){
     vfat2dir_d[own].flags |= VFAT2_F_QUARANTINE;
     if (report){
      cu_vfat2dir_show(&(vfat2dir_d[own].name[0]), &(nstr[0]));
      cu_vfat2hf_report(TRUE, "\"%s\" in slot %u shares that cluster, not written back either",
                        &(nstr[0]), own);
     }
    }

    break;

   }

   vfat2dir_seen[clus >> 3] |= (uint8)(1U << (clus & 7U));

   vfat2dir_owner[clus] = (uint16)(slot);
   vfat2dir_cidx [clus] = (uint16)(ord);
   ord ++;

   clus = (auint)(sys[VFAT2_FAT0 + (clus << 1) + 0U]) |
          ((auint)(sys[VFAT2_FAT0 + (clus << 1) + 1U]) << 8);

  }

  vfat2dir_d[slot].chain = ord;

  /* An entry claiming more bytes than it has clusters for is not fatal:
  ** writeback is capped at the chain either way. It is said out loud because
  ** it means the guest's FAT and its directory disagree. */

  if ( (report) &&
       ((vfat2dir_d[slot].flags & VFAT2_F_QUARANTINE) == 0U) &&
       ((ord << VFAT2_CLU_SHIFT) < vfat2dir_d[slot].size) ){
   cu_vfat2dir_show(&(vfat2dir_d[slot].name[0]), &(nstr[0]));
   cu_vfat2hf_report(TRUE, "\"%s\" in slot %u claims %u bytes but its chain holds %u",
                     &(nstr[0]), slot, vfat2dir_d[slot].size, ord << VFAT2_CLU_SHIFT);
  }

 }
}



cu_vfat2_ent_t* cu_vfat2dir_table(void)
{
 return &(vfat2dir_d[0]);
}



auint cu_vfat2dir_owner(auint clus)
{
 return vfat2dir_owner[clus & 0xFFFFU];
}



auint cu_vfat2dir_cidx(auint clus)
{
 return vfat2dir_cidx[clus & 0xFFFFU];
}



/*
** Whether D entry d, which has C entry c's first cluster under another name,
** is c renamed rather than a new file the guest put on the cluster c freed.
**
** A rename leaves nothing behind but the name: the old slot is emptied or, for
** a driver that renames in place, holds the entry under its new name, and
** everything else FatFs f_rename() copies across -- size, attribute, creation
** time -- is as it was. A delete followed by a create that happens to get the
** freed cluster almost always differs in one of those, and when it does not,
** the two cannot be told apart from the volume at all.
*/
static boole cu_vfat2dir_isrename(cu_vfat2_ent_t const* ctab, auint c, auint d)
{
 cu_vfat2_ent_t const* old = &(ctab[c]);
 cu_vfat2_ent_t const* now = &(vfat2dir_d[d]);

 if ( ((vfat2dir_d[c].flags & VFAT2_F_USED) != 0U) &&
      (memcmp(&(vfat2dir_d[c].name[0]), &(old->name[0]), 11U) == 0) ){
  return FALSE;                        /* The old entry is still there */
 }

 if (now->size != old->size){ return FALSE; }
 if (now->attr != old->attr){ return FALSE; }
 if (memcmp(&(now->ctime[0]), &(old->ctime[0]), 5U) != 0){ return FALSE; }

 return TRUE;
}



void  cu_vfat2dir_match(cu_vfat2_ent_t const* ctab, uint16* dmatch, uint16* cmatch,
                        uint16* dcopy, boole renames)
{
 auint d;
 auint c;
 auint exact;
 auint ren;
 auint cpy;

 for (d = 0U; d < VFAT2_ROOT_SIZE; d ++){
  dmatch[d] = VFAT2_NONE;
  cmatch[d] = VFAT2_NONE;
  dcopy [d] = VFAT2_NONE;
 }

 /* Pass 1, first cluster. This is the file itself, wherever its directory
 ** entry has moved to -- and, if it passes the rename test, whatever it is now
 ** called.
 **
 ** It is why rename works: FatFs f_rename() writes a fresh entry in another
 ** slot, copies the first cluster across and marks the old slot 0xE5, which
 ** looks like a delete plus a create to anything keyed on the slot. Keyed on
 ** the first cluster it is one file that changed its name.
 **
 ** It is equally why slot reuse works. A slot whose contents were replaced by
 ** a different file has a different first cluster, so the old C entry falls
 ** through to "deleted" and the new D entry to "new file", and both happen
 ** against the right host files. There is no per-slot host name to inherit by
 ** mistake: the host name travels with the identity record.
 **
 ** The first cluster alone is not enough when the name changed. A guest that
 ** deletes one file and creates another before any reconcile has seen the
 ** delete may be handed the freed cluster, and the new file would be taken
 ** for the old one renamed: the old host file renamed rather than trashed, and
 ** the new file's data written over it. So a changed name is a rename only if
 ** cu_vfat2dir_isrename() agrees. Otherwise the entry is a new file whose host
 ** file starts as a copy of the old one (dcopy), which is right for both
 ** readings: a new file overwrites what it needs, and a renamed file whose size
 ** also changed keeps its unchanged bytes. The old entry is left unmatched, so
 ** it goes through the delete policy like any other delete.
 **
 ** With renames off -- a reconcile that may not remove anything -- every name
 ** change goes the copy way, so the old host file is never touched. */

 for (d = 0U; d < VFAT2_ROOT_SIZE; d ++){

  if ((vfat2dir_d[d].flags & VFAT2_F_USED) == 0U){ continue; }
  if (vfat2dir_d[d].fclus == 0U){ continue; }

  exact = VFAT2_NONE;
  ren   = VFAT2_NONE;
  cpy   = VFAT2_NONE;

  /* Same name first, wherever it lies in C: a deleted entry kept in C by an
  ** earlier deferral can share the cluster with the file now on it */

  for (c = 0U; c < VFAT2_ROOT_SIZE; c ++){
   if (cmatch[c] != VFAT2_NONE){ continue; }
   if ((ctab[c].flags & VFAT2_F_USED) == 0U){ continue; }
   if (ctab[c].fclus != vfat2dir_d[d].fclus){ continue; }
   if (memcmp(&(ctab[c].name[0]), &(vfat2dir_d[d].name[0]), 11U) == 0){ exact = c; break; }
   if ( (ren == VFAT2_NONE) && (renames) &&
        (cu_vfat2dir_isrename(ctab, c, d)) ){ ren = c; }
   else if (cpy == VFAT2_NONE){ cpy = c; }
  }

  c = (exact != VFAT2_NONE) ? exact : ren;

  if (c != VFAT2_NONE){
   dmatch[d] = (uint16)(c);
   cmatch[c] = (uint16)(d);
  }else{
   dcopy[d]  = (uint16)(cpy);
  }

 }

 /* Pass 2, slot and name both. Empty files have no cluster to be keyed on, so
 ** they are found where they were. */

 for (d = 0U; d < VFAT2_ROOT_SIZE; d ++){

  if ((vfat2dir_d[d].flags & VFAT2_F_USED) == 0U){ continue; }
  if (dmatch[d] != VFAT2_NONE){ continue; }

  c = d;
  if (cmatch[c] != VFAT2_NONE){ continue; }
  if ((ctab[c].flags & VFAT2_F_USED) == 0U){ continue; }
  if (memcmp(&(ctab[c].name[0]), &(vfat2dir_d[d].name[0]), 11U) != 0){ continue; }

  dmatch[d] = (uint16)(c);
  cmatch[c] = (uint16)(d);

 }

 /* Pass 3, name anywhere, for an empty file whose entry moved. Misattributing
 ** one costs a name and no data, which is why the weakest key is allowed to
 ** run at all. */

 for (d = 0U; d < VFAT2_ROOT_SIZE; d ++){

  if ((vfat2dir_d[d].flags & VFAT2_F_USED) == 0U){ continue; }
  if (dmatch[d] != VFAT2_NONE){ continue; }

  for (c = 0U; c < VFAT2_ROOT_SIZE; c ++){
   if (cmatch[c] != VFAT2_NONE){ continue; }
   if ((ctab[c].flags & VFAT2_F_USED) == 0U){ continue; }
   if (memcmp(&(ctab[c].name[0]), &(vfat2dir_d[d].name[0]), 11U) != 0){ continue; }
   dmatch[d] = (uint16)(c);
   cmatch[c] = (uint16)(d);
   break;
  }

 }

 /* A copy is only wanted for an entry that stayed new, from an old entry that
 ** stayed deleted. Either can have been matched by name since pass 1. */

 for (d = 0U; d < VFAT2_ROOT_SIZE; d ++){
  if (dcopy[d] == VFAT2_NONE){ continue; }
  if ( (dmatch[d] != VFAT2_NONE) ||
       (cmatch[dcopy[d]] != VFAT2_NONE) ){ dcopy[d] = VFAT2_NONE; }
 }
}



#endif /* CU_VFAT2 */
