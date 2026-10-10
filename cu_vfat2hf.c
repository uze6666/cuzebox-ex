/*
 *  Host operation layer for the host-directory FAT16 backend
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



#include "cu_vfat2hf.h"

#ifdef CU_VFAT2

#include <dirent.h>
#include <sys/stat.h>

/* Section 7.6, the whole of the platform difference. Windows has its own
** spellings for two of the four operations. The third difference, rename()
** refusing an existing target on Windows where POSIX replaces it, is kept out
** of play by never renaming onto one (cu_vfat2hf_rename()). */

#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
#include <direct.h>
#include <io.h>
#define CU_VFAT2HF_MKDIR(p)      _mkdir(p)
#define CU_VFAT2HF_TRUNC(f, l)   _chsize_s(_fileno(f), (__int64)(l))
#define CU_VFAT2HF_TRUNC_ERR(r)  (r)     /* Returns the error code itself */
#else
#include <unistd.h>
#define CU_VFAT2HF_MKDIR(p)      mkdir((p), 0777)
#define CU_VFAT2HF_TRUNC(f, l)   ftruncate(fileno(f), (off_t)(l))
#define CU_VFAT2HF_TRUNC_ERR(r)  (errno) /* Returns -1 and sets errno */
#endif

#include <errno.h>
#include <stdarg.h>



/* Size of the card directory path. Matches filesys.c's PATH_SIZE */
#define CU_VFAT2HF_PATH_SIZE  1024U

/* Number of host file handles kept open (section 7.3) */
#define CU_VFAT2HF_HANDLES    8U

/* Bytes moved per read when copying one card file into another */
#define CU_VFAT2HF_COPY_CHUNK    0x8000U

/* Where deleted files go (section 8.3), below the card directory */
#define CU_VFAT2HF_TRASH      ".uzetrash"



/* Card directory, terminated with a separator. Everything the backend touches
** is below this, which is the whole of the containment argument (section 8.1):
** there is no protected name list because the emulator's own files are not in
** here to begin with. */
static char  vfat2hf_path[CU_VFAT2HF_PATH_SIZE] = {0};

/* Directory being enumerated, if any */
static DIR*  vfat2hf_dir = NULL;


/* One cached host file handle */
typedef struct{
 char  name[CU_VFAT2_HNAME_SIZE]; /* Bare file name, empty if the slot is free */
 FILE* fp;                        /* Open stream */
 auint pos;                       /* Where the stream is, to skip needless seeks */
 auint lru;                       /* Use stamp, largest is most recent */
 boole wr;                        /* Last access was a write */
}cu_vfat2hf_hnd_t;

/* Handle cache (section 7.3). The legacy backend caches exactly one file
** (vfat_lfile), so reads alternating between two files reopen on every
** sector. */
static cu_vfat2hf_hnd_t vfat2hf_hnd[CU_VFAT2HF_HANDLES];

/* Use stamp source for the cache's LRU */
static auint vfat2hf_lru = 0U;

/* What a delete does (section 8.3) */
static auint vfat2hf_del = CU_VFAT2_DEL_TRASH;

/* The mutation log (section 8.5), NULL if not logging */
static FILE* vfat2hf_logf = NULL;

/* Inside a reconcile's block of the log, whose lines are indented */
static boole vfat2hf_inrec = FALSE;

/* FAT-side reason for the host operations under way, empty if none */
static char  vfat2hf_whystr[256] = {0};

/* The card directory name has been found invalid once already. The name
** cannot change during a run, so saying it again at every reset adds nothing
** (section 8.4). */
static boole vfat2hf_rep_name = FALSE;

/* Last file the read path failed to open, so a sector's worth of failing reads
** gives one line rather than 512. Cleared whenever the handles are. */
static char  vfat2hf_rep_read[CU_VFAT2_HNAME_SIZE] = {0};



/*
** Writes one formatted line to the log, indented if inside a reconcile. When
** the log is stderr it carries the console prefix, since it is then sharing a
** stream with the console lines.
*/
static void cu_vfat2hf_logv(char const* fmt, va_list ap)
{
 if (vfat2hf_logf == NULL){ return; }

 if (vfat2hf_logf == stderr){ fputs("cu_sdhost: ", vfat2hf_logf); }
 if (vfat2hf_inrec){ fputs("  ", vfat2hf_logf); }

 (void)(vfprintf(vfat2hf_logf, fmt, ap));
 fputc('\n', vfat2hf_logf);
 fflush(vfat2hf_logf);
}



void  cu_vfat2hf_logopen(char const* path)
{
 cu_vfat2hf_logclose();

 if (path == NULL){
  vfat2hf_logf = stderr;
  return;
 }

 vfat2hf_logf = fopen(path, "w");
 if (vfat2hf_logf == NULL){
  print_error("cu_sdhost: Could not open log \"%s\": %s\n", path, strerror(errno));
 }
}



void  cu_vfat2hf_logclose(void)
{
 if ((vfat2hf_logf != NULL) && (vfat2hf_logf != stderr)){
  fclose(vfat2hf_logf);
 }

 vfat2hf_logf  = NULL;
 vfat2hf_inrec = FALSE;
}



void  cu_vfat2hf_log(char const* fmt, ...)
{
 va_list ap;

 va_start(ap, fmt);
 cu_vfat2hf_logv(fmt, ap);
 va_end(ap);
}



void  cu_vfat2hf_logrec(char const* fmt, ...)
{
 va_list ap;

 vfat2hf_inrec = FALSE;

 va_start(ap, fmt);
 cu_vfat2hf_logv(fmt, ap);
 va_end(ap);

 vfat2hf_inrec = TRUE;
}



void  cu_vfat2hf_logend(void)
{
 vfat2hf_inrec = FALSE;
}



void  cu_vfat2hf_report(boole console, char const* fmt, ...)
{
 va_list ap;
 char    str[1024];

 va_start(ap, fmt);
 (void)(vsnprintf(&(str[0]), sizeof(str), fmt, ap));
 va_end(ap);

 if ((console) && (vfat2hf_logf != stderr)){
  print_error("cu_sdhost: %s\n", &(str[0]));
 }

 cu_vfat2hf_log("%s", &(str[0]));
}



void  cu_vfat2hf_console(char const* fmt, ...)
{
 va_list ap;
 char    str[1024];

 va_start(ap, fmt);
 (void)(vsnprintf(&(str[0]), sizeof(str), fmt, ap));
 va_end(ap);

 if (vfat2hf_logf != stderr){
  print_error("cu_sdhost: %s\n", &(str[0]));
 }
}



void  cu_vfat2hf_why(char const* fmt, ...)
{
 va_list ap;

 va_start(ap, fmt);
 (void)(vsnprintf(&(vfat2hf_whystr[0]), sizeof(vfat2hf_whystr), fmt, ap));
 va_end(ap);
}



/*
** Reports a host operation that failed: every time, on the console and in the
** log, with the host's error text and the FAT-side reason (section 8.4).
** "estr" is the host's reason, normally strerror() of the errno captured at
** the failure. The line reads "Could not <verb> "<name>"<tail>: <estr>".
*/
static void cu_vfat2hf_fail(char const* estr, char const* verb, char const* name,
                            char const* tail)
{
 if (vfat2hf_whystr[0] != 0){
  cu_vfat2hf_report(TRUE, "Could not %s \"%s\"%s: %s (%s)",
                    verb, name, tail, estr, &(vfat2hf_whystr[0]));
 }else{
  cu_vfat2hf_report(TRUE, "Could not %s \"%s\"%s: %s",
                    verb, name, tail, estr);
 }
}



/*
** Builds the full path of a card file into dest. The name has no path
** component (enumeration and the 8.3 converter both guarantee it), so this is
** a concatenation, not a path resolution.
*/
static void cu_vfat2hf_fullpath(char* dest, auint len, char const* name)
{
 auint i = 0U;
 auint j = 0U;

 if (len == 0U){ return; }

 while ( (i < (len - 1U)) &&
         (vfat2hf_path[i] != 0) ){
  dest[i] = vfat2hf_path[i];
  i ++;
 }

 while ( (i < (len - 1U)) &&
         (name[j] != 0) ){
  dest[i] = name[j];
  i ++;
  j ++;
 }

 dest[i] = 0;
}



boole cu_vfat2hf_setup(char const* game, char const* name)
{
 auint       i = 0U;
 auint       j;
 struct stat st;

 /* Validate the card directory name first, before it is used to build any
 ** path at all (section 7.1). One component, always below the ROM. */

 /* Reported once per session and not logged (section 8.4): it is a mistake on
 ** the command line, not something the guest did, and it cannot change before
 ** the next reset asks again */

 if (name == NULL){ name = ""; }

 if (name[0] == 0){
  if (!vfat2hf_rep_name){ print_error("cu_sdhost: Card directory name is empty\n"); }
  vfat2hf_rep_name = TRUE;
  return FALSE;
 }
 if (name[0] == '-'){
  if (!vfat2hf_rep_name){ print_error("cu_sdhost: Card directory name \"%s\" looks like an option\n", name); }
  vfat2hf_rep_name = TRUE;
  return FALSE;
 }
 if ( (strcmp(name, ".")  == 0) ||
      (strcmp(name, "..") == 0) ){
  if (!vfat2hf_rep_name){ print_error("cu_sdhost: Card directory name \"%s\" is not a directory of its own\n", name); }
  vfat2hf_rep_name = TRUE;
  return FALSE;
 }
 for (j = 0U; name[j] != 0; j ++){
  if ( (name[j] == '/') ||
       (name[j] == '\\') ||
       (name[j] == ':') ){
   if (!vfat2hf_rep_name){ print_error("cu_sdhost: Card directory name \"%s\" is not a single path component\n", name); }
   vfat2hf_rep_name = TRUE;
   return FALSE;
  }
 }

 /* Derive the ROM's directory the way filesys_setpath() does, by walking back
 ** to the last separator. filesys_path is static to filesys.c, so this reads
 ** the original argument instead and has no ordering dependency on it. */

 while ( (i < (CU_VFAT2HF_PATH_SIZE - 1U)) &&
         (game[i] != 0) ){
  vfat2hf_path[i] = game[i];
  i ++;
 }
 vfat2hf_path[i] = 0;

 while (i != 0U){
  i --;
  if ( (vfat2hf_path[i] ==  '/') ||
       (vfat2hf_path[i] == '\\') ){
   i ++;
   break;
  }
 }
 vfat2hf_path[i] = 0;

 /* Append the card directory name and a separator */

 j = 0U;
 while ( (i < (CU_VFAT2HF_PATH_SIZE - 2U)) &&
         (name[j] != 0) ){
  vfat2hf_path[i] = name[j];
  i ++;
  j ++;
 }
 vfat2hf_path[i] = 0;

 /* Create it if it is not there, and make sure what is there is a directory.
 ** Anything else fails the card (section 5.5) rather than serving an empty
 ** volume, which a ROM cannot tell from a correctly set up card with nothing
 ** on it. */

 /* These fail the card, so they are said at every reset that finds the card
 ** directory still unusable: fixing it and resetting is the recovery, and each
 ** attempt deserves its answer */

 if (stat(&(vfat2hf_path[0]), &st) != 0){

  if (CU_VFAT2HF_MKDIR(&(vfat2hf_path[0])) != 0){
   cu_vfat2hf_report(TRUE, "Could not create card directory \"%s\": %s, no card",
                     &(vfat2hf_path[0]), strerror(errno));
   vfat2hf_path[i] = '/';
   vfat2hf_path[i + 1U] = 0;
   return FALSE;
  }

  print_message("cu_sdhost: Created card directory \"%s\"\n", &(vfat2hf_path[0]));
  cu_vfat2hf_log("mkdir   \"%s\"  (card directory did not exist)", &(vfat2hf_path[0]));

 }else if ((st.st_mode & S_IFMT) != S_IFDIR){

  cu_vfat2hf_report(TRUE, "Card directory \"%s\" exists, but is not a directory, no card",
                    &(vfat2hf_path[0]));
  vfat2hf_path[i] = '/';
  vfat2hf_path[i + 1U] = 0;
  return FALSE;

 }

 /* Keep the path separator terminated from here on, so building a file's path
 ** is a plain concatenation */

 vfat2hf_path[i] = '/';
 vfat2hf_path[i + 1U] = 0;

 /* It has to be enumerable, not merely present */

 vfat2hf_dir = opendir(&(vfat2hf_path[0]));
 if (vfat2hf_dir == NULL){
  cu_vfat2hf_report(TRUE, "Card directory \"%s\" cannot be read: %s, no card",
                    &(vfat2hf_path[0]), strerror(errno));
  return FALSE;
 }
 closedir(vfat2hf_dir);
 vfat2hf_dir = NULL;

 return TRUE;
}



auint cu_vfat2hf_signature(void)
{
 DIR*                 d;
 struct dirent const* ds;
 struct stat          st;
 char                 tstr[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 auint                h = 2166136261U;   /* FNV-1a */
 auint                v[2];
 auint                i;
 uint8 const*         p;

 d = opendir(&(vfat2hf_path[0]));
 if (d == NULL){ return 0U; }

 while ((ds = readdir(d)) != NULL){

  if (strlen(ds->d_name) >= CU_VFAT2_HNAME_SIZE){ continue; }
  cu_vfat2hf_fullpath(&(tstr[0]), sizeof(tstr), ds->d_name);
  if (stat(&(tstr[0]), &st) != 0){ continue; }
  if ((st.st_mode & S_IFMT) == S_IFDIR){ continue; }

  /* Order independent: each file's own hash is summed, since readdir() order
  ** is not guaranteed to be stable */
  v[0] = (auint)(st.st_size);
  v[1] = (auint)(st.st_mtime);
  i = 2166136261U;
  for (p = (uint8 const*)(ds->d_name); *p != 0U; p ++){ i = (i ^ *p) * 16777619U; }
  for (p = (uint8 const*)(&(v[0])); p < (uint8 const*)(&(v[2])); p ++){ i = (i ^ *p) * 16777619U; }
  h += i;

 }

 closedir(d);
 return h;
}



char const* cu_vfat2hf_path(void)
{
 return &(vfat2hf_path[0]);
}



void  cu_vfat2hf_find_reset(void)
{
 cu_vfat2hf_find_end();

 vfat2hf_dir = opendir(&(vfat2hf_path[0]));
}



auint cu_vfat2hf_find_next(char* dest, auint len)
{
 struct dirent const* ds;
 struct stat          st;
 char                 tstr[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 auint                i;

 if (len == 0U){ return 0xFFFFFFFFU; }
 if (vfat2hf_dir == NULL){ dest[0] = 0; return 0xFFFFFFFFU; }

 while (TRUE){

  ds = readdir(vfat2hf_dir);

  if (ds == NULL){
   closedir(vfat2hf_dir);
   vfat2hf_dir = NULL;
   dest[0] = 0;
   return 0xFFFFFFFFU;
  }

  for (i = 0U; i < len; i ++){
   dest[i] = ds->d_name[i];
   if (dest[i] == 0){ break; }
  }
  if (i >= len){ continue; }        /* Name too long for the destination */

  cu_vfat2hf_fullpath(&(tstr[0]), sizeof(tstr), &(dest[0]));

  if (stat(&(tstr[0]), &st) != 0){ continue; }

  /* Subdirectories are skipped, ".", ".." and ".uzetrash" with them. vfat2
  ** does not support them (section 2), and the trash is the backend's own
  ** and must not appear on the card it is the trash for. */

  if ((st.st_mode & S_IFMT) == S_IFDIR){ continue; }

  /* Cluster ordinals are counted in 32 bits, so anything at or above
  ** 4 GBytes cannot be addressed (section 4.3). st_size is signed and may be
  ** 32 or 64 bits wide depending on the target, so the high half is tested
  ** with two 16 bit shifts rather than one of 32, which would be undefined
  ** on the narrow one. */

  if (st.st_size < 0){ continue; }
  if (((st.st_size >> 16) >> 16) != 0){ continue; }
  if ((auint)(st.st_size) >= 0xFFFFFFFFU){ continue; }

  return (auint)(st.st_size);

 }
}



void  cu_vfat2hf_find_end(void)
{
 if (vfat2hf_dir != NULL){
  closedir(vfat2hf_dir);
  vfat2hf_dir = NULL;
 }
}



/*
** Returns a cached stream for a card file, opening it if it is not cached and
** evicting the least recently used handle to make room. NULL if it cannot be
** opened at all.
*/
static cu_vfat2hf_hnd_t* cu_vfat2hf_gethnd(char const* name)
{
 char  tstr[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 auint i;
 auint use = 0U;

 vfat2hf_lru ++;

 for (i = 0U; i < CU_VFAT2HF_HANDLES; i ++){
  if ( (vfat2hf_hnd[i].fp != NULL) &&
       (strcmp(&(vfat2hf_hnd[i].name[0]), name) == 0) ){
   vfat2hf_hnd[i].lru = vfat2hf_lru;
   return &(vfat2hf_hnd[i]);
  }
 }

 /* Not cached: take a free slot, or the least recently used one */

 for (i = 0U; i < CU_VFAT2HF_HANDLES; i ++){
  if (vfat2hf_hnd[i].fp == NULL){ use = i; break; }
  if (vfat2hf_hnd[i].lru < vfat2hf_hnd[use].lru){ use = i; }
 }

 if (vfat2hf_hnd[use].fp != NULL){
  fclose(vfat2hf_hnd[use].fp);
  vfat2hf_hnd[use].fp = NULL;
 }

 cu_vfat2hf_fullpath(&(tstr[0]), sizeof(tstr), name);

 /* Read and write, so reconcile can write back through the same handle. A
 ** read-only file still has to be readable, so fall back rather than treating
 ** it as absent. */

 vfat2hf_hnd[use].fp = fopen(&(tstr[0]), "r+b");
 if (vfat2hf_hnd[use].fp == NULL){
  vfat2hf_hnd[use].fp = fopen(&(tstr[0]), "rb");
 }
 if (vfat2hf_hnd[use].fp == NULL){ return NULL; }

 strncpy(&(vfat2hf_hnd[use].name[0]), name, CU_VFAT2_HNAME_SIZE - 1U);
 vfat2hf_hnd[use].name[CU_VFAT2_HNAME_SIZE - 1U] = 0;
 vfat2hf_hnd[use].pos = 0U;
 vfat2hf_hnd[use].lru = vfat2hf_lru;
 vfat2hf_hnd[use].wr  = FALSE;

 return &(vfat2hf_hnd[use]);
}



/*
** Puts a cached stream at a byte offset, ready for an access in the given
** direction. A stream opened for update needs a seek between a write and a
** read either way round, so a direction change forces one even when the
** position is already right.
*/
static boole cu_vfat2hf_seek(cu_vfat2hf_hnd_t* hnd, auint off, boole wr)
{
 if ((hnd->pos != off) || (hnd->wr != wr)){
  if (fseek(hnd->fp, (long)(off), SEEK_SET) != 0){ return FALSE; }
  hnd->pos = off;
 }

 hnd->wr = wr;

 return TRUE;
}



auint cu_vfat2hf_getb(char const* name, auint off)
{
 cu_vfat2hf_hnd_t* hnd;
 int               ch;

 hnd = cu_vfat2hf_gethnd(name);
 if (hnd == NULL){
  /* The guest reads zeros for it. Said once per file until the next reconcile
  ** rather than once per byte: a sector's worth of reads would be 512 lines. */
  if (strcmp(&(vfat2hf_rep_read[0]), name) != 0){
   cu_vfat2hf_report(TRUE, "Could not open \"%s\" to read it: %s (guest reads zeros)",
                     name, strerror(errno));
   strncpy(&(vfat2hf_rep_read[0]), name, CU_VFAT2_HNAME_SIZE - 1U);
   vfat2hf_rep_read[CU_VFAT2_HNAME_SIZE - 1U] = 0;
  }
  return 0xFFFFFFFFU;
 }

 /* Tracking the stream position turns a sector's worth of consecutive calls
 ** into plain fgetc()s against the stream's own buffer. The spec drops the
 ** sector buffer of the legacy backend on the grounds that the handle cache
 ** supplies the locality; this is the part of it that does. */

 if (!cu_vfat2hf_seek(hnd, off, FALSE)){ return 0xFFFFFFFFU; }

 ch = fgetc(hnd->fp);
 if (ch == EOF){ return 0xFFFFFFFFU; }

 hnd->pos ++;

 return (auint)(ch) & 0xFFU;
}



boole cu_vfat2hf_put(char const* name, auint off, uint8 const* src, auint len)
{
 cu_vfat2hf_hnd_t* hnd;
 char              what[64];
 int               err;

 hnd = cu_vfat2hf_gethnd(name);
 if (hnd == NULL){
  cu_vfat2hf_fail(strerror(errno), "open", name, " for writing");
  return FALSE;
 }

 if (!cu_vfat2hf_seek(hnd, off, TRUE)){
  err = errno;
  snprintf(&(what[0]), sizeof(what), " to offset %u", off);
  cu_vfat2hf_fail(strerror(err), "seek", name, &(what[0]));
  return FALSE;
 }

 if (fwrite(src, 1U, len, hnd->fp) != len){
  err = errno;
  snprintf(&(what[0]), sizeof(what), " at offset %u, %u bytes", off, len);
  cu_vfat2hf_fail(strerror(err), "write", name, &(what[0]));
  hnd->pos = 0xFFFFFFFFU;              /* Where it stopped is not known */
  return FALSE;
 }

 hnd->pos += len;

 return TRUE;
}



/*
** Pushes out anything a cached stream is still holding for a file, so a stat()
** of it reports the length it has now rather than the one it had before the
** last write went into the stream's buffer.
*/
static void cu_vfat2hf_flush(char const* name)
{
 auint i;

 for (i = 0U; i < CU_VFAT2HF_HANDLES; i ++){
  if (vfat2hf_hnd[i].fp == NULL){ continue; }
  if (strcmp(&(vfat2hf_hnd[i].name[0]), name) != 0){ continue; }
  (void)(fflush(vfat2hf_hnd[i].fp));
 }
}



auint cu_vfat2hf_size(char const* name)
{
 char        tstr[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 struct stat st;

 /* Anything cached for it may be holding bytes the stream has not passed on
 ** yet, which would make stat() report a length that was true a moment ago */

 cu_vfat2hf_flush(name);

 cu_vfat2hf_fullpath(&(tstr[0]), sizeof(tstr), name);

 if (stat(&(tstr[0]), &st) != 0){ return 0xFFFFFFFFU; }
 if ((st.st_mode & S_IFMT) == S_IFDIR){ return 0xFFFFFFFFU; }
 if (st.st_size < 0){ return 0xFFFFFFFFU; }
 if (((st.st_size >> 16) >> 16) != 0){ return 0xFFFFFFFFU; }

 return (auint)(st.st_size);
}



/*
** Whether a directory entry named exactly this exists. Deliberately asked of
** the directory listing rather than of the path: on Windows a path resolves
** through 8.3 aliases and so answers about a file that is not this one.
*/
static boole cu_vfat2hf_exact(char const* name)
{
 DIR*                 dp;
 struct dirent const* ds;
 boole                ret = FALSE;

 dp = opendir(&(vfat2hf_path[0]));
 if (dp == NULL){ return FALSE; }

 while (TRUE){
  ds = readdir(dp);
  if (ds == NULL){ break; }
  if (strcmp(&(ds->d_name[0]), name) == 0){ ret = TRUE; break; }
 }

 closedir(dp);

 return ret;
}



boole cu_vfat2hf_nameusable(char const* name)
{
 if (cu_vfat2hf_exact(name)){ return TRUE; }
 if (cu_vfat2hf_size(name) == 0xFFFFFFFFU){ return TRUE; }

 /* The path resolves, but to no directory entry of that name -- so it is a
 ** short name alias for some other file, and every operation on it would hit
 ** that file instead. Nothing here can make the name available: a short name
 ** is fixed when the file it belongs to is created. */

 cu_vfat2hf_fail("it is another file's short name on this system", "use the name", name, "");

 return FALSE;
}



boole cu_vfat2hf_create(char const* name)
{
 char  tstr[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 FILE* fp;

 if (!cu_vfat2hf_nameusable(name)){ return FALSE; }

 cu_vfat2hf_fullpath(&(tstr[0]), sizeof(tstr), name);

 /* Already there. Unless what is there is a directory: enumeration leaves
 ** those off the card, so the guest may well create a file of that name, and
 ** taking the directory for it would only move the failure to every write. */

 if (cu_vfat2hf_exact(name)){
  if (cu_vfat2hf_size(name) != 0xFFFFFFFFU){ return TRUE; }
  cu_vfat2hf_fail("a directory of that name is in the card folder", "create", name, "");
  return FALSE;
 }

 fp = fopen(&(tstr[0]), "wb");
 if (fp == NULL){
  cu_vfat2hf_fail(strerror(errno), "create", name, "");
  return FALSE;
 }
 fclose(fp);

 return TRUE;
}



boole cu_vfat2hf_copy(char const* from, char const* to, auint len)
{
 static uint8      cbuf[CU_VFAT2HF_COPY_CHUNK];
 cu_vfat2hf_hnd_t* hnd;
 auint             off = 0U;
 size_t            got;
 size_t            want;

 while (off < len){

  want = ((len - off) < sizeof(cbuf)) ? (len - off) : sizeof(cbuf);

  hnd = cu_vfat2hf_gethnd(from);
  if (hnd == NULL){
   cu_vfat2hf_fail(strerror(errno), "open", from, " to copy it");
   return FALSE;
  }
  if (!cu_vfat2hf_seek(hnd, off, FALSE)){
   cu_vfat2hf_fail(strerror(errno), "seek", from, " to copy it");
   return FALSE;
  }

  got = fread(&(cbuf[0]), 1U, want, hnd->fp);
  hnd->pos += (auint)(got);

  if (got == 0U){
   if (ferror(hnd->fp)){
    cu_vfat2hf_fail(strerror(errno), "read", from, " to copy it");
    return FALSE;
   }
   break;                              /* End of the source */
  }

  if (!cu_vfat2hf_put(to, off, &(cbuf[0]), (auint)(got))){ return FALSE; }

  off += (auint)(got);

 }

 return TRUE;
}



boole cu_vfat2hf_samename(char const* a, char const* b)
{
#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 return (_stricmp(a, b) == 0);
#else
 return (strcmp(a, b) == 0);
#endif
}



boole cu_vfat2hf_truncate(char const* name, auint len)
{
 cu_vfat2hf_hnd_t* hnd;
 char              what[64];
 int               err;

 hnd = cu_vfat2hf_gethnd(name);
 if (hnd == NULL){
  cu_vfat2hf_fail(strerror(errno), "open", name, " for resizing");
  return FALSE;
 }

 /* The stream position is undefined across a truncate, and anything it is
 ** still buffering would land after it, so flush first and forget where the
 ** stream was afterwards (section 7.6) */

 if (fflush(hnd->fp) != 0){
  cu_vfat2hf_fail(strerror(errno), "flush", name, "");
  return FALSE;
 }

 /* _chsize_s() returns its error code rather than setting errno */

 err = CU_VFAT2HF_TRUNC(hnd->fp, len);
 if (err != 0){
  err = CU_VFAT2HF_TRUNC_ERR(err);
  snprintf(&(what[0]), sizeof(what), " to %u bytes", len);
  cu_vfat2hf_fail(strerror(err), "resize", name, &(what[0]));
  return FALSE;
 }

 hnd->pos = 0xFFFFFFFFU;
 (void)(fseek(hnd->fp, 0L, SEEK_SET));
 hnd->pos = 0U;
 hnd->wr  = FALSE;

 return TRUE;
}



boole cu_vfat2hf_rename(char const* from, char const* to)
{
 char tfrom[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 char tto  [CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];

 if (!cu_vfat2hf_nameusable(to)){ return FALSE; }

 cu_vfat2hf_fullpath(&(tfrom[0]), sizeof(tfrom), from);
 cu_vfat2hf_fullpath(&(tto  [0]), sizeof(tto  ), to);

 /* A file already holding the new name. The reconcile has moved every file on
 ** the card out of the way by now, so this one is not on the card: put there
 ** on the host after the card was built. The guest's directory says the name
 ** is free, and it gets it, but the file goes through the delete policy first
 ** rather than being replaced in silence, which is what rename() does on
 ** POSIX and what removing it first would do on Windows. */

 if (cu_vfat2hf_exact(to)){
  char where[CU_VFAT2_HNAME_SIZE + 32U];
  char what [CU_VFAT2_HNAME_SIZE + 32U];
  snprintf(&(what[0]), sizeof(what), " to \"%s\"", to);
  if (cu_vfat2hf_size(to) == 0xFFFFFFFFU){
   cu_vfat2hf_fail("a directory of that name is in the card folder", "rename", from, &(what[0]));
   return FALSE;
  }
  if (vfat2hf_del == CU_VFAT2_DEL_OFF){
   cu_vfat2hf_fail("a file of that name is on the host, not on the card, and --sdhost-delete=off keeps it",
                   "rename", from, &(what[0]));
   return FALSE;
  }
  if (!cu_vfat2hf_delete(to, &(where[0]), sizeof(where))){ return FALSE; }
  cu_vfat2hf_report(TRUE, "\"%s\" was on the host but not on the card, %s to free the name for a rename",
                    to, (vfat2hf_del == CU_VFAT2_DEL_TRASH) ? "moved to the trash" : "unlinked");
  cu_vfat2hf_log("delete  \"%s\" -> %s  (in the way of a rename)", to, &(where[0]));
 }

 cu_vfat2hf_close(from);
 cu_vfat2hf_close(to);

 /* No target is in the way by now, which is what makes rename() mean the same
 ** on both: Windows refuses an existing target where POSIX replaces it */

 if (rename(&(tfrom[0]), &(tto[0])) != 0){
  char what[CU_VFAT2_HNAME_SIZE + 32U];
  int  err = errno;
  snprintf(&(what[0]), sizeof(what), " to \"%s\"", to);
  cu_vfat2hf_fail(strerror(err), "rename", from, &(what[0]));
  return FALSE;
 }

 return TRUE;
}



void  cu_vfat2hf_setdelete(auint mode)
{
 vfat2hf_del = mode;
}



/*
** Builds the path of a file in the trash, with an "n" suffix if n is nonzero,
** so a second file of the same name does not overwrite the first.
*/
static void cu_vfat2hf_trashpath(char* dest, auint len, char const* name, auint n)
{
 char sub[CU_VFAT2_HNAME_SIZE + 32U];

 if (n == 0U){
  snprintf(&(sub[0]), sizeof(sub), "%s/%s", CU_VFAT2HF_TRASH, name);
 }else{
  snprintf(&(sub[0]), sizeof(sub), "%s/%s.%u", CU_VFAT2HF_TRASH, name, n);
 }

 cu_vfat2hf_fullpath(dest, len, &(sub[0]));
}



boole cu_vfat2hf_delete(char const* name, char* where, auint wlen)
{
 char        tfrom[CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE];
 char        tto  [CU_VFAT2HF_PATH_SIZE + CU_VFAT2_HNAME_SIZE + 32U];
 char        sub  [CU_VFAT2_HNAME_SIZE + 32U];
 struct stat st;
 auint       n;

 if (vfat2hf_del == CU_VFAT2_DEL_OFF){
  print_message("cu_sdhost: \"%s\" deleted by the guest, kept on the host\n", name);
  snprintf(where, wlen, "kept (--sdhost-delete=off)");
  return TRUE;
 }

 cu_vfat2hf_close(name);
 cu_vfat2hf_fullpath(&(tfrom[0]), sizeof(tfrom), name);

 if (vfat2hf_del == CU_VFAT2_DEL_UNLINK){
  if (remove(&(tfrom[0])) != 0){
   cu_vfat2hf_fail(strerror(errno), "delete", name, "");
   return FALSE;
  }
  snprintf(where, wlen, "unlinked");
  return TRUE;
 }

 /* Trash it (section 8.3). The subdirectory is invisible to the card because
 ** enumeration skips directories, so the guest cannot see, read or undelete
 ** what it threw away -- this is for the user, not for the ROM. */

 cu_vfat2hf_fullpath(&(tto[0]), sizeof(tto), CU_VFAT2HF_TRASH);

 if (stat(&(tto[0]), &st) != 0){
  if (CU_VFAT2HF_MKDIR(&(tto[0])) != 0){
   snprintf(&(sub[0]), sizeof(sub), "%s: could not create " CU_VFAT2HF_TRASH,
            strerror(errno));
   cu_vfat2hf_fail(&(sub[0]), "delete", name, "");
   return FALSE;
  }
 }

 for (n = 0U; n < 1000U; n ++){
  cu_vfat2hf_trashpath(&(tto[0]), sizeof(tto), name, n);
  if (stat(&(tto[0]), &st) != 0){ break; }
 }

 if (n == 1000U){
  cu_vfat2hf_fail("the trash already holds 1000 of them", "delete", name, "");
  return FALSE;
 }

 if (rename(&(tfrom[0]), &(tto[0])) != 0){
  cu_vfat2hf_fail(strerror(errno), "move", name, " to the trash");
  return FALSE;
 }

 if (n == 0U){ snprintf(where, wlen, CU_VFAT2HF_TRASH "/%s", name); }
 else        { snprintf(where, wlen, CU_VFAT2HF_TRASH "/%s.%u", name, n); }

 return TRUE;
}



boole cu_vfat2hf_namematch(char const* host, uint8 const* name)
{
 uint8 derived[11];

 cu_vfat2hf_tofat(host, &(derived[0]));

 return (memcmp(&(derived[0]), name, 11U) == 0);
}



void  cu_vfat2hf_close(char const* name)
{
 auint i;

 for (i = 0U; i < CU_VFAT2HF_HANDLES; i ++){
  if (vfat2hf_hnd[i].fp == NULL){ continue; }
  if (strcmp(&(vfat2hf_hnd[i].name[0]), name) != 0){ continue; }
  fclose(vfat2hf_hnd[i].fp);
  vfat2hf_hnd[i].fp = NULL;
  vfat2hf_hnd[i].name[0] = 0;
  vfat2hf_hnd[i].pos = 0U;
  vfat2hf_hnd[i].lru = 0U;
  vfat2hf_hnd[i].wr  = FALSE;
 }
}



void  cu_vfat2hf_closeall(void)
{
 auint i;

 for (i = 0U; i < CU_VFAT2HF_HANDLES; i ++){
  if (vfat2hf_hnd[i].fp != NULL){
   fclose(vfat2hf_hnd[i].fp);
   vfat2hf_hnd[i].fp = NULL;
  }
  vfat2hf_hnd[i].name[0] = 0;
  vfat2hf_hnd[i].pos = 0U;
  vfat2hf_hnd[i].lru = 0U;
  vfat2hf_hnd[i].wr  = FALSE;
 }

 vfat2hf_rep_read[0] = 0;              /* A read failure may be said again */
}



void  cu_vfat2hf_tofat(char const* src, uint8* dest)
{
 auint spos = 0U;
 auint dpos = 0U;

 memset(dest, ' ', 11U);

 while (src[spos] != 0){

  if       ((src[spos] >= 'a') && (src[spos] <= 'z')){
   dest[dpos] = (uint8)((src[spos] - 'a') + 'A');
  }else if ((src[spos] >= 'A') && (src[spos] <= 'Z')){
   dest[dpos] = (uint8)(src[spos]);
  }else if ((src[spos] >= '0') && (src[spos] <= '9')){
   dest[dpos] = (uint8)(src[spos]);
  }else if ( (src[spos] == '~') ||
             (src[spos] == '!') ||
             (src[spos] == '-') ||
             (src[spos] == '_') ){
   dest[dpos] = (uint8)(src[spos]);
  }else if ( (src[spos] == '.') &&
             (dpos < 8U) ){
   dpos = 7U; /* Go on to extension */
   spos --;   /* Bounce back to stay at the extension dot */
  }else{
   dest[dpos] = (uint8)('$');
  }

  spos ++;
  dpos ++;

  if (dpos == 8U){
   while ( (src[spos] != 0) &&
           (src[spos] != '.') ){
    spos ++;  /* Long name */
   }
   if (src[spos] == '.'){ spos ++; } /* Jump over extension dot */
  }else if (dpos == 11U){
   while (src[spos] != 0){
    spos ++;  /* Long extension */
   }
  }

 }
}



/*
** Converts one FAT 8.3 name character to a host name character. Anything
** outside the whitelist becomes '$', so no path separator, colon or dot can
** come out of a directory entry.
**
** A letter keeps the case it has in the directory entry, so a file the guest
** creates lands on the host as the guest's driver named it: FAT stores short
** names in upper case, and a guest that saves "hello" gets HELLO.BAS, not
** hello.bas. (It was folded to lower case until 2026-09-23.) Only names derived
** for NEW files come through here -- a file already on the host keeps its own
** name (section 7.5), and the reverse direction, cu_vfat2hf_tofat(), folds to
** upper case, so which case a host name has never changes which FAT name it is.
*/
static char cu_vfat2hf_tohost_ch(uint8 src)
{
 if ((src >= 'a') && (src <= 'z')){ return (char)(src); }
 if ((src >= 'A') && (src <= 'Z')){ return (char)(src); }
 if ((src >= '0') && (src <= '9')){ return (char)(src); }
 if ( (src == '~') ||
      (src == '!') ||
      (src == '-') ||
      (src == '_') ){ return (char)(src); }
 return '$';
}



void  cu_vfat2hf_tohost(uint8 const* src, char* dest, auint len)
{
 auint spos;
 auint dpos = 0U;
 auint slen;

 if (len == 0U){ return; }

 slen = 8U;                            /* Name: trim trailing spaces */
 while ((slen != 0U) && (src[slen - 1U] == ' ')){ slen --; }
 for (spos = 0U; spos < slen; spos ++){
  if (dpos >= (len - 1U)){ break; }
  dest[dpos] = cu_vfat2hf_tohost_ch(src[spos]);
  dpos ++;
 }

 slen = 11U;                           /* Extension: likewise */
 while ((slen != 8U) && (src[slen - 1U] == ' ')){ slen --; }
 if (slen != 8U){                      /* A blank extension gets no dot */
  if (dpos < (len - 1U)){
   dest[dpos] = '.';
   dpos ++;
  }
  for (spos = 8U; spos < slen; spos ++){
   if (dpos >= (len - 1U)){ break; }
   dest[dpos] = cu_vfat2hf_tohost_ch(src[spos]);
   dpos ++;
  }
 }

 dest[dpos] = 0;
}



boole cu_vfat2hf_mangle(uint8* dest, auint n)
{
 uint8 tail[8];
 auint tlen = 0U;
 auint nlen;
 auint keep;
 auint i;

 /* Build the "~n" tail back to front */

 if (n == 0U){ return FALSE; }
 while ((n != 0U) && (tlen < (sizeof(tail) - 1U))){
  tail[tlen] = (uint8)('0' + (n % 10U));
  tlen ++;
  n /= 10U;
 }
 if (n != 0U){ return FALSE; }
 tail[tlen] = (uint8)('~');
 tlen ++;

 if (tlen >= 8U){ return FALSE; }      /* No room for any of the name itself */

 /* Keep as much of the name as the tail leaves */

 nlen = 8U;
 while ((nlen != 0U) && (dest[nlen - 1U] == ' ')){ nlen --; }
 keep = 8U - tlen;
 if (nlen < keep){ keep = nlen; }

 for (i = 0U; i < tlen; i ++){
  dest[keep + i] = tail[tlen - 1U - i];
 }
 for (i = keep + tlen; i < 8U; i ++){
  dest[i] = ' ';
 }

 return TRUE;
}



#endif /* CU_VFAT2 */
