/*
 *  GDB stub: Breakpoints, watchpoints and anomaly breaks
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
 *  Breakpoint address halving and the general shape of the Z packet handling
 *  follow tools/uzem/gdbserver.cpp in the Uzebox repository, which descends
 *  from simulavr by Filipe Rinaldi (2009), originally Theodore Roth and
 *  Klaus Rudolph, 2001 - 2003, GPLv2-or-later. The storage is not taken from
 *  there: uzem keeps breakpoints in a std::vector and linear scans it on
 *  every instruction, which is unaffordable here.
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



#include "cu_gdbbrk.h"

#ifdef CU_GDB

#include "cu_types.h"
#include "cu_spir.h"


/* Emulator state reached through the globals cu_avr.c already exports. Doing
** it this way keeps cu_avr.h untouched; the emulator itself uses the same
** pattern for the keyboard and ESP8266 modules. */
extern cu_state_cpu_t cpu_state;
extern uint32         cpu_code[32768];
extern uint8          access_mem[4096U];
extern uint8          access_io[256U];
extern auint          video_pulsectr;

/* Opcode id an undefined instruction compiles to (cu_avrc.c, UNDEF) */
#define OPID_UNDEF 0x4AU

/* Maximal number of simultaneous watchpoints */
#define WP_MAX 64U

/* Largest watchpoint the shadow buffer can cover for SPI RAM */
#define WP_SHADOW_MAX 64U



/* Hot path gates (see cu_gdbbrk.h) */
boole cu_gdb_hook_ena = FALSE;
boole cu_gdb_slow     = FALSE;
boole cu_gdb_wp_ena   = FALSE;
uint8 cu_gdb_bpmap[4096];



typedef struct{
 boole used;
 auint type;                        /* CU_GDBBRK_WP_* */
 auint space;                       /* CU_GDBBRK_SP_* */
 auint addr;                        /* Start address within the space */
 auint len;
 uint8 shadow[WP_SHADOW_MAX];       /* SPI RAM only */
}brk_wp_t;

static brk_wp_t brk_wp[WP_MAX];
static auint    brk_wpcount    = 0U;
static auint    brk_wpspiram   = 0U; /* Watchpoints living in the SPI RAM */

/* Number of breakpoints set, so the bitmap does not have to be scanned */
static auint    brk_bpcount    = 0U;

/* Halt requested by the client (Ctrl-C) */
static boole    brk_haltreq    = FALSE;

/* Single step in progress */
static boole    brk_stepping   = FALSE;

/* Set on resume, cleared by the first hook call after it. Only meaningful
** while stepping, which always runs the hook on every instruction. */
static boole    brk_skipbp     = FALSE;

/* Suppresses the breakpoint check for the instruction execution resumes on,
** so that continuing from a breakpoint does not stop on it again. Keyed on
** the cycle counter rather than on "the first hook call": in the fast path
** the hook is only entered at breakpointed addresses, so a call keyed flag
** would survive to the next breakpoint, whichever one that was, and swallow
** that one instead. */
static boole    brk_bpsupp     = FALSE;
static auint    brk_bpsuppcy   = 0U;

/* Reason and address of the last halt */
static auint    brk_stopreason = CU_GDBBRK_ST_NONE;
static auint    brk_stopaddr   = 0U;

/* Watchpoint hit recorded by the memory path, acted on by the hook at the
** end of the instruction that caused it (which is where GDB expects the
** stop, so that it can re-read the location and print old and new value) */
static boole    brk_wphit      = FALSE;
static auint    brk_wphittype  = 0U;
static auint    brk_wphitaddr  = 0U;

/* Anomaly break configuration and state */
static boole    brk_an_ena[CU_GDBBRK_AN_COUNT];
static auint    brk_an_par[CU_GDBBRK_AN_COUNT];
static boole    brk_an_hit     = FALSE;
static char     brk_an_detail[192];

/* Reverse execution replay */
static boole    brk_rep_oncy   = FALSE;
static auint    brk_rep_cycle  = 0U;
static boole    brk_rep_oninst = FALSE;
static auint    brk_rep_target = 0U;
static auint    brk_rep_count  = 0U;

/* Break condition evaluation mode (CU_GDBBRK_MODE_*) and the hit the SCAN
** mode records */
static auint    brk_mode        = CU_GDBBRK_MODE_NORMAL;
static boole    brk_scan_found  = FALSE;
static auint    brk_scan_idx    = 0U;
static auint    brk_scan_reason = 0U;
static auint    brk_scan_addr   = 0U;

/* Scanline profiling */
static boole    brk_prof_ena   = FALSE;
static auint    brk_prof_rows  = 0U;
static auint    brk_prof_total = 0U;
static auint    brk_prof_worst = 0U;
static auint    brk_prof_wrow  = 0U;
static auint    brk_prof_over  = 0U;
static auint    brk_prof_pulse = 0xFFFFFFFFU;
static auint    brk_prof_cyc   = 0U;

/* Interrupt latency tracking */
static boole    brk_irq_pend   = FALSE;
static auint    brk_irq_cycle  = 0U;

/* Set of I/O addresses the ATmega644 implements. Bit set means implemented.
** Addresses 0x00 - 0x1F are the register file and are always valid. */
static uint8    brk_io_valid[32];
static boole    brk_io_ready   = FALSE;

/* The I/O registers an ATmega644 has, by data space address. CUzebox's own
** whisper ports at 0x39 and 0x3A are included so that using them does not
** trip the unmapped-io break. */
static uint16 const brk_io_list[] = {
 0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U,
 0x28U, 0x29U, 0x2AU, 0x2BU, 0x35U, 0x36U, 0x37U, 0x39U,
 0x3AU, 0x3BU, 0x3CU, 0x3DU, 0x3EU, 0x3FU, 0x40U, 0x41U,
 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U, 0x4AU,
 0x4BU, 0x4CU, 0x4DU, 0x4EU, 0x50U, 0x51U, 0x53U, 0x54U,
 0x55U, 0x57U, 0x5DU, 0x5EU, 0x5FU, 0x60U, 0x61U, 0x64U,
 0x66U, 0x68U, 0x69U, 0x6BU, 0x6CU, 0x6DU, 0x6EU, 0x6FU,
 0x70U, 0x73U, 0x78U, 0x79U, 0x7AU, 0x7BU, 0x7CU, 0x7EU,
 0x7FU, 0x80U, 0x81U, 0x82U, 0x84U, 0x85U, 0x86U, 0x87U,
 0x88U, 0x89U, 0x8AU, 0x8BU, 0xB0U, 0xB1U, 0xB2U, 0xB3U,
 0xB4U, 0xB6U, 0xB8U, 0xB9U, 0xBAU, 0xBBU, 0xBCU, 0xBDU,
 0xC0U, 0xC1U, 0xC2U, 0xC4U, 0xC5U, 0xC6U };



/*
** Builds the implemented I/O address set on first use.
*/
static void brk_io_prepare(void)
{
 auint i;

 if (brk_io_ready){ return; }

 for (i = 0U; i < 32U; i++){ brk_io_valid[i] = 0U; }
 for (i = 0U; i < 4U;  i++){ brk_io_valid[i] = 0xFFU; } /* r0 - r31 */
 for (i = 0U; i < (sizeof(brk_io_list) / sizeof(brk_io_list[0])); i++){
  brk_io_valid[brk_io_list[i] >> 3] |= (uint8)(1U << (brk_io_list[i] & 7U));
 }

 brk_io_ready = TRUE;
}



/*
** Reads a byte of the SPI RAM, or zero if out of range.
*/
static auint brk_spiram_read(auint addr)
{
 cu_state_spir_t* sr = cu_spir_get_state();

 if ((sr->ram == NULL) || (addr >= sr->size)){ return 0U; }
 return sr->ram[addr];
}



/*
** Records a stop, disarming everything which should not survive it.
*/
static void brk_stop(auint reason, auint addr)
{
 brk_stopreason = reason;
 brk_stopaddr   = addr;
 brk_haltreq    = FALSE;
 brk_stepping   = FALSE;
 brk_rep_oncy   = FALSE;
 brk_rep_oninst = FALSE;
}



/*
** Recomputes the hot path gates. cu_gdb_slow must be set whenever anything
** has to be evaluated on every instruction; leaving it clear is what keeps a
** merely attached client from costing a function call per instruction.
*/
void  cu_gdbbrk_regate(void)
{
 auint i;
 boole slow = FALSE;
 boole anom = FALSE;

 for (i = 0U; i < CU_GDBBRK_AN_COUNT; i++){
  if (brk_an_ena[i]){ anom = TRUE; }
 }

 if (brk_haltreq   ){ slow = TRUE; }
 if (brk_stepping  ){ slow = TRUE; }
 if (brk_wpcount   ){ slow = TRUE; }
 if (brk_rep_oncy  ){ slow = TRUE; }
 if (brk_rep_oninst){ slow = TRUE; }
 if (brk_prof_ena  ){ slow = TRUE; }
 if (anom          ){ slow = TRUE; }

 cu_gdb_slow   = slow;
 cu_gdb_wp_ena = (boole)( (brk_wpcount != 0U) ||
                          (brk_an_ena[CU_GDBBRK_AN_UNINIT_READ]) ||
                          (brk_an_ena[CU_GDBBRK_AN_UNMAPPED_IO]) );
}



/*
** Clears every breakpoint, watchpoint and anomaly break.
*/
void  cu_gdbbrk_reset(void)
{
 auint i;

 for (i = 0U; i < 4096U; i++){ cu_gdb_bpmap[i] = 0U; }
 for (i = 0U; i < WP_MAX; i++){ brk_wp[i].used = FALSE; }
 for (i = 0U; i < CU_GDBBRK_AN_COUNT; i++){ brk_an_ena[i] = FALSE; }

 brk_an_par[CU_GDBBRK_AN_STACK_OVERFLOW] = 0x0100U; /* Bottom of the SRAM */
 brk_an_par[CU_GDBBRK_AN_IRQ_LATENCY]    = 64U;     /* Cycles */

 brk_bpcount    = 0U;
 brk_wpcount    = 0U;
 brk_wpspiram   = 0U;
 brk_haltreq    = FALSE;
 brk_stepping   = FALSE;
 brk_skipbp     = FALSE;
 brk_bpsupp     = FALSE;
 brk_wphit      = FALSE;
 brk_an_hit     = FALSE;
 brk_stopreason = CU_GDBBRK_ST_NONE;
 brk_rep_oncy   = FALSE;
 brk_rep_oninst = FALSE;
 brk_prof_ena   = FALSE;
 brk_irq_pend   = FALSE;
 brk_mode       = CU_GDBBRK_MODE_NORMAL;
 brk_scan_found = FALSE;

 cu_gdbbrk_profile_clear();
 cu_gdbbrk_regate();
}



/*
** Adds or removes a breakpoint. GDB hands over a byte address; the core's PC
** is a word address, so it is halved before it indexes the bitmap.
*/
boole cu_gdbbrk_bp_set(auint addr, boole ena)
{
 auint wpc = (addr >> 1) & 0x7FFFU;
 auint idx;
 uint8 msk;

 if (addr > 0xFFFFU){ return FALSE; }

 idx = wpc >> 3;
 msk = (uint8)(1U << (wpc & 7U));

 if (ena){
  if ((cu_gdb_bpmap[idx] & msk) == 0U){
   cu_gdb_bpmap[idx] |= msk;
   brk_bpcount ++;
  }
 }else{
  if ((cu_gdb_bpmap[idx] & msk) != 0U){
   cu_gdb_bpmap[idx] &= (uint8)(~msk);
   brk_bpcount --;
  }
 }

 return TRUE;
}



/*
** Adds a watchpoint.
*/
boole cu_gdbbrk_wp_add(auint type, auint addr, auint len)
{
 auint i;
 auint space;
 auint base;
 auint limit;
 cu_state_spir_t* sr;

 if (len == 0U){ len = 1U; }

 if       ((addr >= 0x800000U) && (addr < 0x810000U)){
  space = CU_GDBBRK_SP_DATA;
  base  = addr - 0x800000U;
  limit = 0x1100U;
 }else if ((addr >= 0x820000U) && (addr < 0x900000U)){
  sr    = cu_spir_get_state();
  space = CU_GDBBRK_SP_SPIRAM;
  base  = addr - 0x820000U;
  limit = sr->size;
  /* The SPI RAM has no write hook in cu_spir.c, so its watchpoints are done
  ** by shadow comparison. That covers writes but cannot see reads. */
  if (type != CU_GDBBRK_WP_WRITE){ return FALSE; }
  if (len > WP_SHADOW_MAX){ return FALSE; }
 }else{
  return FALSE;
 }

 if ((base >= limit) || (len > (limit - base))){ return FALSE; }

 for (i = 0U; i < WP_MAX; i++){
  if (!brk_wp[i].used){ break; }
 }
 if (i == WP_MAX){ return FALSE; }

 brk_wp[i].used  = TRUE;
 brk_wp[i].type  = type;
 brk_wp[i].space = space;
 brk_wp[i].addr  = base;
 brk_wp[i].len   = len;
 brk_wpcount ++;

 if (space == CU_GDBBRK_SP_SPIRAM){
  auint j;
  for (j = 0U; j < len; j++){
   brk_wp[i].shadow[j] = (uint8)(brk_spiram_read(base + j));
  }
  brk_wpspiram ++;
 }

 cu_gdbbrk_regate();
 return TRUE;
}



/*
** Removes a watchpoint.
*/
boole cu_gdbbrk_wp_del(auint type, auint addr, auint len)
{
 auint i;
 auint space;
 auint base;

 if (len == 0U){ len = 1U; }

 if       ((addr >= 0x800000U) && (addr < 0x810000U)){
  space = CU_GDBBRK_SP_DATA;
  base  = addr - 0x800000U;
 }else if ((addr >= 0x820000U) && (addr < 0x900000U)){
  space = CU_GDBBRK_SP_SPIRAM;
  base  = addr - 0x820000U;
 }else{
  return FALSE;
 }

 for (i = 0U; i < WP_MAX; i++){
  if ( (brk_wp[i].used          ) &&
       (brk_wp[i].type  == type ) &&
       (brk_wp[i].space == space) &&
       (brk_wp[i].addr  == base ) &&
       (brk_wp[i].len   == len  ) ){
   brk_wp[i].used = FALSE;
   brk_wpcount --;
   if (space == CU_GDBBRK_SP_SPIRAM){ brk_wpspiram --; }
   cu_gdbbrk_regate();
   return TRUE;
  }
 }

 return FALSE;
}



/*
** Watchpoint check on the data space memory path. Also carries the
** uninit-read and unmapped-io anomaly checks, since they need exactly the
** same call sites and the same gate.
*/
void  cu_gdbbrk_watch(auint addr, boole iswrite)
{
 auint i;
 auint want = iswrite ? CU_GDBBRK_WP_WRITE : CU_GDBBRK_WP_READ;

 if (brk_wpcount != 0U){

  for (i = 0U; i < WP_MAX; i++){
   if (!brk_wp[i].used){ continue; }
   if (brk_wp[i].space != CU_GDBBRK_SP_DATA){ continue; }
   if ( (addr <  brk_wp[i].addr) ||
        (addr >= (brk_wp[i].addr + brk_wp[i].len)) ){ continue; }
   if ( (brk_wp[i].type != want) &&
        (brk_wp[i].type != CU_GDBBRK_WP_ACCESS) ){ continue; }

   /* Recorded, not acted on: the hook halts at the end of the instruction,
   ** which is where GDB expects a watchpoint stop. Reported back in GDB's
   ** flattened data-space address, matching what the Z-packet used to set
   ** it (mirrors the +0x820000 done for the SPI RAM path below). */
   brk_wphit     = TRUE;
   brk_wphitaddr = 0x800000U + addr;
   brk_wphittype = brk_wp[i].type;
   break;
  }

 }

 if ( (!iswrite) &&
      (brk_an_ena[CU_GDBBRK_AN_UNINIT_READ]) &&
      (addr >= 0x0100U) && (addr <= 0x10FFU) ){
  /* The emulator's own access info block already records whether a byte was
  ** ever written, so no extra bookkeeping is needed for this. */
  if ((access_mem[addr & 0x0FFFU] & CU_MEM_W) == 0U){
   char det[64];
   sprintf(&(det[0]), "uninitialised SRAM read at 0x%04X", (unsigned)(addr));
   cu_gdbbrk_anomaly_hit(CU_GDBBRK_AN_UNINIT_READ, &(det[0]));
  }
 }

 if ( (brk_an_ena[CU_GDBBRK_AN_UNMAPPED_IO]) &&
      (addr >= 0x0020U) && (addr < 0x0100U) ){
  brk_io_prepare();
  if ((brk_io_valid[addr >> 3] & (1U << (addr & 7U))) == 0U){
   char det[64];
   sprintf(&(det[0]), "%s of unimplemented I/O 0x%02X",
       iswrite ? "write" : "read", (unsigned)(addr));
   cu_gdbbrk_anomaly_hit(CU_GDBBRK_AN_UNMAPPED_IO, &(det[0]));
  }
 }
}



/*
** Compares the SPI RAM shadows, reporting the first changed watchpoint.
*/
static boole brk_spiram_check(void)
{
 auint i;
 auint j;
 auint v;

 for (i = 0U; i < WP_MAX; i++){
  if (!brk_wp[i].used){ continue; }
  if (brk_wp[i].space != CU_GDBBRK_SP_SPIRAM){ continue; }
  for (j = 0U; j < brk_wp[i].len; j++){
   v = brk_spiram_read(brk_wp[i].addr + j);
   if (v != brk_wp[i].shadow[j]){
    brk_wp[i].shadow[j] = (uint8)(v);
    brk_wphit     = TRUE;
    brk_wphittype = CU_GDBBRK_WP_WRITE;
    brk_wphitaddr = 0x820000U + brk_wp[i].addr + j;
    return TRUE;
   }
  }
 }

 return FALSE;
}



/*
** Evaluates whether an enabled interrupt is waiting to be serviced. This
** mirrors the conditions cu_avr_itcheck() tests.
*/
static boole brk_irq_pending(void)
{
 uint8 const* io = &(cpu_state.iors[0]);

 if ((io[CU_IO_SPCR] & io[CU_IO_SPSR] & 0x80U) != 0U){ return TRUE; }
 if ((io[CU_IO_WDTCSR] & 0xC0U) == 0xC0U){ return TRUE; }
 if ((io[CU_IO_TIFR1] & io[CU_IO_TIMSK1] & 0x07U) != 0U){ return TRUE; }
 if ((io[CU_IO_UCSR0A] & 0x40U) != 0U){ return TRUE; }
 return FALSE;
}



/*
** Per-scanline bookkeeping, driven off the pulse counter changing. Used for
** the profile command and the video overrun break.
*/
static void brk_scanline_track(void)
{
 auint cost;

 if (video_pulsectr == brk_prof_pulse){ return; }

 if (brk_prof_pulse != 0xFFFFFFFFU){

  cost = WRAP32(cpu_state.cycle - brk_prof_cyc);

  if (cost < 0x00100000U){ /* Guards against a state restore or a reset */

   brk_prof_rows  ++;
   brk_prof_total += cost;
   if (cost > brk_prof_worst){
    brk_prof_worst = cost;
    brk_prof_wrow  = brk_prof_pulse;
   }

   if (cost > CU_GDBBRK_ROWBUDGET){
    brk_prof_over ++;
    if (brk_an_ena[CU_GDBBRK_AN_VIDEO_OVERRUN]){
     char det[96];
     sprintf(&(det[0]),
         "scanline %u took %u cycles, budget %u",
         (unsigned)(brk_prof_pulse), (unsigned)(cost),
         (unsigned)(CU_GDBBRK_ROWBUDGET));
     cu_gdbbrk_anomaly_hit(CU_GDBBRK_AN_VIDEO_OVERRUN, &(det[0]));
    }
   }

  }
 }

 brk_prof_pulse = video_pulsectr;
 brk_prof_cyc   = cpu_state.cycle;
}



/*
** Acts on a break condition according to the current mode. Returns TRUE when
** the caller must halt.
*/
static boole brk_fire(auint reason, auint addr)
{
 if (brk_mode == CU_GDBBRK_MODE_SUPPRESS){ return FALSE; }

 if (brk_mode == CU_GDBBRK_MODE_SCAN){
  /* Recorded, not acted on. Reverse-continue wants the *last* such point
  ** before where it started, so later hits overwrite earlier ones. */
  brk_scan_found  = TRUE;
  brk_scan_idx    = brk_rep_count;
  brk_scan_reason = reason;
  brk_scan_addr   = addr;
  return FALSE;
 }

 brk_stop(reason, addr);
 cu_gdbbrk_regate();
 return TRUE;
}



/*
** Per-instruction hook. Returns TRUE when execution must halt before the
** instruction at the current PC.
**
** Ordering matters: anything caused by the instruction that just finished is
** reported first, so the CPU is presented to GDB standing after it.
*/
boole cu_gdbbrk_exec_hook(void)
{
 auint pc = cpu_state.pc & 0x7FFFU;
 auint sp;
 boole skipbp = brk_skipbp;

 brk_skipbp = FALSE;

 if (cu_gdb_slow){

  /* A watchpoint recorded by the memory path during the previous
  ** instruction */

  if (brk_wphit){
   brk_wphit = FALSE;
   if (brk_fire( (brk_wphittype == CU_GDBBRK_WP_READ  ) ? CU_GDBBRK_ST_RWATCH :
                 (brk_wphittype == CU_GDBBRK_WP_ACCESS) ? CU_GDBBRK_ST_AWATCH :
                                                          CU_GDBBRK_ST_WATCH,
                 brk_wphitaddr )){ return TRUE; }
  }

  /* SPI RAM watchpoints, which have no hook to fire from */

  if (brk_wpspiram != 0U){
   if (brk_spiram_check()){
    brk_wphit = FALSE;
    if (brk_fire(CU_GDBBRK_ST_WATCH, brk_wphitaddr)){ return TRUE; }
   }
  }

  /* Scanline accounting */

  if (brk_prof_ena || brk_an_ena[CU_GDBBRK_AN_VIDEO_OVERRUN]){
   brk_scanline_track();
  }

  /* Interrupt latency */

  if (brk_an_ena[CU_GDBBRK_AN_IRQ_LATENCY]){
   boole pend = brk_irq_pending();
   if (pend && (!brk_irq_pend)){
    brk_irq_cycle = cpu_state.cycle;
   }else if ((!pend) && brk_irq_pend){
    auint lat = WRAP32(cpu_state.cycle - brk_irq_cycle);
    if (lat > brk_an_par[CU_GDBBRK_AN_IRQ_LATENCY]){
     char det[96];
     sprintf(&(det[0]), "interrupt latency %u cycles, threshold %u",
         (unsigned)(lat),
         (unsigned)(brk_an_par[CU_GDBBRK_AN_IRQ_LATENCY]));
     cu_gdbbrk_anomaly_hit(CU_GDBBRK_AN_IRQ_LATENCY, &(det[0]));
    }
   }else{}
   brk_irq_pend = pend;
  }

  /* Stack overflow */

  if (brk_an_ena[CU_GDBBRK_AN_STACK_OVERFLOW]){
   sp = ((auint)(cpu_state.iors[CU_IO_SPL])     ) |
        ((auint)(cpu_state.iors[CU_IO_SPH]) << 8);
   if (sp < brk_an_par[CU_GDBBRK_AN_STACK_OVERFLOW]){
    char det[96];
    sprintf(&(det[0]), "SP 0x%04X below floor 0x%04X",
        (unsigned)(sp),
        (unsigned)(brk_an_par[CU_GDBBRK_AN_STACK_OVERFLOW]));
    cu_gdbbrk_anomaly_hit(CU_GDBBRK_AN_STACK_OVERFLOW, &(det[0]));
   }
  }

  /* Illegal opcode. cu_avrc.c compiles anything it does not recognise to
  ** the UNDEF operation, which the decoder otherwise silently runs as a one
  ** cycle no-operation. */

  if (brk_an_ena[CU_GDBBRK_AN_ILLEGAL_OPCODE]){
   if ((cpu_code[pc] & 0x7FU) == OPID_UNDEF){
    char det[96];
    sprintf(&(det[0]), "undefined opcode at 0x%04X", (unsigned)(pc << 1));
    cu_gdbbrk_anomaly_hit(CU_GDBBRK_AN_ILLEGAL_OPCODE, &(det[0]));
   }
  }

  if (brk_an_hit){
   brk_an_hit = FALSE;
   if (brk_fire(CU_GDBBRK_ST_ANOMALY, pc << 1)){ return TRUE; }
  }

  /* Reverse execution replay targets. These are the replay's own scaffolding
  ** and always halt, whatever the mode is. */

  if (brk_rep_oninst){
   if (brk_rep_count >= brk_rep_target){
    brk_stop(CU_GDBBRK_ST_STEP, pc << 1);
    cu_gdbbrk_regate();
    return TRUE;
   }
  }

  if (brk_rep_oncy){
   if (cpu_state.cycle == brk_rep_cycle){
    brk_stop(CU_GDBBRK_ST_STEP, pc << 1);
    cu_gdbbrk_regate();
    return TRUE;
   }
  }

  /* Halt asked for by the client */

  if (brk_haltreq){
   brk_stop(CU_GDBBRK_ST_HALT, pc << 1);
   cu_gdbbrk_regate();
   return TRUE;
  }

  /* Single step, which completes once one instruction has run. skipbp is
  ** only clear from the second hook call after the resume, which is exactly
  ** when the stepped instruction has been executed. */

  if (brk_stepping){
   if (!skipbp){
    brk_stop(CU_GDBBRK_ST_STEP, pc << 1);
    cu_gdbbrk_regate();
    return TRUE;
   }
  }

 }

 /* Breakpoints. Suppressed for the instruction execution resumed on, so that
 ** continuing from a breakpoint does not stop on it again. The suppression
 ** lifts as soon as the cycle counter has moved, that is, as soon as one
 ** instruction has actually been executed. */

 if (brk_bpsupp && (cpu_state.cycle != brk_bpsuppcy)){ brk_bpsupp = FALSE; }

 if ( (!brk_bpsupp) &&
      (brk_bpcount != 0U) &&
      (((cu_gdb_bpmap[pc >> 3] >> (pc & 7U)) & 1U) != 0U) ){
  if (brk_fire(CU_GDBBRK_ST_SWBREAK, pc << 1)){ return TRUE; }
 }

 if (cu_gdb_slow){ brk_rep_count ++; }

 return FALSE;
}



/*
** Break condition evaluation mode.
*/
void  cu_gdbbrk_mode(auint mode)
{
 brk_mode = mode;
 if (mode == CU_GDBBRK_MODE_SCAN){ brk_scan_found = FALSE; }
}



/*
** Returns the break condition recorded in SCAN mode.
*/
boole cu_gdbbrk_scan_get(auint* idx, auint* reason, auint* addr)
{
 if (!brk_scan_found){ return FALSE; }
 if (idx    != NULL){ *idx    = brk_scan_idx;    }
 if (reason != NULL){ *reason = brk_scan_reason; }
 if (addr   != NULL){ *addr   = brk_scan_addr;   }
 return TRUE;
}



/*
** Requests a single instruction step on the next resume.
*/
void  cu_gdbbrk_step_req(void)
{
 brk_stepping = TRUE;
 cu_gdbbrk_regate();
}



/*
** Records a halt which something else brought about (the emulator's own
** debugger stopping the CPU), at the instruction it stopped before.
*/
void  cu_gdbbrk_extstop(void)
{
 brk_stop(CU_GDBBRK_ST_HALT, (cpu_state.pc & 0x7FFFU) << 1);
}



/*
** Requests a halt before the next instruction.
*/
void  cu_gdbbrk_halt_req(void)
{
 brk_haltreq = TRUE;
 cu_gdbbrk_regate();
}



/*
** Called when execution resumes.
*/
void  cu_gdbbrk_resume(void)
{
 brk_skipbp     = TRUE;
 brk_bpsupp     = TRUE;
 brk_bpsuppcy   = cpu_state.cycle;
 brk_wphit      = FALSE;
 brk_an_hit     = FALSE;
 brk_haltreq    = FALSE; /* Resuming cancels a pending Ctrl-C */
 brk_stopreason = CU_GDBBRK_ST_NONE;
 brk_prof_pulse = 0xFFFFFFFFU; /* Restart the scanline measurement */
}



/*
** Returns the reason for the last halt.
*/
auint cu_gdbbrk_getstop(auint* addr)
{
 if (addr != NULL){ *addr = brk_stopaddr; }
 return brk_stopreason;
}



/*
** Returns a description of the last anomaly break.
*/
char const* cu_gdbbrk_getanomaly(void)
{
 if (brk_stopreason != CU_GDBBRK_ST_ANOMALY){ return NULL; }
 return &(brk_an_detail[0]);
}



/*
** Reverse execution replay arming.
*/
void  cu_gdbbrk_replay_toc(auint cycle)
{
 brk_rep_oncy   = TRUE;
 brk_rep_oninst = FALSE;
 brk_rep_cycle  = cycle;
 brk_rep_count  = 0U;
 cu_gdbbrk_regate();
}

void  cu_gdbbrk_replay_toi(auint ninstr)
{
 brk_rep_oncy   = FALSE;
 brk_rep_oninst = TRUE;
 brk_rep_target = ninstr;
 brk_rep_count  = 0U;
 cu_gdbbrk_regate();
}

void  cu_gdbbrk_replay_off(void)
{
 brk_rep_oncy   = FALSE;
 brk_rep_oninst = FALSE;
 cu_gdbbrk_regate();
}

auint cu_gdbbrk_replay_cnt(void)
{
 return brk_rep_count;
}



/*
** Anomaly break configuration.
*/
void  cu_gdbbrk_anomaly_set(auint which, boole ena)
{
 if (which >= CU_GDBBRK_AN_COUNT){ return; }
 brk_an_ena[which] = ena;
 if (which == CU_GDBBRK_AN_IRQ_LATENCY){ brk_irq_pend = brk_irq_pending(); }
 cu_gdbbrk_regate();
}

boole cu_gdbbrk_anomaly_get(auint which)
{
 if (which >= CU_GDBBRK_AN_COUNT){ return FALSE; }
 return brk_an_ena[which];
}

void  cu_gdbbrk_anomaly_par(auint which, auint par)
{
 if (which >= CU_GDBBRK_AN_COUNT){ return; }
 brk_an_par[which] = par;
}

auint cu_gdbbrk_anomaly_getpar(auint which)
{
 if (which >= CU_GDBBRK_AN_COUNT){ return 0U; }
 return brk_an_par[which];
}



/*
** Reports an anomaly. The hook turns it into a halt at the end of the
** instruction which caused it.
*/
void  cu_gdbbrk_anomaly_hit(auint which, char const* detail)
{
 if (which >= CU_GDBBRK_AN_COUNT){ return; }
 if (!brk_an_ena[which]){ return; }
 if (brk_an_hit){ return; } /* Keep the first one */

 brk_an_hit = TRUE;

 if (detail != NULL){
  strncpy(&(brk_an_detail[0]), detail, sizeof(brk_an_detail) - 1U);
  brk_an_detail[sizeof(brk_an_detail) - 1U] = 0;
 }else{
  brk_an_detail[0] = 0;
 }
}



/*
** Fills the SRAM with the poison pattern.
*/
void  cu_gdbbrk_poison(void)
{
 auint i;

 for (i = 0U; i < 4096U; i++){
  cpu_state.sram[i] = CU_GDBBRK_POISON;
 }
}



/*
** Scanline profiling.
*/
void  cu_gdbbrk_profile_ena(boole ena)
{
 brk_prof_ena   = ena;
 brk_prof_pulse = 0xFFFFFFFFU;
 cu_gdbbrk_regate();
}

boole cu_gdbbrk_profile_ison(void)
{
 return brk_prof_ena;
}

void  cu_gdbbrk_profile_get(auint* rows, auint* total, auint* worst,
                            auint* worstrow, auint* overruns)
{
 if (rows     != NULL){ *rows     = brk_prof_rows;  }
 if (total    != NULL){ *total    = brk_prof_total; }
 if (worst    != NULL){ *worst    = brk_prof_worst; }
 if (worstrow != NULL){ *worstrow = brk_prof_wrow;  }
 if (overruns != NULL){ *overruns = brk_prof_over;  }
}

void  cu_gdbbrk_profile_clear(void)
{
 brk_prof_rows  = 0U;
 brk_prof_total = 0U;
 brk_prof_worst = 0U;
 brk_prof_wrow  = 0U;
 brk_prof_over  = 0U;
 brk_prof_pulse = 0xFFFFFFFFU;
}



#endif /* CU_GDB */
