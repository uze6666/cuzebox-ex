/*
 *  GDB stub: monitor commands
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
** The monitor commands are simultaneously the human's escape hatch in the
** GDB console and the whole agent facing feature surface: a generic MCP GDB
** wrapper with a raw command tool reaches every one of them with no custom
** code. That is why the output contract matters as much as the commands do:
**
**  - one "key: value" per line, no tables, no box drawing, no colour
**  - "--json" on any structured command gives a single JSON object instead
**  - errors are a single "error: <reason>" line, never silence
**  - dumping commands are capped, and say "truncated: true" when they hit it
*/



#ifndef CU_GDBMON_H
#define CU_GDBMON_H



#include "types.h"



#ifdef CU_GDB



/*
** Runs a monitor command, writing its output to the client console through
** cu_gdb_out().
*/
void  cu_gdbmon_exec(char const* cmd);


/*
** Per-frame tick, driving the input recording and replay, the injected input
** queues and the frame countdowns.
*/
void  cu_gdbmon_frame(void);


/*
** The CPU is let go by the client. The frame tick tells a frame by the cycle
** counter having moved since it last looked, and it does not look while the
** CPU is held: whatever ran between its last look and the halt (the pass a
** client attached after, instructions stepped) is not a frame of the run
** which starts now, and a "frames 1" asked for at such a halt ended at once.
*/
void  cu_gdbmon_resumed(void);


/*
** Discards injected input state: the host key queue, timed controller
** presses, an armed frame countdown and an armed --wait. Called when the
** target is reset, since keystrokes queued for the old ROM landing in the new
** one is a trap, and a --wait armed before a reset can never be satisfied.
*/
void  cu_gdbmon_reset(void);


/*
** Exchanges the frame limiter state with main_loop(): takes whether the
** limiter is off right now, returns whether it should be. They differ only
** when "monitor speed" asked for a change since the last call.
**
** The change is applied by main_loop() and not by the monitor command because
** it re-creates the window (vsync is a renderer flag), which is main.c's job
** and is exactly what its F4 handler does. F4 itself is no use to a script:
** once a ROM has enabled the keyboard dongle, main_loop() hands every key to
** it and never reaches the hotkeys.
*/
boole cu_gdbmon_nolimit_sync(boole cur);


/*
** The game's picture alone, as a PNG in the current directory: the game
** area of the frame (what the film takes: no status text, no debug bars),
** each line twice so that the picture has the screen's shape. The file is
** <game>-snapshot-<n>.png, the first number that is free; <game> is the
** ROM file's name up to its first '-' or '.' (uzebros-noattract.hex and
** a recorded run's uzebros-debug.hex are both "uzebros"), which
** cu_gdbmon_setgame() is given at start. From the keyboard: Print Screen,
** or Shift+F12.
*/
void  cu_gdbmon_gameshot(void);
void  cu_gdbmon_setgame(char const* path);



#else /* CU_GDB */



#define cu_gdbmon_nolimit_sync(cur) (cur)
#define cu_gdbmon_gameshot()        do{}while(0)
#define cu_gdbmon_setgame(path)     do{}while(0)



#endif /* CU_GDB */



#endif
