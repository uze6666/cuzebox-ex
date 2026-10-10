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



#include "cu_gdbovl.h"

#ifdef CU_GDB

#include "cu_gdb.h"
#include "cu_gdbbrk.h"
#include "cu_types.h"
#include "guicore.h"
#include "chars_big.h"

#include <stdio.h>
#include <string.h>


/* Emulator state reached through the globals cu_avr.c already exports, the
** same way cu_gdbbrk.c and the keyboard and ESP8266 modules do it, so that
** cu_avr.h stays untouched. */
extern cu_state_cpu_t cpu_state;



/* The window's normal caption, "CUzeBox" plus the loaded game's file name.
** Owned and composed by main.c, reached the way the emulator's other globals
** are, so that the strings below extend it rather than restating it. */
extern char main_caption[];



/* Panel geometry, in the 320 x 270 logical coordinates the pixel buffer is
** addressed by (each logical pixel is two entries of the 640 wide buffer).
**
** The bounds matter: in GUICORE_GAMEONLY only x 5 to 314 and y 18 to 246 of
** that surface reaches the window, and the text GUI's own rows above and
** below are not even drawn in that mode. So the panel lives in the middle of
** the game area, well inside that crop, which is the one placement visible
** in all four display mode combinations.
**
** The panel uses its own larger 8x8 font (chars_big[], see chars_big.h)
** rather than the tiny 6x6 one the main HUD uses, so it needs to be wider
** than a plain 4/3 rescale of the old 264x72 box would allow: the widest
** value string (a watchpoint's reason plus its address, 30 characters after
** the 9 character label column) would then overflow the x 5 to 314 crop
** before any border or padding. OVL_X0/X1 below are pushed out to just
** inside that crop instead, which recovers all but the last couple of
** characters of that one rare case; every other row (heading, hint, and the
** rest of the body) fits with room to spare. */
#define OVL_X0      6U
#define OVL_X1    313U
#define OVL_Y0     87U
#define OVL_Y1    177U

/* Character cell is 8 x 8, one cell of padding inside the border */
#define OVL_TX     (OVL_X0 + 8U)
#define OVL_ROWH   10U

/* Text rows */
#define OVL_ROW_HEAD  (OVL_Y0 +  8U)
#define OVL_ROW_BODY  (OVL_Y0 + 28U)
#define OVL_ROW_HINT  (OVL_Y0 + 73U)

/* Colors, as Uzebox palette indices: bits 0-2 red, 3-5 green, 6-7 blue.
** guicore builds this palette once at init and it is not game controlled, so
** the indices mean the same thing whatever the ROM is doing. */
#define OVL_C_FILL   0x40U   /* Dark blue */
#define OVL_C_EDGE   0xFFU   /* White */
#define OVL_C_HEAD   0x3FU   /* Yellow */
#define OVL_C_LABEL  0xADU   /* Light grey */
#define OVL_C_VALUE  0xFFU   /* White */

/* Blink period of the "paused" markers, as a shift of the millisecond tick.
** 512 ms per state, so about 1 Hz, which reads as deliberate rather than as
** something malfunctioning. */
#define OVL_BLINKSH  9U



/* What the panel reports, latched when the CPU halts */
static boole ovl_ishalted = FALSE;
static boole ovl_iswait   = FALSE;  /* Halted at reset, no client yet */
static boole ovl_attached = FALSE;
static auint ovl_port     = 0U;
static char  ovl_reason[64] = "";
static boole ovl_hasaddr  = FALSE;  /* Whether ovl_addr is worth showing */
static auint ovl_addr     = 0U;

/* Whether the panel is drawn at all. Switched with F5 while halted, or with
** "monitor overlay on|off". The caption is deliberately not affected: it is
** the only thing left saying why the CPU stopped once the panel is off. */
static boole ovl_enabled  = TRUE;

/* Counts draw calls, to re-apply the caption every so often. F4 and F11
** destroy and recreate the window with the emulator's own title, so a caption
** set once when halting would be lost by toggling full screen while paused. */
static auint ovl_capctr   = 0U;

/* Last caption applied, so the periodic re-apply is a no-op for the window
** manager unless something actually changed. Sized to hold main_caption[]
** (which already carries the game's file name) plus the longest suffix
** below, so that a long file name cannot truncate the stop reason away. */
static char  ovl_caption[256] = "";



/*
** Applies a caption, if it differs from the one last applied.
*/
static void ovl_setcaption(char const* str, boole force)
{
 if ( (!force) &&
      (strcmp(&(ovl_caption[0]), str) == 0) ){ return; }

 snprintf(&(ovl_caption[0]), sizeof(ovl_caption), "%s", str);
 guicore_setcaption(&(ovl_caption[0]));
}



/*
** Composes and applies the caption for the current latched state.
*/
static void ovl_caption_update(boole force)
{
 char buf[256];

 if (!ovl_ishalted){

  if (ovl_attached){
   snprintf(&(buf[0]), sizeof(buf), "%s - debugging (port %u)",
       &(main_caption[0]), (unsigned)(ovl_port));
  }else if (ovl_port != 0U){
   snprintf(&(buf[0]), sizeof(buf), "%s - gdb stub on port %u",
       &(main_caption[0]), (unsigned)(ovl_port));
  }else{
   snprintf(&(buf[0]), sizeof(buf), "%s", &(main_caption[0]));
  }

 }else if (ovl_iswait){

  snprintf(&(buf[0]), sizeof(buf), "%s - WAITING for debugger on port %u%s",
      &(main_caption[0]), (unsigned)(ovl_port),
      ovl_enabled ? "" : "  (F5: panel)");

 }else{

  /* A watchpoint's address is the interesting one; for every other stop it
  ** is the PC. Read live rather than latched, so that the periodic re-apply
  ** below picks up a PC the client has since moved with a P packet. Masked
  ** for the same reason the stop reply masks it (see cu_gdbovl_draw).
  **
  ** With the panel switched off there is nothing on screen saying how to
  ** get it back, so the caption carries the key while it is hidden. */
  snprintf(&(buf[0]), sizeof(buf), "%s - PAUSED (debugger): %s @ 0x%06X%s",
      &(main_caption[0]), &(ovl_reason[0]),
      (unsigned)(ovl_hasaddr ? ovl_addr
                             : ((cpu_state.pc & 0x7FFFU) << 1)),
      ovl_enabled ? "" : "  (F5: panel)");

 }

 ovl_setcaption(&(buf[0]), force);
}



/*
** Saves and restores the pixels the panel covers, so that a screenshot can
** show the frame the game actually produced rather than the debugger's own
** chrome sitting on top of it. Two entries of the 640 wide buffer per
** logical pixel, so 614 x 90, about 216 KiB.
**
** The same pair lifts the panel for good when it is switched off (F5 or
** "monitor overlay off"): nothing repaints the buffer while halted, so
** simply ceasing to draw would leave the last painted panel on screen.
*/
#define OVL_BW ((OVL_X1 - OVL_X0) * 2U)
#define OVL_BH (OVL_Y1 - OVL_Y0)

static uint32 ovl_backing[OVL_BW * OVL_BH];
static boole  ovl_hasback = FALSE;


static void ovl_backup(void)
{
 uint32 const* pix = guicore_getpixbuf();
 auint         ptc = guicore_getpitch();
 auint         y;

 for (y = 0U; y < OVL_BH; y ++){
  memcpy(&(ovl_backing[y * OVL_BW]),
      &(pix[((OVL_Y0 + y) * ptc) + (OVL_X0 * 2U)]),
      OVL_BW * sizeof(uint32));
 }
 ovl_hasback = TRUE;
}


static void ovl_restore(void)
{
 uint32* pix = guicore_getpixbuf();
 auint   ptc = guicore_getpitch();
 auint   y;

 for (y = 0U; y < OVL_BH; y ++){
  memcpy(&(pix[((OVL_Y0 + y) * ptc) + (OVL_X0 * 2U)]),
      &(ovl_backing[y * OVL_BW]),
      OVL_BW * sizeof(uint32));
 }
}



/*
** Fills a rectangle of the logical surface with a palette color.
*/
static void ovl_fill(auint x0, auint y0, auint x1, auint y1, auint col)
{
 uint32* pix = guicore_getpixbuf();
 auint   ptc = guicore_getpitch();
 uint32  c   = (guicore_getpalette())[col];
 auint   x;
 auint   y;
 auint   dp;

 for (y = y0; y < y1; y ++){
  dp = (y * ptc) + (x0 * 2U);
  for (x = x0; x < x1; x ++){
   pix[dp     ] = c;
   pix[dp + 1U] = c;
   dp += 2U;
  }
 }
}



/*
** Renders a character of the larger 8 x 8 set at a logical position, leaving
** the background alone (unlike textgui.c's own, which paints it). Set pixels
** are two buffer entries wide, matching that 2:1 pixel aspect ratio.
*/
static void ovl_putchar(auint x, auint y, auint chr, auint col)
{
 uint32* pix = guicore_getpixbuf();
 auint   ptc = guicore_getpitch();
 uint32  c   = (guicore_getpalette())[col];
 auint   i;
 auint   j;
 auint   dp;
 auint   cp;
 auint   ch;

 cp = (chr & 0x7FU) * 8U;
 dp = (y * ptc) + (x * 2U);

 for (i = 0U; i < 8U; i ++){
  ch = chars_big[cp];
  for (j = 0U; j < 8U; j ++){
   if ((ch & (0x01U << j)) != 0U){
    pix[dp + (j * 2U)     ] = c;
    pix[dp + (j * 2U) + 1U] = c;
   }
  }
  cp ++;
  dp += ptc;
 }
}



/*
** Renders a string, clipped to whatever room is left before the panel's right
** border, so that a long value can never spill over it. Spaces are skipped as
** the panel is filled before anything is written on it.
*/
static void ovl_putstr(auint x, auint y, char const* str, auint col)
{
 auint i;
 auint max = (OVL_X1 - 1U - x) / 8U;

 for (i = 0U; (i < max) && (str[i] != 0); i ++){
  if (str[i] != ' '){
   ovl_putchar(x + (i * 8U), y, (auint)((unsigned char)(str[i])), col);
  }
 }
}



/*
** Renders one body row, the label in a dimmer color than the value so the
** numbers stand out at this size.
*/
static void ovl_putrow(auint row, char const* label, char const* value)
{
 auint y = OVL_ROW_BODY + (row * OVL_ROWH);

 ovl_putstr(OVL_TX, y, label, OVL_C_LABEL);
 ovl_putstr(OVL_TX + (9U * 8U), y, value, OVL_C_VALUE);
}



/*
** Returns the current stack pointer, the way cu_gdbbrk.c reads it.
*/
static auint ovl_getsp(void)
{
 return ( ((auint)(cpu_state.iors[CU_IO_SPL])     ) |
          ((auint)(cpu_state.iors[CU_IO_SPH]) << 8) );
}



void cu_gdbovl_halted(auint reason, auint addr, boole attached, auint port)
{
 char const* an;

 ovl_ishalted = TRUE;
 ovl_attached = attached;
 ovl_port     = port;
 ovl_iswait   = (!attached);
 ovl_hasaddr  = FALSE;
 ovl_addr     = 0U;

 switch (reason){

  case CU_GDBBRK_ST_STEP:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "single step");
   break;

  case CU_GDBBRK_ST_SWBREAK:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "breakpoint");
   break;

  case CU_GDBBRK_ST_HWBREAK:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "hardware breakpoint");
   break;

  case CU_GDBBRK_ST_WATCH:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "watchpoint (write)");
   ovl_hasaddr = TRUE;
   break;

  case CU_GDBBRK_ST_RWATCH:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "watchpoint (read)");
   ovl_hasaddr = TRUE;
   break;

  case CU_GDBBRK_ST_AWATCH:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "watchpoint (access)");
   ovl_hasaddr = TRUE;
   break;

  case CU_GDBBRK_ST_ANOMALY:
   an = cu_gdbbrk_getanomaly();
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "break-on: %s",
       (an != NULL) ? an : "anomaly");
   break;

  case CU_GDBBRK_ST_HALT:
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "interrupted by debugger");
   break;

  default:
   /* CU_GDBBRK_ST_NONE: either the halt at reset --gdb-wait performs, or a
   ** client having just attached and not yet asked for anything. */
   snprintf(&(ovl_reason[0]), sizeof(ovl_reason), "%s",
       attached ? "client attached" : "halted at reset");
   break;
 }

 /* Only watchpoints have an address distinct from the PC the panel already
 ** shows on its own row. It arrives in GDB's flattened form (data space at
 ** 0x800000, SPI RAM at 0x820000), which is what to display: it is what the
 ** Z packet used to set the watchpoint, and what GDB reports back. */
 if (ovl_hasaddr){ ovl_addr = addr; }

 ovl_caption_update(FALSE);
}



void cu_gdbovl_running(boole attached, auint port)
{
 ovl_ishalted = FALSE;
 ovl_iswait   = FALSE;
 ovl_attached = attached;
 ovl_port     = port;

 /* The game repaints the area from here on, so the saved pixels are stale;
 ** the next halt takes a fresh copy. */
 ovl_hasback  = FALSE;

 ovl_caption_update(FALSE);
}



/*
** Paints the panel. Assumes the pixels it covers have already been saved.
*/
static void ovl_paint(void)
{
 char  buf[80];
 boole blink;

 blink = (((SDL_GetTicks() >> OVL_BLINKSH) & 1U) != 0U);

 /* Panel body and a one pixel border */

 ovl_fill(OVL_X0, OVL_Y0, OVL_X1, OVL_Y1, OVL_C_FILL);
 ovl_fill(OVL_X0, OVL_Y0, OVL_X1, OVL_Y0 + 1U, OVL_C_EDGE);
 ovl_fill(OVL_X0, OVL_Y1 - 1U, OVL_X1, OVL_Y1, OVL_C_EDGE);
 ovl_fill(OVL_X0, OVL_Y0, OVL_X0 + 1U, OVL_Y1, OVL_C_EDGE);
 ovl_fill(OVL_X1 - 1U, OVL_Y0, OVL_X1, OVL_Y1, OVL_C_EDGE);

 /* Heading. The markers blinking is the point of the whole panel: a still
 ** picture with a caption on it could be a hung process, a blinking one
 ** cannot be. */

 if (ovl_iswait){
  snprintf(&(buf[0]), sizeof(buf), "%s  WAITING FOR DEBUGGER  %s",
      blink ? ">>" : "  ", blink ? "<<" : "  ");
 }else{
  snprintf(&(buf[0]), sizeof(buf), "%s  DEBUG SESSION PAUSED  %s",
      blink ? ">>" : "  ", blink ? "<<" : "  ");
 }
 /* 28 characters wide either way, centered in the 37 available */
 ovl_putstr(OVL_TX + (4U * 8U), OVL_ROW_HEAD, &(buf[0]), OVL_C_HEAD);

 /* Body */

 if (ovl_hasaddr){
  snprintf(&(buf[0]), sizeof(buf), "%s @ 0x%06X",
      &(ovl_reason[0]), (unsigned)(ovl_addr));
 }else{
  snprintf(&(buf[0]), sizeof(buf), "%s", &(ovl_reason[0]));
 }
 ovl_putrow(0U, ovl_iswait ? "Status :" : "Reason :", &(buf[0]));

 /* The PC mask is not cosmetic: cpu_state.pc can carry bits above the 15
 ** which index the 64K of flash, and every other reader masks them away
 ** before use, so showing it raw can be off by a multiple of 0x10000. */
 snprintf(&(buf[0]), sizeof(buf), "0x%06X     SP : 0x%04X",
     (unsigned)((cpu_state.pc & 0x7FFFU) << 1),
     (unsigned)(ovl_getsp()));
 ovl_putrow(1U, "PC     :", &(buf[0]));

 snprintf(&(buf[0]), sizeof(buf), "%llu   Cycle : %llu",
     (unsigned long long)(cu_gdb_getframe()),
     (unsigned long long)(cu_gdb_getcycle64()));
 ovl_putrow(2U, "Frame  :", &(buf[0]));

 if (ovl_attached){
  snprintf(&(buf[0]), sizeof(buf), "connected, port %u",
      (unsigned)(ovl_port));
 }else{
  snprintf(&(buf[0]), sizeof(buf), "none, listening on port %u",
      (unsigned)(ovl_port));
 }
 ovl_putrow(3U, "Client :", &(buf[0]));

 /* How to get rid of it. Dimmer than the values, being help rather than
 ** state. 27 characters, centered in the 37 available. */

 ovl_putstr(OVL_TX + (5U * 8U), OVL_ROW_HINT,
     "Press F5 to hide this panel", OVL_C_LABEL);
}



void cu_gdbovl_draw(void)
{
 if (!ovl_ishalted){ return; }

 /* The window may have been recreated under us by a display mode change,
 ** which restores the emulator's own caption. Cheaper to re-apply now and
 ** then than to detect it. Kept ahead of the switched off case below: with
 ** the panel hidden the caption is the only thing left reporting the halt,
 ** so it has to go on being restored. */
 ovl_capctr ++;
 if ((ovl_capctr & 0x1FU) == 0U){ ovl_caption_update(TRUE); }

 /* Switched off: lift the panel once, then leave the buffer alone. Merely
 ** ceasing to paint would strand the last one on screen, nothing else
 ** repainting while halted. Dropping the backup makes a later switch on
 ** take a fresh one, of the picture actually underneath by then. */
 if (!ovl_enabled){
  if (ovl_hasback){
   ovl_restore();
   ovl_hasback = FALSE;
  }
  return;
 }

 /* Save what the panel is about to cover, once per halt. Only the first
 ** paint sees the game's own pixels; every later one would save the panel
 ** back over itself. Nothing repaints the buffer while halted, so a single
 ** copy stays valid for the whole stop. */
 if (!ovl_hasback){ ovl_backup(); }

 ovl_paint();
}



void cu_gdbovl_enable(boole ena)
{
 if (ovl_enabled == ena){ return; }

 ovl_enabled = ena;

 /* The caption gains or loses its "(F5: panel)" marker with this, and the
 ** panel itself is lifted or repainted by the next cu_gdbovl_draw(), about
 ** 15 ms away. */
 ovl_caption_update(FALSE);
}



boole cu_gdbovl_isenabled(void)
{
 return ovl_enabled;
}



boole cu_gdbovl_hide(void)
{
 if ( (!ovl_ishalted) ||
      (!ovl_hasback) ){ return FALSE; }

 ovl_restore();
 return TRUE;
}



void cu_gdbovl_show(void)
{
 if ( (!ovl_ishalted) ||
      (!ovl_hasback) ){ return; }

 ovl_paint();
}



#endif /* CU_GDB */
