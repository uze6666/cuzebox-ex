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



#include "cu_sdb.h"

#ifdef CU_VFAT2

#include "cu_vfat.h"
#include "cu_vfat2.h"

#include <stdlib.h>



/* Card directory name if "--sdhost" is given without one */
#ifndef CU_VFAT2_DIR
#define CU_VFAT2_DIR "uzedisk"
#endif


/* Which backend is serving the card */
static boole sdb_vfat2 =
#ifdef CU_VFAT2_ON
 TRUE;      /* Built to use the host directory backend unless told otherwise */
#else
 FALSE;     /* Legacy backend, as CUzeBox has always had it */
#endif

/* Card directory name, "--sdhost=<name>" overriding the built in default */
static char  sdb_dir[256] = CU_VFAT2_DIR;

/* What becomes of a host file the guest deleted (section 8.3). Trashing by
** default: this backend gives the guest a power the legacy one never had, and
** the card holds the user's own content. */
static auint sdb_del = CU_VFAT2_DEL_TRASH;

/* "--sdhost-rescan": rebuild the card when its directory changes on the host */
static boole sdb_rescan = FALSE;

/* Keep blocks with a bad CRC instead of refusing them */
static boole sdb_crcign = FALSE;

/* "--sdhost-log[=path]": the mutation log, to stderr if no path is given */
static boole sdb_log = FALSE;
static char  sdb_logpath[512] = {0};

/* An option configuring the host directory backend was given, so a run in
** which that backend is not in use can say it has no effect */
static boole sdb_opts = FALSE;



auint cu_sdb_parseargs(int argc, char** argv)
{
 int   i;
 int   o = 1;
 char* a;

 for (i = 1; i < argc; i++){

  a = argv[i];

  if (strcmp(a, "--no-sdhost") == 0){
   sdb_vfat2 = FALSE;
   continue;
  }

  if (strncmp(a, "--sdhost", 8U) == 0){
   if ((a[8] == 0) || (a[8] == '=')){
    sdb_vfat2 = TRUE;
    if (a[8] == '='){
     strncpy(&(sdb_dir[0]), &(a[9]), sizeof(sdb_dir) - 1U);
     sdb_dir[sizeof(sdb_dir) - 1U] = 0;
    }
    continue;
   }
  }

  if (strncmp(a, "--sdhost-log", 12U) == 0){
   if ((a[12] == 0) || (a[12] == '=')){
    sdb_log  = TRUE;
    sdb_opts = TRUE;
    sdb_logpath[0] = 0;
    if (a[12] == '='){
     strncpy(&(sdb_logpath[0]), &(a[13]), sizeof(sdb_logpath) - 1U);
     sdb_logpath[sizeof(sdb_logpath) - 1U] = 0;
    }
    continue;
   }
  }

  if (strcmp(a, "--sdhost-rescan") == 0){
   sdb_rescan = TRUE;
   sdb_opts   = TRUE;
   continue;
  }

  if (strncmp(a, "--sdhost-crc=", 13U) == 0){
   sdb_opts = TRUE;
   if      (strcmp(&(a[13]), "strict") == 0){ sdb_crcign = FALSE; }
   else if (strcmp(&(a[13]), "ignore") == 0){ sdb_crcign = TRUE;  }
   else{
    print_error("cu_sdhost: \"%s\" is not strict or ignore\n", &(a[13]));
   }
   continue;
  }

  if (strncmp(a, "--sdhost-delete=", 16U) == 0){
   sdb_opts = TRUE;
   if       (strcmp(&(a[16]), "trash")  == 0){ sdb_del = CU_VFAT2_DEL_TRASH;  }
   else if  (strcmp(&(a[16]), "unlink") == 0){ sdb_del = CU_VFAT2_DEL_UNLINK; }
   else if  (strcmp(&(a[16]), "off")    == 0){ sdb_del = CU_VFAT2_DEL_OFF;    }
   else{
    print_error("cu_sdhost: \"%s\" is not trash, unlink or off\n", &(a[16]));
   }
   continue;
  }

  argv[o] = a;
  o ++;

 }

 argv[o] = NULL;
 return (auint)(o);
}



void  cu_sdb_init(char const* game)
{
 if (sdb_vfat2){
  if (sdb_log){
   cu_vfat2hf_logopen((sdb_logpath[0] != 0) ? &(sdb_logpath[0]) : NULL);
  }
  cu_vfat2hf_setdelete(sdb_del);
  cu_vfat2_setcrc(sdb_crcign);
  cu_vfat2_setrescan(sdb_rescan);
  cu_vfat2_init(game, &(sdb_dir[0]));
 }else if (sdb_opts){
  print_error("cu_sdhost: --sdhost-log, --sdhost-crc, --sdhost-delete and --sdhost-rescan have no effect without --sdhost\n");
 }

 atexit(&cu_sdb_quit);
}



void  cu_sdb_setgame(char const* game)
{
 if (!sdb_vfat2){ return; }
 cu_vfat2_reset();          /* Settles what the guest left into the old directory */
 cu_vfat2_init(game, &(sdb_dir[0]));
}
void  cu_sdb_reset(void)
{
 if (sdb_vfat2){ cu_vfat2_reset(); }
 else          { cu_vfat_reset();  }
}



auint cu_sdb_read_begin(auint lba)
{
 if (sdb_vfat2){ return cu_vfat2_read_begin(lba); }

 (void)(lba);

 return 0xFEU;              /* Read data token: the data phase follows */
}



auint cu_sdb_read(auint pos)
{
 if (sdb_vfat2){ return cu_vfat2_read(pos); }

 return cu_vfat_read(pos);
}



void  cu_sdb_write(auint pos, auint data)
{
 if (sdb_vfat2){ cu_vfat2_write(pos, data); }
 else          { cu_vfat_write(pos, data);  }
}



auint cu_sdb_write_end(boole crcok)
{
 if (sdb_vfat2){ return cu_vfat2_write_end(crcok); }

 /* The legacy backend writes through as the bytes arrive, so by the time the
 ** CRC verdict is in there is nothing left to reject. Reporting it anyway is
 ** what cu_spisd.c always did. */

 if (!crcok){ return 0x0BU; } /* Data response token: rejected on CRC */

 return 0x05U;               /* Data response token: accepted */
}



void  cu_sdb_write_abort(void)
{
 if (sdb_vfat2){ cu_vfat2_write_abort(); }
}



void  cu_sdb_frametick(void)
{
 if (sdb_vfat2){ cu_vfat2_frametick(); }
}



void  cu_sdb_quit(void)
{
 /* main() flushes the filesystem channels on its way out, which is all the
 ** legacy backend needs. */

 if (sdb_vfat2){ cu_vfat2_quit(); }
}



#endif /* CU_VFAT2 */
