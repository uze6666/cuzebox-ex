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


/*
** Everything that reads the guest's volume image and turns it into structure:
** the root directory parsed into D (section 6.2), the cluster chains walked
** into the ownership map (section 6.3), and D matched against C (section 6.4).
**
** It touches nothing else. No host file is opened here and no decision is
** taken about what to do with the result -- cu_vfat2.c does that. Keeping the
** two apart is what lets the read path use the same walk the reconcile does,
** rather than a second one that can quietly disagree with it.
*/


#ifndef CU_VFAT2DIR_H
#define CU_VFAT2DIR_H


#include "types.h"
#include "cu_vfat2.h"


/*
** Parses the root directory of "sys" into D and walks every chain in it,
** rebuilding the cluster ownership map (sections 6.2 and 6.3). An entry whose
** chain loops or crosses an earlier one is quarantined: it loses its writeback
** for this round only, and everything else on the card proceeds normally.
**
** "report" asks for the console and log lines of section 8.4, each entry at
** most once per scan. The reconcile passes TRUE; the read path rebuilding the
** map on the fly passes FALSE, so a guest reading over a broken FAT does not
** flood the console with what the next reconcile will say once anyway.
*/
void  cu_vfat2dir_scan(uint8 const* sys, boole report);


/*
** Renders an 11 byte 8.3 name as "NAME    .EXT" for a message, 13 bytes with
** the terminator. Not the host name converter: this is only ever read by a
** person, so it shows what is actually in the entry, with unprintable bytes
** as '?', rather than the sanitised host form.
*/
void  cu_vfat2dir_show(uint8 const* name, char* dest);


/*
** D, the directory as the guest's sys[] currently asks for it, 512 entries.
** Writable: the reconcile fills in each entry's host name before committing
** the table as the new C.
*/
cu_vfat2_ent_t* cu_vfat2dir_table(void);


/*
** The slot owning a cluster, or VFAT2_NONE. Meaningful for clusters 2 up to
** VFAT2_DATA_END only.
*/
auint cu_vfat2dir_owner(auint clus);


/*
** A cluster's ordinal within its file's chain, so its byte offset in the host
** file is that times the cluster size.
*/
auint cu_vfat2dir_cidx(auint clus);


/*
** Resolves which D entry is which C entry, filling dmatch (indexed by D slot,
** holding a C slot or VFAT2_NONE) and cmatch (the reverse). Section 6.4: three
** passes, most specific first, and the ordering is the fix for the rename and
** slot-reuse defects rather than an optimisation -- it must not be rearranged.
**
** What is left over says the rest: an unmatched D entry is a new file, an
** unmatched C entry is a deleted one.
**
** A D entry on a C entry's first cluster under a different name is matched as
** a rename only if "renames" is set and the old slot and the rest of the entry
** say it is one. Otherwise it is new, and dcopy (indexed by D slot) names the
** deleted C entry whose host file it starts as a copy of, or VFAT2_NONE.
*/
void  cu_vfat2dir_match(cu_vfat2_ent_t const* ctab, uint16* dmatch, uint16* cmatch,
                        uint16* dcopy, boole renames);


#endif
