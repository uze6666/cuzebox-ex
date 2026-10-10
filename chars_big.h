/*
 *  Larger character set, for the GDB stub overlay panel
 *
 *  Copyright (C) 2026
 *    Alec Bourque (Uze)
 *  Glyph data based on font8x8_basic, Copyright (C) Daniel Hepper
 *  <daniel@hepper.net>, itself based on Marcel Sondaar's font8x8 (public
 *  domain International Business Machines VGA fonts). Both released as
 *  Public Domain; reformatted here into this project's flat byte-table
 *  layout, matching chars.h's row-per-byte, high-bit-first convention.
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



#ifndef CHARS_BIG_H
#define CHARS_BIG_H



#include "types.h"



/*
** Tiny 8x8 pixel character set, 128 characters, ASCII (only the printable
** 0x20 - 0x7E range is populated; the rest is blank), 8 bytes / character,
** each byte is a pixel row, high bit first beginning with bit 7. Used only
** by the GDB stub overlay panel (cu_gdbovl.c), which needs bigger, more
** legible glyphs than the main HUD's chars[] and has the panel room to
** spare for them.
*/
extern uint8 const chars_big[];


#endif
