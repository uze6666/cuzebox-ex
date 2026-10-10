/*
 *  GDB Remote Serial Protocol stub
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



#ifndef CU_GDB_H
#define CU_GDB_H



#include "types.h"



#ifdef CU_GDB



/* Default TCP port to listen on */
#define CU_GDB_PORT 1234



/*
** Consumes the stub's own command line options from argv, compacting the
** array, and returns the new argument count. Recognised:
**
**   --gdb[=port]   Enable the stub (default port 1234)
**   --gdb-wait     Halt at reset until a client attaches
**   --gdb-reverse  Advertise and allow bs/bc (reverse execution). Left off
**                  by default: the Microchip AVR toolchain's avr-gdb 7.8
**                  hits an internal error the moment it sees reverse
**                  support advertised and then tries to step. See
**                  doc/notes-gdbstub.md.
**   --gdb-log[=f]  Log the packet traffic (default cuzebox-gdb.log)
**   --gdb-focus-ide
**                  Bring the window which was in front at startup (the IDE
**                  which launched the emulator) back to the foreground when a
**                  breakpoint, watchpoint or break-on condition halts the CPU.
**                  Windows only, and off by default so that scripted RSP
**                  sessions never move the user's focus.
*/
auint cu_gdb_parseargs(int argc, char** argv);


/*
** Records which window is in front, for --gdb-focus-ide to return to. Must be
** called after cu_gdb_parseargs() and before the emulator's own window is
** created, or it captures that window instead.
*/
void  cu_gdb_focus_capture(void);


/*
** Whether --gdb-nolimit asked for the frame limiter to be off from the start.
** "monitor speed unlimited" does the same to a running emulator, but it has to
** re-create the window to drop vsync, and that costs about a second of wall
** time: a scripted run that knows it wants full speed asks before the window
** exists. Must be called after cu_gdb_parseargs().
*/
boole cu_gdb_startnolimit(void);
boole cu_gdb_starthidden(void);


/*
** Brings the stub up if it was asked for on the command line. Must be called
** after the emulator has been reset.
*/
void  cu_gdb_init(void);


/*
** Tears the stub down.
*/
void  cu_gdb_quit(void);


/*
** Services the GDB connection, and holds here for as long as the CPU is
** halted by the debugger, keeping the window and the audio alive. Returns
** TRUE if the user asked to close the emulator while it was held.
**
** This is the stub's only hook into the main loop.
*/
boole cu_gdb_hold(void);


/*
** Returns whether the CPU is currently halted by the debugger.
*/
boole cu_gdb_ishalted(void);


/*
** Living beside the emulator's own debugger (ENABLE_DEBUGGER: the debugger
** window, the API and the web debugger). Both stop the CPU by having
** cu_avr_run() return CU_BREAK, so each stop has to find its owner, and a
** stop by one has to be a stop for the other.
**
** cu_gdb_ownsbreak()      Whether the CU_BREAK just returned is the stub's
**                         (frame.c: then the other debugger is not told it
**                         hit a breakpoint).
** cu_gdb_foreignstop()    The other debugger stopped the CPU (its breakpoint,
**                         watchpoint or step). An attached client is told the
**                         target halted, as on a Ctrl-C, and can inspect it.
** cu_gdb_foreignresumed() TRUE once, when the client resumed from such a
**                         stop: main.c then lets the other debugger's pause go.
** cu_gdb_setidle()        A function the stub calls every pass of its hold
**                         loop, which main_loop() does not run during: what
**                         keeps the API and web servers answering while a
**                         client holds the CPU.
*/
boole cu_gdb_ownsbreak(void);
void  cu_gdb_foreignstop(void);
boole cu_gdb_foreignresumed(void);
void  cu_gdb_setidle(void (*hook)(void));

/*
** Returns whether the GDB stub was requested on the command line (--gdb),
** independent of whether a client has attached or the CPU is halted.
*/
boole cu_gdb_isenabled(void);


/*
** Sends a line of text to the client's console (as an RSP "O" packet). Used
** by the monitor commands.
*/
void  cu_gdb_out(char const* str);


/*
** Power-cycle reset of the emulated machine, plus everything the stub keeps
** alongside it: SRAM poisoning, the reverse execution history, the widened
** cycle counter and any injected input still queued.
**
** Shared by the RSP "R" packet and "monitor reset" so the two cannot drift
** apart -- they had, with monitor reset leaving the cycle counter and the
** reverse history describing the pre-reset machine.
*/
void  cu_gdb_reset_target(void);


/*
** Writes a snapshot of the emulator state to a file, or reads one back.
** Returns TRUE on success, and on failure fills "err" with a reason.
*/
boole cu_gdb_state_save(char const* path, char* err, auint errsz);
boole cu_gdb_state_load(char const* path, char* err, auint errsz);


/*
** A snapshot of the emulator state in memory (CPU, I/O, video timing, SPI
** RAM), opaque to the caller: what the reverse ring and cu_rewind.c keep.
*/
void* cu_gdb_snap_new(void);
boole cu_gdb_snap_take(void* s);
void  cu_gdb_snap_put(void const* s);
void  cu_gdb_snap_delete(void* s);


/*
** Reverse execution ring buffer depth, in frames.
*/
void  cu_gdb_reverse_setdepth(auint frames);
auint cu_gdb_reverse_getdepth(void);


/*
** Total cycles emulated since reset, not wrapping at 32 bits, and the
** derived frame count. Used by the status and cycles monitor commands.
*/
uint64_t cu_gdb_getcycle64(void);
uint64_t cu_gdb_getframe(void);


/*
** Cycles emulated since the CPU was last halted.
*/
uint64_t cu_gdb_getsincestop(void);


/*
** Cycles in a full frame: 262 scanlines of 1820 cycles = 476840.
**
** Not 1920. VIDEO_CY_MAX (cu_avr.c) is 1920, but that is the ceiling at which
** a scanline is declared a sync error, not its length: cu_avr_run() ends a row
** when the ROM's own sync pulse forces video_cycle to VIDEO_CY_MAX, which for
** a correctly syncing kernel happens at 1820, the real Uzebox scanline.
**
** Measured three ways on BUGZ.UZE rather than reasoned about: F10 (advance one
** frame) moves the cycle counter by exactly 476840, "monitor frames 30" by
** exactly 30 times that, and the free running rate divides by it to exactly
** 65.0 and 59.0 frames per second on two different displays. 503040 divides
** none of them evenly.
**
** It is a ROM dependent number, so the frame count derived from it is exact
** only for a ROM that syncs properly. One with no video kernel at all -- the
** stub's own test fixture, say -- has every row run to the 1920 ceiling and
** 268 of them per frame, and its derived frame number then reads about 7 per
** cent low. Use "monitor frames" when the count has to be exact.
*/
#define CU_GDB_FRAMECY (262U * 1820U)



#else /* CU_GDB */



#define cu_gdb_parseargs(argc, argv) ((auint)(argc))
#define cu_gdb_focus_capture()       do{}while(0)
#define cu_gdb_startnolimit()        (FALSE)
#define cu_gdb_starthidden()         (FALSE)
#define cu_gdb_init()                do{}while(0)
#define cu_gdb_quit()                do{}while(0)
#define cu_gdb_hold()                (FALSE)
#define cu_gdb_ishalted()            (FALSE)
#define cu_gdb_isenabled()           (FALSE)
#define cu_gdb_ownsbreak()           (FALSE)
#define cu_gdb_foreignstop()         do{}while(0)
#define cu_gdb_foreignresumed()      (FALSE)
#define cu_gdb_setidle(hook)         do{}while(0)



#endif /* CU_GDB */



#endif
