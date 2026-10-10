/*
 *  GDB stub: Paused session indicator (window caption and on screen panel)
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
** While the debugger holds the CPU the emulator produces no new frames, so
** the window holds a still picture -- the finished frame, or the rows drawn
** before the stop if it landed inside the video kernel -- or nothing at all
** with --gdb-wait, where none was ever drawn. A motionless window is
** indistinguishable from a hang, which this module addresses on two fronts:
**
** - The window caption, which is what can be seen when the emulator sits
**   behind the IDE or in the task bar.
** - A panel drawn over the held picture, with a blinking marker so that the
**   process is visibly still servicing its window.
**
** Nothing here is on any hot path: the panel is drawn by the stub's hold
** loop, at its refresh rate of about 66 Hz, and only while halted.
*/



#ifndef CU_GDBOVL_H
#define CU_GDBOVL_H



#include "types.h"



#ifdef CU_GDB



/*
** Latches what the panel should report and applies the matching caption.
** "reason" is a CU_GDBBRK_ST_* value, or CU_GDBBRK_ST_NONE for the halt at
** reset which --gdb-wait performs before any client exists. "addr" is the
** address the reason relates to, as returned by cu_gdbbrk_getstop().
*/
void cu_gdbovl_halted(auint reason, auint addr, boole attached, auint port);


/*
** Applies the caption for a running CPU, whether a client is attached to it
** or not. No panel is drawn in this state.
*/
void cu_gdbovl_running(boole attached, auint port);


/*
** Draws the panel into the GUI pixel buffer. To be called from the hold loop
** immediately before guicore_update(), and only while halted.
*/
void cu_gdbovl_draw(void);


/*
** Switches the panel on or off, which is what F5 does while halted and what
** "monitor overlay on|off" does for a script. Off lifts it from the screen
** and stops it being painted, leaving the frame underneath visible; the
** window caption keeps reporting the halt either way.
*/
void cu_gdbovl_enable(boole ena);


/*
** Whether the panel is switched on.
*/
boole cu_gdbovl_isenabled(void);


/*
** Temporarily lifts the panel, putting back the frame the game produced
** underneath it, so that a capture of the pixel buffer shows the emulator's
** own output rather than the debugger's chrome. Returns TRUE if it did
** anything, in which case cu_gdbovl_show() must be called to put the panel
** back. A no-op returning FALSE when the CPU is not halted.
*/
boole cu_gdbovl_hide(void);


/*
** Repaints the panel after a cu_gdbovl_hide().
*/
void cu_gdbovl_show(void);



#else /* CU_GDB */



#define cu_gdbovl_halted(reason, addr, attached, port) do{}while(0)
#define cu_gdbovl_running(attached, port)              do{}while(0)
#define cu_gdbovl_draw()                               do{}while(0)
#define cu_gdbovl_enable(ena)                          do{}while(0)
#define cu_gdbovl_isenabled()                          (FALSE)
#define cu_gdbovl_hide()                               (FALSE)
#define cu_gdbovl_show()                               do{}while(0)



#endif /* CU_GDB */



#endif
