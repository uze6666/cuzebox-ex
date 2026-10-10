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


/*
** Everything vfat2 does to the host filesystem happens here. See
** doc/spec-sd-enhanced-vfat.md section 7.
**
** This does not extend filesys.c, which has no delete, truncate, rename or
** mkdir primitive, whose single SD channel can hold one file open at a time,
** and whose enumeration is rooted at the game's directory rather than at the
** card subdirectory. filesys.c is not modified.
*/


#ifndef CU_VFAT2HF_H
#define CU_VFAT2HF_H


#include "types.h"


/* Size of a host file name (no path component). Generous against the 64 of
** the legacy backend, so enumeration rejects far fewer names. */
#define CU_VFAT2_HNAME_SIZE  256U


/* What becomes of a host file the guest deleted (section 8.3). Trashing is
** the default because vfat2 gives the guest a power the legacy backend never
** had, and the card holds the user's own content. */
#define CU_VFAT2_DEL_TRASH   0U    /* Move it to .uzetrash/ inside the card */
#define CU_VFAT2_DEL_UNLINK  1U    /* Really remove it                      */
#define CU_VFAT2_DEL_OFF     2U    /* Leave it alone and say so             */


/* Lets the compiler check the reporting calls' format strings against their
** arguments, which matters for messages that are mostly seen when something
** has already gone wrong */
#if defined(__GNUC__)
#define CU_VFAT2HF_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define CU_VFAT2HF_PRINTF(f, a)
#endif


/*
** Starts the mutation log of section 8.5, to the given file, or to stderr if
** "path" is NULL. Without this call nothing is logged. A file that cannot be
** opened is reported and the run continues without a log.
**
** Under FLAG_NOCONSOLE and headless builds print_error() compiles to nothing,
** and the log is the only place any of this reaches. It is written with
** fprintf() directly for that reason.
*/
void  cu_vfat2hf_logopen(char const* path);


/*
** Closes the log, if one is open.
*/
void  cu_vfat2hf_logclose(void);


/*
** Writes one line to the log only: a host mutation, or a detail the console
** gives in summary. Lines between cu_vfat2hf_logrec() and cu_vfat2hf_logend()
** are indented under that reconcile's heading. No trailing newline.
*/
void  cu_vfat2hf_log(char const* fmt, ...) CU_VFAT2HF_PRINTF(1, 2);


/*
** Heads the log's lines for one reconcile, and ends that reconcile's block.
*/
void  cu_vfat2hf_logrec(char const* fmt, ...) CU_VFAT2HF_PRINTF(1, 2);
void  cu_vfat2hf_logend(void);


/*
** Reports a failure or a refusal (section 8.4): on the console with the
** "cu_sdhost: " prefix if "console" is set, and in the log always. The caller
** decides "console", which is where the rate limits live. When the log itself
** is stderr the console line is left out, so nothing appears twice. No
** trailing newline.
*/
void  cu_vfat2hf_report(boole console, char const* fmt, ...) CU_VFAT2HF_PRINTF(2, 3);


/*
** A console line only, for a summary whose detail the caller logs line by
** line itself. Left out when the log is stderr, which already carries the
** detail. No trailing newline.
*/
void  cu_vfat2hf_console(char const* fmt, ...) CU_VFAT2HF_PRINTF(1, 2);


/*
** Sets the FAT-side reason for the host operations that follow, such as
** "guest deleted OLD.TMP from slot 3". A host operation failing names it
** beside the host's own error text, since "Permission denied" alone does not
** say what the guest had asked for. An empty string clears it.
*/
void  cu_vfat2hf_why(char const* fmt, ...) CU_VFAT2HF_PRINTF(1, 2);


/*
** Derives the card directory from the path the emulator was given and the
** card directory name, validates the name, and creates the directory if it
** does not exist (section 7.1). Returns FALSE if no card can be served, which
** the caller turns into the failed card state of section 5.5 rather than
** serving an empty volume. A message naming the path and the reason is
** printed on every failure.
**
** "name" must be a single path component: empty, ".", "..", anything holding
** '/', '\' or ':', and anything starting with '-' are refused. That is what
** keeps the emulator's own files -- the ROM, eeprom.bin, esp.cfg, all of them
** in the parent directory -- off the card by construction.
*/
boole cu_vfat2hf_setup(char const* game, char const* name);


/*
** The card directory's path, terminated with a separator, for messages. Valid
** after cu_vfat2hf_setup() whether or not it succeeded.
*/
char const* cu_vfat2hf_path(void);


/*
** Starts enumerating the card directory. Mirrors filesys_find_reset() but
** rooted one level deeper.
*/
void  cu_vfat2hf_find_reset(void);


/*
** Returns the next file's size, with its bare name in dest, or 0xFFFFFFFF at
** the end of the directory. Skips subdirectories (".uzetrash" among them),
** files at or above 4 GBytes, and names too long for dest.
*/
auint cu_vfat2hf_find_next(char* dest, auint len);


/*
** Ends an enumeration early. Not needed if find_next() ran to the end.
*/
void  cu_vfat2hf_find_end(void);


/*
** A hash of the card directory's listing: every file's name, size and
** modification time. Two equal values mean nothing was added, removed, resized
** or rewritten on the host in between (to the second: two saves of the same
** length within one second of each other look the same). Uses its own
** enumeration, so it does not disturb find_reset()/find_next().
*/
auint cu_vfat2hf_signature(void);


/*
** Reads one byte from a card file at a byte offset, returning it, or
** 0xFFFFFFFF if the file cannot be opened or the offset is past its end. The
** handle cache (section 7.3) keeps this cheap across a sector's worth of
** consecutive calls.
*/
auint cu_vfat2hf_getb(char const* name, auint off);


/*
** Writes "len" bytes into a card file at a byte offset, extending it if the
** offset is past its end. FALSE if the file cannot be opened or written, in
** which case the caller leaves its C entry alone so the next reconcile
** retries.
*/
boole cu_vfat2hf_put(char const* name, auint off, uint8 const* src, auint len);


/*
** The size of a card file, or 0xFFFFFFFF if it is not there. This is how the
** resize rule of section 6.6 learns the host length it must not shorten
** without having seen the guest shorten it first.
*/
auint cu_vfat2hf_size(char const* name);


/*
** Whether a name derived from a directory entry can be used as a host file
** name at all, which is not the same question as whether the path is free.
**
** Windows keeps an 8.3 alias for every long file name and resolves paths
** through it, so "mylong~1.txt" opens, renames and deletes
** "MyLongReport.txt" whenever that file happens to own that short name. Since
** the "~n" tail is exactly what the dedup of section 7.4 emits, a derived name
** can silently address a completely unrelated file. That is refused here
** rather than acted on: either a directory entry with exactly this name
** exists, or nothing of that name resolves at all.
*/
boole cu_vfat2hf_nameusable(char const* name);


/*
** Creates an empty card file, or leaves an existing one alone. FALSE if it
** could not be created.
*/
boole cu_vfat2hf_create(char const* name);


/*
** Copies up to "len" bytes from the start of card file "from" into card file
** "to" at the same offsets, stopping early at the end of "from". "to" must
** exist. FALSE, reported with the FAT-side reason, if either cannot be read or
** written.
**
** This is how a file the guest created on a cluster it had just freed by a
** delete gets the old file's bytes, without the old host file ever being
** renamed or written to (section 6.4).
*/
boole cu_vfat2hf_copy(char const* from, char const* to, auint len);


/*
** Whether two card file names name the same host file, as far as the host's
** file system is concerned: ignoring case on Windows, exactly elsewhere.
*/
boole cu_vfat2hf_samename(char const* a, char const* b);


/*
** Sets a card file's length, zero-extending or truncating to reach it
** (section 7.6). This is the one genuinely destructive operation vfat2
** performs by default -- a truncation cannot be trashed, so unlike a delete
** it has no safety net, and section 6.6 is written the way it is because of
** that.
*/
boole cu_vfat2hf_truncate(char const* name, auint len);


/*
** Renames a card file, replacing the target if one exists. Both names are
** bare, and no rename can leave the card directory because neither can hold a
** path separator.
*/
boole cu_vfat2hf_rename(char const* from, char const* to);


/*
** Removes a card file under the policy of section 8.3: to ".uzetrash/" inside
** the card directory by default, with ".1", ".2" on collision. The trash is
** invisible to the card because enumeration skips directories. On success
** "where" says what became of it, for the log: the trash path, "unlinked", or
** that the policy kept it.
*/
boole cu_vfat2hf_delete(char const* name, char* where, auint wlen);


/*
** Chooses that policy, one of CU_VFAT2_DEL_*.
*/
void  cu_vfat2hf_setdelete(auint mode);


/*
** Whether the FAT 8.3 name a host file derives is the one the guest now has
** for it (section 7.5). Asked before every rename: without it, "Hello.bas"
** would be flattened to "HELLO.BAS" the first time the guest's driver rewrote
** an unrelated field of its directory entry.
*/
boole cu_vfat2hf_namematch(char const* host, uint8 const* name);


/*
** Closes one cached handle, by name. Windows will not let remove() or
** rename() touch a file any process holds open, so every mutation goes
** through this first.
*/
void  cu_vfat2hf_close(char const* name);


/*
** Closes every cached handle. Needed at the end of a reconcile on Windows,
** where an open handle blocks remove() and rename() from any process, and at
** teardown.
*/
void  cu_vfat2hf_closeall(void);


/*
** Converts a host file name to the 11 byte FAT 8.3 form. Follows
** cu_vfat_tofat(): the character whitelist is a-z (upcased), A-Z, 0-9 and
** "~!-_", everything else becoming '$'.
*/
void  cu_vfat2hf_tofat(char const* src, uint8* dest);


/*
** Converts an 11 byte FAT 8.3 name to a host file name, bounded by len
** including the terminator.
**
** The name and extension are taken as the fixed 8 and 3 byte fields they are,
** each trimmed of trailing spaces, so a blank extension yields no dot and
** nothing here reads past src[10]. The whitelist is cu_vfat_tohost()'s, so no
** path separator, colon or ".." can be produced from a directory entry -- the
** other half of why section 8.1 is structural.
*/
void  cu_vfat2hf_tohost(uint8 const* src, char* dest, auint len);


/*
** Stamps a "~n" tail into the name field of an 11 byte FAT 8.3 name, so two
** host files whose names converge on one 8.3 name can still be told apart
** (section 7.4). "n" runs from 1; the tail is placed to leave as much of the
** original name as will fit. Returns FALSE if n cannot be represented, which
** bounds the caller's search.
*/
boole cu_vfat2hf_mangle(uint8* dest, auint n);


#endif
