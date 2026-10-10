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
** This header is included by cu_avr.c and cu_avr_n.h to provide the hot path
** hooks. When CU_GDB is not defined every hook collapses to nothing, so the
** emulator sources are unaffected and the Emscripten build carries no stub.
*/



#ifndef CU_GDBBRK_H
#define CU_GDBBRK_H



#include "types.h"



#ifdef CU_GDB



/* Watchpoint kinds, matching the GDB Z packet types */
#define CU_GDBBRK_WP_WRITE   2U
#define CU_GDBBRK_WP_READ    3U
#define CU_GDBBRK_WP_ACCESS  4U

/* Address spaces a watchpoint can live in */
#define CU_GDBBRK_SP_DATA    0U
#define CU_GDBBRK_SP_SPIRAM  1U

/* Stop reasons reported through cu_gdbbrk_getstop() */
#define CU_GDBBRK_ST_NONE      0U
#define CU_GDBBRK_ST_STEP      1U
#define CU_GDBBRK_ST_SWBREAK   2U
#define CU_GDBBRK_ST_HWBREAK   3U
#define CU_GDBBRK_ST_WATCH     4U
#define CU_GDBBRK_ST_RWATCH    5U
#define CU_GDBBRK_ST_AWATCH    6U
#define CU_GDBBRK_ST_ANOMALY   7U
#define CU_GDBBRK_ST_HALT      8U

/* Anomaly break conditions */
#define CU_GDBBRK_AN_VIDEO_OVERRUN   0U
#define CU_GDBBRK_AN_STACK_OVERFLOW  1U
#define CU_GDBBRK_AN_UNMAPPED_IO     2U
#define CU_GDBBRK_AN_UNINIT_READ     3U
#define CU_GDBBRK_AN_ILLEGAL_OPCODE  4U
#define CU_GDBBRK_AN_IRQ_LATENCY     5U
#define CU_GDBBRK_AN_COUNT           6U

/* Value SRAM is poisoned with at reset so uninitialised reads can be seen */
#define CU_GDBBRK_POISON  0xC5U



/*
** Master gate for the per-instruction hook. FALSE whenever no GDB client is
** attached, which is what keeps the instruction loop at its original cost.
*/
extern boole cu_gdb_hook_ena;

/*
** Set when something beyond the plain breakpoint bitmap has to be evaluated
** on every instruction (single step, watchpoints, anomaly breaks, a pending
** halt request). Kept separate so that a client which is merely attached and
** free running only pays for a bitmap lookup.
*/
extern boole cu_gdb_slow;

/*
** Gate for the memory path watchpoint checks.
*/
extern boole cu_gdb_wp_ena;

/*
** Breakpoint bitmap over the 32768 word addresses of the Code ROM. 4 KiB,
** never a list: a linear scan per instruction would be unaffordable at
** 28.6 MHz emulated with the frame limiter off.
*/
extern uint8 cu_gdb_bpmap[4096];



/*
** Per-instruction hook. Returns TRUE when execution must halt before the
** instruction at the current PC. Only called when the macro below decides
** something is worth looking at.
*/
boole cu_gdbbrk_exec_hook(void);

/*
** Watchpoint check on the data space memory path. "iswrite" selects between
** a write and a read access.
*/
void  cu_gdbbrk_watch(auint addr, boole iswrite);



/*
** The hot path hook. This is the only thing added to the instruction loop.
*/
#define CU_GDB_EXEC_HOOK(pc) \
 ( (cu_gdb_hook_ena) && \
   ( (cu_gdb_slow) || \
     (((cu_gdb_bpmap[((pc) & 0x7FFFU) >> 3] >> ((pc) & 7U)) & 1U) != 0U) ) \
   ? cu_gdbbrk_exec_hook() : FALSE )

/*
** The memory path hooks.
*/
#define CU_GDB_WATCH_W(addr) \
 do{ if (cu_gdb_wp_ena){ cu_gdbbrk_watch((addr), TRUE ); } }while(0)

#define CU_GDB_WATCH_R(addr) \
 do{ if (cu_gdb_wp_ena){ cu_gdbbrk_watch((addr), FALSE); } }while(0)



/*
** Clears every breakpoint, watchpoint and anomaly break. Called when a
** client detaches.
*/
void  cu_gdbbrk_reset(void);

/*
** Adds or removes a breakpoint. "addr" is a GDB (byte) code address.
** Returns FALSE if the address is out of range.
*/
boole cu_gdbbrk_bp_set(auint addr, boole ena);

/*
** Adds a watchpoint. "type" is one of CU_GDBBRK_WP_*, "addr" a GDB address
** in either the data or the SPI RAM space. Returns FALSE on failure (no
** free slot, unsupported space and type combination, out of range).
*/
boole cu_gdbbrk_wp_add(auint type, auint addr, auint len);

/*
** Removes a watchpoint. Returns FALSE if no matching watchpoint existed.
*/
boole cu_gdbbrk_wp_del(auint type, auint addr, auint len);

/*
** Requests a single instruction step on the next resume.
*/
void  cu_gdbbrk_step_req(void);

/*
** Requests a halt before the next instruction (Ctrl-C from the client).
*/
void  cu_gdbbrk_halt_req(void);

/*
** Records a halt which something else brought about (the emulator's own
** debugger stopping the CPU), so the client is told the CPU is stopped.
*/
void  cu_gdbbrk_extstop(void);

/*
** Called when execution resumes, so that a breakpoint on the instruction we
** are sitting on does not fire immediately.
*/
void  cu_gdbbrk_resume(void);

/*
** Returns the reason for the last halt, and through "addr" the address it
** relates to (a data address for watchpoints, a GDB code address otherwise).
*/
auint cu_gdbbrk_getstop(auint* addr);

/*
** Returns a human readable description of the last anomaly break, or NULL.
*/
char const* cu_gdbbrk_getanomaly(void);



/*
** Reverse execution replay support. The emulator's own timeline is the
** wrapping cycle counter; instructions are only counted while a replay is
** armed, which is why there is no always-on instruction counter to pay for.
**
** cu_gdbbrk_replay_toc()  halts once cpu_state.cycle reaches "cycle".
** cu_gdbbrk_replay_toi()  halts once "ninstr" instructions have run.
** cu_gdbbrk_replay_off()  disarms, and is implied by any halt.
** cu_gdbbrk_replay_cnt()  instructions run since the replay was armed.
*/
void  cu_gdbbrk_replay_toc(auint cycle);
void  cu_gdbbrk_replay_toi(auint ninstr);
void  cu_gdbbrk_replay_off(void);
auint cu_gdbbrk_replay_cnt(void);


/*
** Break condition evaluation mode, which the reverse execution needs to bend
** during a replay.
**
** NORMAL   -- a break condition halts, which is what a live session wants.
** SUPPRESS -- break conditions are ignored, so a replay can run to a known
**             instruction without tripping over the breakpoint that stopped
**             it in the first place.
** SCAN     -- break conditions are recorded but do not halt, which is how
**             reverse-continue finds the last one before the current point.
*/
#define CU_GDBBRK_MODE_NORMAL   0U
#define CU_GDBBRK_MODE_SUPPRESS 1U
#define CU_GDBBRK_MODE_SCAN     2U

void  cu_gdbbrk_mode(auint mode);

/*
** Returns the break condition recorded in SCAN mode, if there was one:
** through "idx" the instruction index it fired before, through "reason" a
** CU_GDBBRK_ST_* value and through "addr" the related address.
*/
boole cu_gdbbrk_scan_get(auint* idx, auint* reason, auint* addr);

/*
** Enables or disables an anomaly break (CU_GDBBRK_AN_*).
*/
void  cu_gdbbrk_anomaly_set(auint which, boole ena);

/*
** Returns whether an anomaly break is enabled.
*/
boole cu_gdbbrk_anomaly_get(auint which);

/*
** Sets the parameter of a parameterised anomaly break: the cycle threshold
** for CU_GDBBRK_AN_IRQ_LATENCY, the stack floor for
** CU_GDBBRK_AN_STACK_OVERFLOW.
*/
void  cu_gdbbrk_anomaly_par(auint which, auint par);

/*
** Returns the parameter of a parameterised anomaly break.
*/
auint cu_gdbbrk_anomaly_getpar(auint which);

/*
** Reports an anomaly from outside (used for the video overrun condition,
** which is detected at the scanline boundary rather than per instruction).
*/
void  cu_gdbbrk_anomaly_hit(auint which, char const* detail);

/*
** Recomputes cu_gdb_slow and cu_gdb_wp_ena after any change. Called
** internally, exposed for cu_gdb.c's attach and detach paths.
*/
void  cu_gdbbrk_regate(void);

/*
** Fills the SRAM with the poison pattern, so that uninit-read has something
** to detect. Called on reset while that break is armed.
*/
void  cu_gdbbrk_poison(void);

/*
** Enables or disables scanline profiling. While on, the per-instruction hook
** measures the cycle cost of every scanline.
*/
void  cu_gdbbrk_profile_ena(boole ena);

/*
** Returns whether scanline profiling is on.
*/
boole cu_gdbbrk_profile_ison(void);

/*
** Returns profiling data collected since the last clear: the number of
** scanlines seen, their total cycle cost, the worst scanline's cost and its
** pulse number, and the number of scanlines which overran their budget.
*/
void  cu_gdbbrk_profile_get(auint* rows, auint* total, auint* worst,
                            auint* worstrow, auint* overruns);

/*
** Clears the profiling counters.
*/
void  cu_gdbbrk_profile_clear(void);

/*
** Scanline budget in cycles, above which a scanline counts as an overrun.
*/
#define CU_GDBBRK_ROWBUDGET 1820U



#else /* CU_GDB */



#define CU_GDB_EXEC_HOOK(pc) (FALSE)
#define CU_GDB_WATCH_W(addr) do{}while(0)
#define CU_GDB_WATCH_R(addr) do{}while(0)



#endif /* CU_GDB */



#endif
