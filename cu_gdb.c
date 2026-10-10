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
 *  The register packet layout, the address space offsets and the halving of
 *  breakpoint addresses follow tools/uzem/gdbserver.cpp in the Uzebox
 *  repository, which descends from simulavr by Filipe Rinaldi (2009),
 *  originally Theodore Roth and Klaus Rudolph, 2001 - 2003, licensed
 *  GPLv2-or-later. The packet loop, the qSupported handshake and the memory
 *  map follow simavr/sim/sim_gdb.c by Michel Pollet, GPLv3.
 *
 *  Note that neither the socket setup nor the error handling is taken from
 *  uzem: its bind() passes the size of a pointer, and it calls exit() on a
 *  client disconnect, which would take an SDL emulator with unsaved EEPROM
 *  down with it. No path in this file ever calls exit().
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



/* Winsock must come before anything which might pull in windows.h, which SDL
** does on this target. */
#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <windows.h>
#else
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <netinet/tcp.h>
 #include <arpa/inet.h>
 #include <unistd.h>
 #include <fcntl.h>
 #include <errno.h>
#endif

#include "cu_gdb.h"

#ifdef CU_GDB

#include "cu_gdbbrk.h"
#include "cu_gdbmon.h"
#include "cu_gdbovl.h"
#include "cu_avr.h"
#include "cu_spir.h"
#include "cu_sdb.h"
#include "guicore.h"
#include "audio.h"
#include "ginput.h"



#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 typedef SOCKET gdb_sock_t;
 #define GDB_BADSOCK  INVALID_SOCKET
 #define gdb_sockclose(s) closesocket(s)
 #define gdb_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
 typedef int gdb_sock_t;
 #define GDB_BADSOCK  (-1)
 #define gdb_sockclose(s) close(s)
 #define gdb_wouldblock() ((errno == EAGAIN) || (errno == EWOULDBLOCK))
#endif



/* Emulator globals, reached the way cu_avr.c's own peers do */
extern cu_state_cpu_t cpu_state;
extern uint32         cpu_code[32768];
extern auint          video_pulsectr;
extern auint          video_pedge;
extern auint          video_prise;
extern auint          video_cycle;
extern boole          video_rowflag;
extern auint          cycle_next_event;
extern auint          timer1_base;
extern boole          event_it;
extern boole          event_it_enter;
extern auint          event_it_vect;
extern auint          audio_samples[4U];
extern auint          audio_rp;
extern auint          audio_wp;



/* GDB address space bases. Dispatch is by range rather than by masking a 16
** bit offset out, so that the SPI RAM window can exceed 64 KiB and so that a
** read running off the end of a region can be refused instead of wrapping. */
#define GDB_FLASH_BASE   0x000000U
#define GDB_FLASH_SIZE   0x010000U
#define GDB_DATA_BASE    0x800000U
#define GDB_DATA_SIZE    0x001100U
#define GDB_EEPROM_BASE  0x810000U
#define GDB_EEPROM_SIZE  0x000800U
#define GDB_SPIRAM_BASE  0x820000U

/* Signals reported in stop replies */
#define GDB_SIGINT   2U
#define GDB_SIGTRAP  5U

/* Buffer sizes. The advertised PacketSize is half of the receive buffer, so
** that a fully escaped binary payload still fits. */
#define GDB_BUFSZ    16384U
#define GDB_PACKETSZ 4096U



/* Configuration from the command line */
static boole      gdb_wanted   = FALSE;
static auint      gdb_port     = CU_GDB_PORT;
static boole      gdb_waitatt  = FALSE;
static boole      gdb_revena  = FALSE;
static boole      gdb_focuside = FALSE;

/* --gdb-nolimit: start with the 60Hz frame limiter off */
static boole      gdb_nolimit = FALSE;
/* --gdb-show: keep the window visible on a --gdb-nolimit run, which hides it */
static boole      gdb_show    = FALSE;
static boole      gdb_dolog    = FALSE;
static char       gdb_logpath[512] = "cuzebox-gdb.log";
static FILE*      gdb_logf     = NULL;

/* The window which was in front when the emulator started, so a breakpoint can
** hand focus back to it. Captured before the emulator's own window exists. */
#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
static HWND       gdb_idewin   = NULL;
#endif

/* How long a stop has to hold before the focus hand-off fires. A debug adapter
** plants an entry breakpoint of its own and resumes from it within a few ms,
** and that stop is never shown to the user, so handing focus over on it would
** pull them out of the emulator at launch for no reason. */
#define GDB_FOCUS_HOLD_MS 400U
static boole      gdb_focuspend = FALSE;
static auint      gdb_focustick = 0U;

/* Connection */
static gdb_sock_t gdb_listen   = GDB_BADSOCK;
static gdb_sock_t gdb_conn     = GDB_BADSOCK;
static boole      gdb_up       = FALSE;

/* The CPU was stopped by the emulator's own debugger while a client was
** attached, and the client was told (cu_gdb_foreignstop()) */
static boole      gdb_foreign  = FALSE;

/* Called from the hold loop (cu_gdb_setidle()) */
static void       (*gdb_idlehook)(void) = NULL;
static boole      gdb_extended = FALSE;

/* Execution state */
static boole      gdb_halted   = FALSE;
static boole      gdb_stopsent = FALSE;
static boole      gdb_quitreq  = FALSE;
static boole      gdb_instep   = FALSE; /* Resumed for a single step */

/* Receive assembly */
static char       gdb_rx[GDB_BUFSZ];
static auint      gdb_rxlen    = 0U;
static char       gdb_pkt[GDB_BUFSZ];
static auint      gdb_pktlen   = 0U;

/* Transmit, kept for retransmission on a NAK */
static char       gdb_tx[GDB_BUFSZ];
static auint      gdb_txlen    = 0U;

/* Cycle bookkeeping. cpu_state.cycle wraps at 32 bits, so a widened
** accumulator is kept for the monitor commands and the snapshots. */
static uint64_t   gdb_cyc64    = 0U;
static auint      gdb_cycprev  = 0U;
static uint64_t   gdb_cycstop  = 0U;



/* Reverse execution ring buffer */

typedef struct{
 boole          valid;
 uint64_t       cyc64;
 cu_state_cpu_t cpu;
 auint          v_pulsectr;
 auint          v_pedge;
 auint          v_prise;
 auint          v_cycle;
 boole          v_rowflag;
 auint          nextevent;
 auint          t1base;
 boole          e_it;
 boole          e_itenter;
 auint          e_itvect;
 auint          au_smp[4];
 auint          au_rp;
 auint          au_wp;
 cu_state_spir_t spir;
 uint8*         spiram;
 auint          spiramsz;
}gdb_snap_t;

#define GDB_SNAP_DEPTH_MAX 64U

static gdb_snap_t* gdb_snap      = NULL;
static auint       gdb_snapdepth = 8U;
static auint       gdb_snapwrite = 0U;
static auint       gdb_snapcount = 0U;
static uint64_t    gdb_snaplast  = 0U;



static void  gdb_send(char const* data);
static void  gdb_dispatch(void);
static void  gdb_cycle_track(void);



/*
** Logging, opt-in only. uzem opens gdb.log in the working directory
** unconditionally and writes to it on every memory read; that is not done
** here.
*/
static void gdb_log(char const* dir, char const* data, auint len)
{
 auint i;

 if (!gdb_dolog){ return; }
 if (gdb_logf == NULL){
  gdb_logf = fopen(&(gdb_logpath[0]), "w");
  if (gdb_logf == NULL){ gdb_dolog = FALSE; return; }
 }

 fputs(dir, gdb_logf);
 for (i = 0U; i < len; i++){
  if ((data[i] >= 32) && (data[i] < 127)){ fputc(data[i], gdb_logf); }
  else{ fprintf(gdb_logf, "\\x%02X", (unsigned)((uint8)(data[i]))); }
 }
 fputc('\n', gdb_logf);
 fflush(gdb_logf);
}



/*
** Hex helpers.
*/
static char const gdb_hexd[] = "0123456789abcdef";

static asint gdb_unhex1(char c)
{
 if ((c >= '0') && (c <= '9')){ return (asint)(c - '0'); }
 if ((c >= 'a') && (c <= 'f')){ return (asint)(c - 'a') + 10; }
 if ((c >= 'A') && (c <= 'F')){ return (asint)(c - 'A') + 10; }
 return -1;
}

static auint gdb_hexbyte(uint8 val, char* dest)
{
 dest[0] = gdb_hexd[(val >> 4) & 0xFU];
 dest[1] = gdb_hexd[(val     ) & 0xFU];
 return 2U;
}

/*
** Parses a hexadecimal number, advancing the pointer. Returns the number of
** digits consumed.
*/
static auint gdb_hexnum(char const** p, auint* dest)
{
 auint val = 0U;
 auint cnt = 0U;
 asint dig;

 while ((dig = gdb_unhex1(**p)) >= 0){
  val = (val << 4) | (auint)(dig);
  (*p) ++;
  cnt ++;
  if (cnt > 8U){ break; }
 }

 *dest = val;
 return cnt;
}



/*
** Socket teardown, returning the emulator to free running. Never exits.
*/
static void gdb_drop(char const* why)
{
 if (gdb_conn != GDB_BADSOCK){
  gdb_sockclose(gdb_conn);
  gdb_conn = GDB_BADSOCK;
 }

 gdb_rxlen    = 0U;
 gdb_halted   = FALSE;
 gdb_stopsent = FALSE;
 gdb_instep   = FALSE;
 gdb_extended = FALSE;

 cu_gdb_hook_ena = FALSE;
 cu_gdbbrk_reset();

 cu_gdbovl_running(FALSE, gdb_port);

 if (why != NULL){ print_message("GDB: %s, resuming free run\n", why); }
}



/*
** Raw write to the client. A failure drops the connection rather than
** retrying forever.
*/
static void gdb_write(char const* data, auint len)
{
 auint sent = 0U;
 int   res;

 if (gdb_conn == GDB_BADSOCK){ return; }

 while (sent < len){
  res = send(gdb_conn, &(data[sent]), (int)(len - sent), 0);
  if (res > 0){
   sent += (auint)(res);
  }else if ((res < 0) && gdb_wouldblock()){
   /* The kernel buffer is full. Wait for it to drain; a GDB client always
   ** reads, so this resolves. */
   fd_set wfd;
   struct timeval tv;
   FD_ZERO(&wfd);
   FD_SET(gdb_conn, &wfd);
   tv.tv_sec  = 2;
   tv.tv_usec = 0;
   if (select((int)(gdb_conn) + 1, NULL, &wfd, NULL, &tv) <= 0){
    gdb_drop("send timed out");
    return;
   }
  }else{
   gdb_drop("send failed");
   return;
  }
 }
}



/*
** Sends an RSP packet, framing and checksumming it.
*/
static void gdb_send(char const* data)
{
 auint len = (auint)(strlen(data));
 auint i;
 uint8 csum = 0U;

 if ((len + 8U) > GDB_BUFSZ){ len = GDB_BUFSZ - 8U; }

 gdb_tx[0] = '$';
 for (i = 0U; i < len; i++){
  gdb_tx[1U + i] = data[i];
  csum = (uint8)(csum + (uint8)(data[i]));
 }
 gdb_tx[1U + len] = '#';
 (void)gdb_hexbyte(csum, &(gdb_tx[2U + len]));
 gdb_txlen = len + 4U;

 gdb_log("<< ", &(gdb_tx[0]), gdb_txlen);
 gdb_write(&(gdb_tx[0]), gdb_txlen);
}



/*
** Sends a line of text to the client's console. Used by the monitor
** commands, which can produce more output than one packet holds.
*/
void  cu_gdb_out(char const* str)
{
 char  buf[GDB_PACKETSZ];
 auint len = (auint)(strlen(str));
 auint pos = 0U;
 auint i;
 auint n;

 if (gdb_conn == GDB_BADSOCK){
  print_message("%s", str);
  return;
 }

 while (pos < len){
  n = len - pos;
  if (n > ((GDB_PACKETSZ / 2U) - 4U)){ n = (GDB_PACKETSZ / 2U) - 4U; }
  buf[0] = 'O';
  for (i = 0U; i < n; i++){
   (void)gdb_hexbyte((uint8)(str[pos + i]), &(buf[1U + (i * 2U)]));
  }
  buf[1U + (n * 2U)] = 0;
  gdb_send(&(buf[0]));
  pos += n;
 }
}



/*
** Address space dispatch. Fills "space" with a GDB_*_BASE value, "off" with
** the offset inside it and "size" with the size of the region. Returns FALSE
** if the address is in no region at all.
*/
static boole gdb_space(auint addr, auint* space, auint* off, auint* size)
{
 cu_state_spir_t* sr;

 if (addr < (GDB_FLASH_BASE + GDB_FLASH_SIZE)){
  *space = GDB_FLASH_BASE;
  *off   = addr - GDB_FLASH_BASE;
  *size  = GDB_FLASH_SIZE;
  return TRUE;
 }

 if ((addr >= GDB_DATA_BASE) && (addr < (GDB_DATA_BASE + GDB_DATA_SIZE))){
  *space = GDB_DATA_BASE;
  *off   = addr - GDB_DATA_BASE;
  *size  = GDB_DATA_SIZE;
  return TRUE;
 }

 if ((addr >= GDB_EEPROM_BASE) &&
     (addr <  (GDB_EEPROM_BASE + GDB_EEPROM_SIZE))){
  *space = GDB_EEPROM_BASE;
  *off   = addr - GDB_EEPROM_BASE;
  *size  = GDB_EEPROM_SIZE;
  return TRUE;
 }

 sr = cu_spir_get_state();
 if ((addr >= GDB_SPIRAM_BASE) &&
     (addr <  (GDB_SPIRAM_BASE + sr->size))){
  *space = GDB_SPIRAM_BASE;
  *off   = addr - GDB_SPIRAM_BASE;
  *size  = sr->size;
  return TRUE;
 }

 return FALSE;
}



/*
** Reads a byte of the data space. I/O registers are read straight out of
** iors[] rather than through cu_avr_read_io(), so that inspecting UDR0 in a
** debugger does not consume a byte from the emulated ESP8266 and inspecting
** TCNT1L does not disturb the 16 bit latch.
*/
static auint gdb_data_read(auint off)
{
 if (off < 0x0100U){ return cpu_state.iors[off]; }
 return cpu_state.sram[off & 0x0FFFU];
}

static void gdb_data_write(auint off, auint val)
{
 if (off < 0x0100U){ cpu_state.iors[off] = (uint8)(val); }
 else              { cpu_state.sram[off & 0x0FFFU] = (uint8)(val); }
}



/*
** Reads emulator memory into "dest". Returns FALSE if the range is not
** wholly inside one region: a read which spans two address spaces or runs
** past the end of one must be an error, never a silent truncation.
*/
static boole gdb_mem_read(auint addr, auint len, uint8* dest)
{
 auint space;
 auint off;
 auint size;
 auint i;
 cu_state_spir_t* sr;

 if (len == 0U){ return TRUE; }
 if (!gdb_space(addr, &space, &off, &size)){ return FALSE; }
 if (len > (size - off)){ return FALSE; }

 /* Refreshes TCNT1H / TCNT1L from the cycle counter so the timer reads
 ** live rather than stale */
 (void)cu_avr_get_state();

 switch (space){

  case GDB_FLASH_BASE:
   /* crom is a byte array in this emulator, not an array of 16 bit words, so
   ** there is no odd address dance to do here. */
   for (i = 0U; i < len; i++){ dest[i] = cpu_state.crom[off + i]; }
   break;

  case GDB_DATA_BASE:
   for (i = 0U; i < len; i++){ dest[i] = (uint8)(gdb_data_read(off + i)); }
   break;

  case GDB_EEPROM_BASE:
   for (i = 0U; i < len; i++){ dest[i] = cpu_state.eepr[off + i]; }
   break;

  default:
   sr = cu_spir_get_state();
   if (sr->ram == NULL){ return FALSE; }
   for (i = 0U; i < len; i++){ dest[i] = sr->ram[off + i]; }
   break;
 }

 return TRUE;
}



/*
** Writes emulator memory. Same all-or-nothing range rule as the read.
*/
static boole gdb_mem_write(auint addr, auint len, uint8 const* src)
{
 auint space;
 auint off;
 auint size;
 auint i;
 cu_state_spir_t* sr;

 if (len == 0U){ return TRUE; }
 if (!gdb_space(addr, &space, &off, &size)){ return FALSE; }
 if (len > (size - off)){ return FALSE; }

 switch (space){

  case GDB_FLASH_BASE:
   for (i = 0U; i < len; i++){ cpu_state.crom[off + i] = src[i]; }
   cu_avr_crom_update(off, len);
   break;

  case GDB_DATA_BASE:
   for (i = 0U; i < len; i++){ gdb_data_write(off + i, src[i]); }
   /* Raw iors[] writes, then a state resync. Going through
   ** cu_avr_write_io() instead would run the peripheral side effects, so
   ** poking SPDR from a debugger would start an SPI transfer. */
   if (off < 0x0100U){ cu_avr_io_update(); }
   break;

  case GDB_EEPROM_BASE:
   for (i = 0U; i < len; i++){ cpu_state.eepr[off + i] = src[i]; }
   break;

  default:
   sr = cu_spir_get_state();
   if (sr->ram == NULL){ return FALSE; }
   for (i = 0U; i < len; i++){ sr->ram[off + i] = src[i]; }
   break;
 }

 return TRUE;
}



/*
** Builds the 39 byte register block GDB expects for an AVR:
**
**   0 - 31  r0 - r31, one byte each
**   32      SREG
**   33      SP, SPL first
**   34      PC, four bytes little endian, as a *byte* address
*/
static auint gdb_regs_get(uint8* dest)
{
 auint i;
 auint pc = (cpu_state.pc & 0x7FFFU) << 1;

 for (i = 0U; i < 32U; i++){ dest[i] = cpu_state.iors[i]; }
 dest[32] = cpu_state.iors[CU_IO_SREG];
 dest[33] = cpu_state.iors[CU_IO_SPL];
 dest[34] = cpu_state.iors[CU_IO_SPH];
 dest[35] = (uint8)( pc        & 0xFFU);
 dest[36] = (uint8)((pc >>  8) & 0xFFU);
 dest[37] = (uint8)((pc >> 16) & 0xFFU); /* GDB only uses 23 bits of the PC */
 dest[38] = 0U;

 return 39U;
}

static void gdb_regs_set(uint8 const* src)
{
 auint i;
 auint pc;

 for (i = 0U; i < 32U; i++){ cpu_state.iors[i] = src[i]; }
 cpu_state.iors[CU_IO_SREG] = src[32];
 cpu_state.iors[CU_IO_SPL]  = src[33];
 cpu_state.iors[CU_IO_SPH]  = src[34];

 pc = ((auint)(src[35])      ) |
      ((auint)(src[36]) <<  8) |
      ((auint)(src[37]) << 16);
 cpu_state.pc = (pc >> 1) & 0x7FFFU;
}



/*
** Sends a stop reply. T packets only, never a bare S, always carrying SREG,
** SP and PC so GDB does not have to ask for them.
*/
static void gdb_send_stop(void)
{
 char  buf[256];
 auint pos = 0U;
 auint reason;
 auint addr = 0U;
 auint sig  = GDB_SIGTRAP;
 char const* det;

 reason = cu_gdbbrk_getstop(&addr);
 if (reason == CU_GDBBRK_ST_HALT){ sig = GDB_SIGINT; }

 pos += (auint)sprintf(&(buf[pos]), "T%02X", (unsigned)(sig));
 pos += (auint)sprintf(&(buf[pos]), "20:%02X;",
     (unsigned)(cpu_state.iors[CU_IO_SREG]));
 pos += (auint)sprintf(&(buf[pos]), "21:%02X%02X;",
     (unsigned)(cpu_state.iors[CU_IO_SPL]),
     (unsigned)(cpu_state.iors[CU_IO_SPH]));
 {
  /* Masked the same way gdb_regs_get() masks it: cpu_state.pc can carry
  ** stray bits above the 15 used to index 64K of flash, and every other
  ** reader (register reads, the breakpoint bitmap) masks them away before
  ** use, so the stop reply must too or it can report a PC off by a
  ** multiple of 0x10000 bytes. */
  auint pc = (cpu_state.pc & 0x7FFFU) << 1;
  pos += (auint)sprintf(&(buf[pos]), "22:%02X%02X%02X00;",
      (unsigned)((pc      ) & 0xFFU),
      (unsigned)((pc >>  8) & 0xFFU),
      (unsigned)((pc >> 16) & 0xFFU));
 }

 switch (reason){
  case CU_GDBBRK_ST_SWBREAK:
   pos += (auint)sprintf(&(buf[pos]), "swbreak:;");
   break;
  case CU_GDBBRK_ST_HWBREAK:
   pos += (auint)sprintf(&(buf[pos]), "hwbreak:;");
   break;
  case CU_GDBBRK_ST_WATCH:
   pos += (auint)sprintf(&(buf[pos]), "watch:%X;", (unsigned)(addr));
   break;
  case CU_GDBBRK_ST_RWATCH:
   pos += (auint)sprintf(&(buf[pos]), "rwatch:%X;", (unsigned)(addr));
   break;
  case CU_GDBBRK_ST_AWATCH:
   pos += (auint)sprintf(&(buf[pos]), "awatch:%X;", (unsigned)(addr));
   break;
  default:
   break;
 }

 (void)(pos);
 gdb_send(&(buf[0]));

 /* GDB has no stop reason for the Uzebox specific breaks, so the detail goes
 ** to the console as a line of text after the reply. */
 det = cu_gdbbrk_getanomaly();
 if (det != NULL){
  char line[256];
  sprintf(&(line[0]), "cuzebox: break-on: %s\n", det);
  cu_gdb_out(&(line[0]));
 }
}



/*
** Hands the foreground back to the window which was in front when the emulator
** started, so a breakpoint reached while watching the emulator brings the IDE
** up by itself. Only does anything if --gdb-focus-ide asked for it. Called from
** the hold loop once a stop has lasted GDB_FOCUS_HOLD_MS, never straight off
** the halt.
**
** Windows only. It works because the emulator is the foreground process at the
** moment it calls this -- that is the case Windows lets set the foreground
** window. Having alt-tabbed elsewhere in the meantime flashes the task bar
** instead, which is the OS deciding, not a failure to handle.
*/
static void gdb_focus_restore(void)
{
#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 if (!gdb_focuside){ return; }
 if (gdb_idewin == NULL){ return; }
 if (!IsWindow(gdb_idewin)){ return; } /* Closed since it was captured */
 (void)(SetForegroundWindow(gdb_idewin));
#endif
}



/*
** Halts the CPU and tells the client about it.
*/
static void gdb_enter_halt(void)
{
 auint reason;
 auint addr = 0U;

 if (gdb_halted){ return; }

 gdb_halted  = TRUE;
 gdb_instep  = FALSE;
 gdb_cycstop = gdb_cyc64;

 if (!gdb_stopsent){
  gdb_send_stop();
  gdb_stopsent = TRUE;
 }

 reason = cu_gdbbrk_getstop(&addr);
 cu_gdbovl_halted(reason, addr, gdb_conn != GDB_BADSOCK, gdb_port);

 /* Breakpoint, watchpoint and break-on stops only. A completed single step or
 ** an IDE issued pause already has the user in the IDE, and the halts at reset
 ** and at client attach don't come through here at all. Armed rather than done
 ** here: the hold loop fires it once the stop has lasted GDB_FOCUS_HOLD_MS,
 ** which is what separates a stop the user is looking at from one the client
 ** planted and resumes from by itself. */
 if ( (reason >= CU_GDBBRK_ST_SWBREAK) &&
      (reason <= CU_GDBBRK_ST_ANOMALY) ){
  gdb_focuspend = TRUE;
  gdb_focustick = (auint)(SDL_GetTicks());
 }
}



/*
** Resumes the CPU.
*/
static void gdb_resume(boole step)
{
 cu_gdbbrk_mode(CU_GDBBRK_MODE_NORMAL);
 cu_gdbbrk_replay_off();
 if (step){ cu_gdbbrk_step_req(); }
 cu_gdbbrk_resume();
 cu_gdbmon_resumed();
 gdb_halted   = FALSE;
 gdb_stopsent = FALSE;
 gdb_instep   = step;
 gdb_focuspend = FALSE; /* Resumed before it held: not a stop the user saw */

 cu_gdbovl_running(gdb_conn != GDB_BADSOCK, gdb_port);
}



/*
** The memory map. Serving it is optional: GDB copes fine without one and the
** monitor commands still reach the SPI RAM, so if a client ever misbehaves
** on the extra regions the qXfer advertisement can simply be dropped.
*/
static auint gdb_memmap(char* dest, auint destsz)
{
 cu_state_spir_t* sr = cu_spir_get_state();

 return (auint)snprintf(dest, destsz,
     "<?xml version=\"1.0\"?>"
     "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map V1.0//EN\""
     " \"http://sourceware.org/gdb/gdb-memory-map.dtd\">"
     "<memory-map>"
     "<memory type=\"flash\" start=\"0x%X\" length=\"0x%X\">"
     "<property name=\"blocksize\">0x100</property></memory>"
     "<memory type=\"ram\" start=\"0x%X\" length=\"0x%X\"/>"
     "<memory type=\"ram\" start=\"0x%X\" length=\"0x%X\"/>"
     "<memory type=\"ram\" start=\"0x%X\" length=\"0x%X\"/>"
     "</memory-map>",
     (unsigned)(GDB_FLASH_BASE),   (unsigned)(GDB_FLASH_SIZE),
     (unsigned)(GDB_DATA_BASE),    (unsigned)(GDB_DATA_SIZE),
     (unsigned)(GDB_EEPROM_BASE),  (unsigned)(GDB_EEPROM_SIZE),
     (unsigned)(GDB_SPIRAM_BASE),  (unsigned)(sr->size));
}



/*
** Serves a qXfer read window out of a prepared buffer.
*/
static void gdb_xfer_reply(char const* data, auint len,
                           auint off, auint want)
{
 char  buf[GDB_PACKETSZ];
 auint n;

 if (off >= len){
  gdb_send("l");
  return;
 }

 n = len - off;
 if (n > want){ n = want; }
 if (n > (sizeof(buf) - 4U)){ n = sizeof(buf) - 4U; }

 buf[0] = (char)(((off + n) < len) ? 'm' : 'l');
 memcpy(&(buf[1]), &(data[off]), n);
 buf[1U + n] = 0;
 gdb_send(&(buf[0]));
}



/* ---------------------------------------------------------------------- */
/* Snapshots and reverse execution                                        */
/* ---------------------------------------------------------------------- */



/*
** Fills a snapshot from the live emulator state.
*/
static boole gdb_snap_take(gdb_snap_t* s)
{
 cu_state_spir_t* sr = cu_spir_get_state();

 memcpy(&(s->cpu), cu_avr_get_state(), sizeof(cu_state_cpu_t));

 s->cyc64      = gdb_cyc64;
 s->v_pulsectr = video_pulsectr;
 s->v_pedge    = video_pedge;
 s->v_prise    = video_prise;
 s->v_cycle    = video_cycle;
 s->v_rowflag  = video_rowflag;
 s->nextevent  = cycle_next_event;
 s->t1base     = timer1_base;
 s->e_it       = event_it;
 s->e_itenter  = event_it_enter;
 s->e_itvect   = event_it_vect;
 s->au_smp[0]  = audio_samples[0];
 s->au_smp[1]  = audio_samples[1];
 s->au_smp[2]  = audio_samples[2];
 s->au_smp[3]  = audio_samples[3];
 s->au_rp      = audio_rp;
 s->au_wp      = audio_wp;
 s->spir       = *sr;
 s->spir.ram   = NULL; /* The pointer must not survive into a snapshot */

 if ((sr->ram != NULL) && (sr->size != 0U)){
  if (s->spiramsz != sr->size){
   uint8* np = (uint8*)(realloc(s->spiram, sr->size));
   if (np == NULL){ return FALSE; }
   s->spiram   = np;
   s->spiramsz = sr->size;
  }
  memcpy(s->spiram, sr->ram, sr->size);
 }

 s->valid = TRUE;
 return TRUE;
}



/*
** Restores the emulator from a snapshot.
*/
static void gdb_snap_put(gdb_snap_t const* s)
{
 cu_state_cpu_t*  cpu = cu_avr_get_state();
 cu_state_spir_t* sr  = cu_spir_get_state();
 uint8*           ram = sr->ram;
 boole            cromdiff;

 cromdiff = (boole)(memcmp(&(cpu->crom[0]), &(s->cpu.crom[0]), 65536U) != 0);

 memcpy(cpu, &(s->cpu), sizeof(cu_state_cpu_t));

 gdb_cyc64        = s->cyc64;
 gdb_cycprev      = cpu->cycle;
 video_pulsectr   = s->v_pulsectr;
 video_pedge      = s->v_pedge;
 video_prise      = s->v_prise;
 video_cycle      = s->v_cycle;
 video_rowflag    = s->v_rowflag;
 cycle_next_event = s->nextevent;
 timer1_base      = s->t1base;
 event_it         = s->e_it;
 event_it_enter   = s->e_itenter;
 event_it_vect    = s->e_itvect;
 audio_samples[0] = s->au_smp[0];
 audio_samples[1] = s->au_smp[1];
 audio_samples[2] = s->au_smp[2];
 audio_samples[3] = s->au_smp[3];
 audio_rp         = s->au_rp;
 audio_wp         = s->au_wp;

 if ((s->spiram != NULL) && (ram != NULL) && (s->spiramsz == sr->size)){
  memcpy(ram, s->spiram, s->spiramsz);
 }
 *sr     = s->spir;
 sr->ram = ram; /* Keep the live heap buffer, never the saved pointer */
 cu_spir_update();

 /* Only recompile when the flash actually changed: cu_avr_crom_update()
 ** raises the "ROM changed" indicator, which would make main.c write a ROM
 ** dump for a state restore that did not touch the ROM at all. */
 if (cromdiff){
  cu_avr_crom_update(0U, 65536U);
 }
 (void)cu_avr_crom_ischanged(TRUE);

 cu_avr_io_update();
 (void)cu_avr_eeprom_ischanged(TRUE);
}



/*
** Allocates the reverse execution ring buffer.
*/
static boole gdb_snap_alloc(void)
{
 auint i;

 if (gdb_snapdepth == 0U){ return FALSE; }
 if (gdb_snap != NULL){ return TRUE; }

 gdb_snap = (gdb_snap_t*)(calloc(gdb_snapdepth, sizeof(gdb_snap_t)));
 if (gdb_snap == NULL){ return FALSE; }

 for (i = 0U; i < gdb_snapdepth; i++){
  gdb_snap[i].valid   = FALSE;
  gdb_snap[i].spiram  = NULL;
  gdb_snap[i].spiramsz = 0U;
 }
 gdb_snapwrite = 0U;
 gdb_snapcount = 0U;

 return TRUE;
}


static void gdb_snap_free(void)
{
 auint i;

 if (gdb_snap == NULL){ return; }
 for (i = 0U; i < gdb_snapdepth; i++){
  free(gdb_snap[i].spiram);
 }
 free(gdb_snap);
 gdb_snap      = NULL;
 gdb_snapcount = 0U;
 gdb_snapwrite = 0U;
 gdb_snaplast  = 0U;
}


void  cu_gdb_reverse_setdepth(auint frames)
{
 if (frames > GDB_SNAP_DEPTH_MAX){ frames = GDB_SNAP_DEPTH_MAX; }
 gdb_snap_free();
 gdb_snapdepth = frames;
}


auint cu_gdb_reverse_getdepth(void)
{
 return gdb_snapdepth;
}



/*
** Takes a periodic snapshot, at most once per emulated frame.
*/
static void gdb_snap_periodic(void)
{
 if (gdb_snapdepth == 0U){ return; }
 if (!gdb_snap_alloc()){ return; }

 if ( (gdb_snapcount != 0U) &&
      ((gdb_cyc64 - gdb_snaplast) < (uint64_t)(CU_GDB_FRAMECY)) ){ return; }

 if (gdb_snap_take(&(gdb_snap[gdb_snapwrite]))){
  gdb_snaplast = gdb_cyc64;
  gdb_snapwrite = (gdb_snapwrite + 1U) % gdb_snapdepth;
  if (gdb_snapcount < gdb_snapdepth){ gdb_snapcount ++; }
 }
}



/*
** Snapshots for others, opaque to them.
*/
void* cu_gdb_snap_new(void)
{
 return calloc(1U, sizeof(gdb_snap_t));
}

boole cu_gdb_snap_take(void* s)
{
 gdb_cycle_track();
 return gdb_snap_take((gdb_snap_t*)(s));
}

void  cu_gdb_snap_put(void const* s)
{
 gdb_snap_put((gdb_snap_t const*)(s));
}

void  cu_gdb_snap_delete(void* s)
{
 if (s == NULL){ return; }
 free(((gdb_snap_t*)(s))->spiram);
 free(s);
}



/*
** Finds the newest snapshot strictly before the given cycle count.
*/
static gdb_snap_t* gdb_snap_before(uint64_t cyc)
{
 auint       i;
 auint       idx;
 gdb_snap_t* best = NULL;

 for (i = 0U; i < gdb_snapcount; i++){
  idx = (gdb_snapwrite + gdb_snapdepth - 1U - i) % gdb_snapdepth;
  if (!gdb_snap[idx].valid){ continue; }
  if (gdb_snap[idx].cyc64 < cyc){
   best = &(gdb_snap[idx]);
   break;
  }
 }

 return best;
}



/*
** Runs the emulator until a break condition fires, with a safety bound so a
** confused replay cannot wedge the emulator.
*/
static boole gdb_run_until_break(auint maxrows)
{
 auint rows = 0U;
 auint dummy;

 while (rows < maxrows){
  (void)cu_avr_run();
  rows ++;
  if (cu_gdbbrk_getstop(&dummy) != CU_GDBBRK_ST_NONE){ return TRUE; }
 }

 return FALSE;
}



/*
** Reverse step and reverse continue. Both work by restoring the newest
** snapshot before the current position and replaying forward, which is only
** affordable because CUzeBox is deterministic.
*/
static boole gdb_reverse(boole tobreak)
{
 gdb_snap_t* s;
 auint       target;
 auint       n;
 auint       idx    = 0U;
 auint       reason = 0U;
 auint       addr   = 0U;
 auint       rowcap;

 if (gdb_snapdepth == 0U){ return FALSE; }
 if (gdb_snap == NULL){ return FALSE; }

 s = gdb_snap_before(gdb_cyc64);
 if (s == NULL){ return FALSE; }

 target = cpu_state.cycle;
 /* Each snapshot is at most a frame back, and a scanline is at least a few
 ** cycles, so this is a generous bound on the replay. */
 rowcap = (auint)((gdb_cyc64 - s->cyc64) / 8U) + 1024U;

 /* Pass one: replay to where we are now, counting instructions, and (for a
 ** reverse continue) recording the last break condition on the way. */

 gdb_snap_put(s);
 cu_gdbbrk_mode(tobreak ? CU_GDBBRK_MODE_SCAN : CU_GDBBRK_MODE_SUPPRESS);
 cu_gdbbrk_resume();
 cu_gdbbrk_replay_toc(target);
 if (!gdb_run_until_break(rowcap)){
  cu_gdbbrk_mode(CU_GDBBRK_MODE_NORMAL);
  cu_gdbbrk_replay_off();
  return FALSE;
 }
 n = cu_gdbbrk_replay_cnt();

 if (tobreak){
  if (!cu_gdbbrk_scan_get(&idx, &reason, &addr)){
   /* Nothing to go back to. Land on the snapshot itself, which is what GDB
   ** shows as the start of the recorded history. */
   idx = 0U;
  }
 }else{
  if (n == 0U){ idx = 0U; }
  else        { idx = n - 1U; }
 }

 /* Pass two: replay again, stopping at the instruction found. */

 gdb_snap_put(s);
 cu_gdbbrk_mode(CU_GDBBRK_MODE_SUPPRESS);
 cu_gdbbrk_resume();
 cu_gdbbrk_replay_toi(idx);
 if (!gdb_run_until_break(rowcap)){
  cu_gdbbrk_mode(CU_GDBBRK_MODE_NORMAL);
  cu_gdbbrk_replay_off();
  return FALSE;
 }

 cu_gdbbrk_mode(CU_GDBBRK_MODE_NORMAL);
 cu_gdbbrk_replay_off();
 gdb_cycle_track();
 return TRUE;
}



/* ---------------------------------------------------------------------- */
/* Save states                                                            */
/* ---------------------------------------------------------------------- */

#define GDB_STATE_MAGIC "CUZEBOXSTATE001\n"


boole cu_gdb_state_save(char const* path, char* err, auint errsz)
{
 FILE*       f;
 gdb_snap_t  s;
 boole       ok = TRUE;

 memset(&s, 0, sizeof(s));
 if (!gdb_snap_take(&s)){
  snprintf(err, errsz, "out of memory");
  return FALSE;
 }

 f = fopen(path, "wb");
 if (f == NULL){
  snprintf(err, errsz, "cannot open %s for writing", path);
  free(s.spiram);
  return FALSE;
 }

 if (fwrite(GDB_STATE_MAGIC, 1U, 16U, f) != 16U){ ok = FALSE; }
 if (fwrite(&(s.cyc64), 1U, sizeof(s.cyc64), f) != sizeof(s.cyc64)){ ok = FALSE; }
 if (fwrite(&(s.cpu), 1U, sizeof(s.cpu), f) != sizeof(s.cpu)){ ok = FALSE; }
 if (fwrite(&(s.v_pulsectr), 1U, sizeof(auint) * 4U, f) !=
     (sizeof(auint) * 4U)){ ok = FALSE; }
 if (fwrite(&(s.v_rowflag), 1U, sizeof(boole), f) != sizeof(boole)){ ok = FALSE; }
 if (fwrite(&(s.nextevent), 1U, sizeof(auint) * 2U, f) !=
     (sizeof(auint) * 2U)){ ok = FALSE; }
 if (fwrite(&(s.e_it), 1U, sizeof(boole) * 2U, f) !=
     (sizeof(boole) * 2U)){ ok = FALSE; }
 if (fwrite(&(s.e_itvect), 1U, sizeof(auint), f) != sizeof(auint)){ ok = FALSE; }
 if (fwrite(&(s.au_smp[0]), 1U, sizeof(auint) * 6U, f) !=
     (sizeof(auint) * 6U)){ ok = FALSE; }
 if (fwrite(&(s.spir), 1U, sizeof(s.spir), f) != sizeof(s.spir)){ ok = FALSE; }
 if (fwrite(&(s.spiramsz), 1U, sizeof(s.spiramsz), f) !=
     sizeof(s.spiramsz)){ ok = FALSE; }
 if (s.spiramsz != 0U){
  if (fwrite(s.spiram, 1U, s.spiramsz, f) != s.spiramsz){ ok = FALSE; }
 }

 fclose(f);
 free(s.spiram);

 if (!ok){ snprintf(err, errsz, "write to %s failed", path); }
 return ok;
}


boole cu_gdb_state_load(char const* path, char* err, auint errsz)
{
 FILE*       f;
 gdb_snap_t  s;
 char        magic[16];
 boole       ok = TRUE;

 memset(&s, 0, sizeof(s));

 f = fopen(path, "rb");
 if (f == NULL){
  snprintf(err, errsz, "cannot open %s", path);
  return FALSE;
 }

 if (fread(&(magic[0]), 1U, 16U, f) != 16U){ ok = FALSE; }
 if (ok && (memcmp(&(magic[0]), GDB_STATE_MAGIC, 16U) != 0)){
  fclose(f);
  snprintf(err, errsz, "%s is not a CUzeBox save state", path);
  return FALSE;
 }

 if (fread(&(s.cyc64), 1U, sizeof(s.cyc64), f) != sizeof(s.cyc64)){ ok = FALSE; }
 if (fread(&(s.cpu), 1U, sizeof(s.cpu), f) != sizeof(s.cpu)){ ok = FALSE; }
 if (fread(&(s.v_pulsectr), 1U, sizeof(auint) * 4U, f) !=
     (sizeof(auint) * 4U)){ ok = FALSE; }
 if (fread(&(s.v_rowflag), 1U, sizeof(boole), f) != sizeof(boole)){ ok = FALSE; }
 if (fread(&(s.nextevent), 1U, sizeof(auint) * 2U, f) !=
     (sizeof(auint) * 2U)){ ok = FALSE; }
 if (fread(&(s.e_it), 1U, sizeof(boole) * 2U, f) !=
     (sizeof(boole) * 2U)){ ok = FALSE; }
 if (fread(&(s.e_itvect), 1U, sizeof(auint), f) != sizeof(auint)){ ok = FALSE; }
 if (fread(&(s.au_smp[0]), 1U, sizeof(auint) * 6U, f) !=
     (sizeof(auint) * 6U)){ ok = FALSE; }
 if (fread(&(s.spir), 1U, sizeof(s.spir), f) != sizeof(s.spir)){ ok = FALSE; }
 if (fread(&(s.spiramsz), 1U, sizeof(s.spiramsz), f) !=
     sizeof(s.spiramsz)){ ok = FALSE; }

 if (ok && (s.spiramsz != 0U)){
  if (s.spiramsz > MAX_SPI_RAM_SIZE){
   ok = FALSE;
  }else{
   s.spiram = (uint8*)(malloc(s.spiramsz));
   if (s.spiram == NULL){ ok = FALSE; }
   else if (fread(s.spiram, 1U, s.spiramsz, f) != s.spiramsz){ ok = FALSE; }
   else{}
  }
 }

 fclose(f);

 if (!ok){
  free(s.spiram);
  snprintf(err, errsz, "%s is truncated or corrupt", path);
  return FALSE;
 }

 s.spir.ram = NULL;
 gdb_snap_put(&s);
 free(s.spiram);

 /* The recorded history no longer describes the timeline we are on */
 gdb_snap_free();

 return TRUE;
}



/* ---------------------------------------------------------------------- */
/* Packet dispatch                                                        */
/* ---------------------------------------------------------------------- */



/*
** Decodes an RSP binary payload in place (the "}" escape). Returns the
** decoded length.
*/
static auint gdb_unescape(char* data, auint len)
{
 auint i;
 auint o = 0U;

 for (i = 0U; i < len; i++){
  if ((data[i] == '}') && ((i + 1U) < len)){
   i ++;
   data[o] = (char)(data[i] ^ 0x20);
  }else{
   data[o] = data[i];
  }
  o ++;
 }

 return o;
}



/*
** Handles a "v" packet.
*/
static void gdb_do_v(char* p, auint len)
{
 char* colon;
 auint addr;
 auint n;
 char const* cp;

 if (strncmp(p, "vCont?", 6U) == 0){
  gdb_send("vCont;c;C;s;S");
  return;
 }

 if (strncmp(p, "vCont;", 6U) == 0){
  /* Only the first action is honoured; there is one thread. */
  char act = p[6];
  if ((act == 's') || (act == 'S')){ gdb_resume(TRUE); }
  else                             { gdb_resume(FALSE); }
  return;
 }

 if (strncmp(p, "vMustReplyEmpty", 15U) == 0){
  gdb_send("");
  return;
 }

 if (strncmp(p, "vFlashErase:", 12U) == 0){
  cp = p + 12;
  if (gdb_hexnum(&cp, &addr) == 0U){ gdb_send("E01"); return; }
  if (*cp != ','){ gdb_send("E01"); return; }
  cp ++;
  if (gdb_hexnum(&cp, &n) == 0U){ gdb_send("E01"); return; }
  if ((addr >= GDB_FLASH_SIZE) || (n > (GDB_FLASH_SIZE - addr))){
   gdb_send("E01");
   return;
  }
  memset(&(cpu_state.crom[addr]), 0xFF, n);
  gdb_send("OK");
  return;
 }

 if (strncmp(p, "vFlashWrite:", 12U) == 0){
  cp = p + 12;
  if (gdb_hexnum(&cp, &addr) == 0U){ gdb_send("E01"); return; }
  if (*cp != ':'){ gdb_send("E01"); return; }
  colon = (char*)(cp) + 1;
  n = gdb_unescape(colon, len - (auint)(colon - p));
  if ((addr >= GDB_FLASH_SIZE) || (n > (GDB_FLASH_SIZE - addr))){
   gdb_send("E01");
   return;
  }
  memcpy(&(cpu_state.crom[addr]), colon, n);
  gdb_send("OK");
  return;
 }

 if (strncmp(p, "vFlashDone", 10U) == 0){
  cu_avr_crom_update(0U, GDB_FLASH_SIZE);
  gdb_send("OK");
  return;
 }

 if (strncmp(p, "vKill", 5U) == 0){
  gdb_send("OK");
  gdb_drop("client sent vKill");
  return;
 }

 gdb_send(""); /* Unknown v packet */
}



/*
** Handles a "q" or "Q" packet.
*/
static void gdb_do_q(char* p, auint len)
{
 char        buf[GDB_PACKETSZ];
 char        cmd[GDB_PACKETSZ];
 char const* cp;
 auint       off;
 auint       want;
 auint       n;
 asint       hi;
 asint       lo;

 (void)(len);

 if (strncmp(p, "qSupported", 10U) == 0){
  /* ReverseStep+;ReverseContinue+ is left out unless explicitly requested
  ** (--gdb-reverse). The Microchip AVR toolchain's avr-gdb 7.8 hits an
  ** internal error ("to_execution_direction must be implemented for
  ** reverse async") the moment it advertises reverse support to a remote
  ** target and then tries to step, taking every command after it down with
  ** it. A newer gdb-avr handles it fine, so this is opt-in rather than
  ** removed outright. */
  snprintf(&(buf[0]), sizeof(buf),
      "PacketSize=%X;qXfer:memory-map:read+;swbreak+;hwbreak+%s",
      (unsigned)(GDB_PACKETSZ),
      gdb_revena ? ";ReverseStep+;ReverseContinue+" : "");
  gdb_send(&(buf[0]));
  return;
 }

 if (strncmp(p, "qXfer:memory-map:read::", 23U) == 0){
  cp = p + 23;
  if (gdb_hexnum(&cp, &off) == 0U){ gdb_send("E01"); return; }
  if (*cp != ','){ gdb_send("E01"); return; }
  cp ++;
  if (gdb_hexnum(&cp, &want) == 0U){ gdb_send("E01"); return; }
  n = gdb_memmap(&(cmd[0]), sizeof(cmd));
  gdb_xfer_reply(&(cmd[0]), n, off, want);
  return;
 }

 if (strncmp(p, "qRcmd,", 6U) == 0){
  cp = p + 6;
  n  = 0U;
  while ((cp[0] != 0) && (cp[1] != 0) && (n < (sizeof(cmd) - 1U))){
   hi = gdb_unhex1(cp[0]);
   lo = gdb_unhex1(cp[1]);
   if ((hi < 0) || (lo < 0)){ break; }
   cmd[n] = (char)((hi << 4) | lo);
   n ++;
   cp += 2;
  }
  cmd[n] = 0;
  cu_gdbmon_exec(&(cmd[0]));
  gdb_send("OK");
  return;
 }

 if (strncmp(p, "qAttached", 9U) == 0){ gdb_send("1");   return; }
 if (strncmp(p, "qC",        2U) == 0){ gdb_send("QC1"); return; }
 if (strncmp(p, "qSymbol",   7U) == 0){ gdb_send("OK");  return; }
 if (strncmp(p, "qfThreadInfo", 12U) == 0){ gdb_send("m1"); return; }
 if (strncmp(p, "qsThreadInfo", 12U) == 0){ gdb_send("l");  return; }

 /* qOffsets is deliberately not answered. simavr had to remove its
 ** Text=0;Data=800000;Bss=800000 reply to fix issue #150, and answering it
 ** makes GDB relocate symbols that are already absolute. */

 gdb_send("");
}



/*
** Handles a Z (insert) or z (remove) packet.
*/
static void gdb_do_z(char* p, boole insert)
{
 char const* cp = p + 1;
 auint       type;
 auint       addr;
 auint       kind;

 if (gdb_hexnum(&cp, &type) == 0U){ gdb_send("E01"); return; }
 if (*cp != ','){ gdb_send("E01"); return; }
 cp ++;
 if (gdb_hexnum(&cp, &addr) == 0U){ gdb_send("E01"); return; }
 if (*cp != ','){ gdb_send("E01"); return; }
 cp ++;
 (void)gdb_hexnum(&cp, &kind);

 switch (type){

  case 0U: /* Software breakpoint */
  case 1U: /* Hardware breakpoint. Same bitmap: flash is never patched with
           ** BREAK opcodes, because Uzebox games read flash for tile and
           ** sprite data and a patched opcode would corrupt graphics. */
   if (!cu_gdbbrk_bp_set(addr, insert)){ gdb_send("E01"); return; }
   cu_gdbbrk_regate();
   gdb_send("OK");
   return;

  case 2U: /* Write watchpoint */
  case 3U: /* Read watchpoint */
  case 4U: /* Access watchpoint */
   if (kind == 0U){ kind = 1U; }
   if (insert){
    if (!cu_gdbbrk_wp_add(type, addr, kind)){ gdb_send("E01"); return; }
   }else{
    if (!cu_gdbbrk_wp_del(type, addr, kind)){ gdb_send("E01"); return; }
   }
   gdb_send("OK");
   return;

  default:
   gdb_send("");
   return;
 }
}



/*
** Dispatches one complete packet.
*/
static void gdb_dispatch(void)
{
 char*       p   = &(gdb_pkt[0]);
 auint       len = gdb_pktlen;
 char const* cp;
 auint       addr;
 auint       n;
 auint       i;
 asint       hi;
 asint       lo;
 uint8       mem[GDB_PACKETSZ];
 char        buf[GDB_PACKETSZ];

 if (len == 0U){ gdb_send(""); return; }

 switch (p[0]){

  case '?':
   /* GDB asks why we stopped. If the CPU is running, report a plain trap so
   ** that "target remote" lands in a defined state. */
   gdb_send_stop();
   return;

  case '!':
   gdb_extended = TRUE;
   gdb_send("OK");
   return;

  case 'g':
   n = gdb_regs_get(&(mem[0]));
   for (i = 0U; i < n; i++){ (void)gdb_hexbyte(mem[i], &(buf[i * 2U])); }
   buf[n * 2U] = 0;
   gdb_send(&(buf[0]));
   return;

  case 'G':
   cp = p + 1;
   for (i = 0U; i < 39U; i++){
    hi = gdb_unhex1(cp[0]);
    lo = gdb_unhex1(cp[1]);
    if ((hi < 0) || (lo < 0)){ gdb_send("E01"); return; }
    mem[i] = (uint8)((hi << 4) | lo);
    cp += 2;
   }
   gdb_regs_set(&(mem[0]));
   gdb_send("OK");
   return;

  case 'p':
   cp = p + 1;
   if (gdb_hexnum(&cp, &n) == 0U){ gdb_send("E01"); return; }
   (void)gdb_regs_get(&(mem[0]));
   if       (n < 32U){
    (void)gdb_hexbyte(mem[n], &(buf[0]));
    buf[2] = 0;
   }else if (n == 32U){
    (void)gdb_hexbyte(mem[32], &(buf[0]));
    buf[2] = 0;
   }else if (n == 33U){
    (void)gdb_hexbyte(mem[33], &(buf[0]));
    (void)gdb_hexbyte(mem[34], &(buf[2]));
    buf[4] = 0;
   }else if (n == 34U){
    (void)gdb_hexbyte(mem[35], &(buf[0]));
    (void)gdb_hexbyte(mem[36], &(buf[2]));
    (void)gdb_hexbyte(mem[37], &(buf[4]));
    (void)gdb_hexbyte(mem[38], &(buf[6]));
    buf[8] = 0;
   }else{
    gdb_send("E01");
    return;
   }
   gdb_send(&(buf[0]));
   return;

  case 'P':
   cp = p + 1;
   if (gdb_hexnum(&cp, &n) == 0U){ gdb_send("E01"); return; }
   if (*cp != '='){ gdb_send("E01"); return; }
   cp ++;
   (void)gdb_regs_get(&(mem[0]));
   i = 0U;
   while ((cp[0] != 0) && (cp[1] != 0) && (i < 4U)){
    hi = gdb_unhex1(cp[0]);
    lo = gdb_unhex1(cp[1]);
    if ((hi < 0) || (lo < 0)){ break; }
    buf[i] = (char)((hi << 4) | lo);
    i ++;
    cp += 2;
   }
   if       (n < 33U){
    if (i < 1U){ gdb_send("E01"); return; }
    mem[n] = (uint8)(buf[0]);
   }else if (n == 33U){
    if (i < 2U){ gdb_send("E01"); return; }
    mem[33] = (uint8)(buf[0]);
    mem[34] = (uint8)(buf[1]);
   }else if (n == 34U){
    if (i < 4U){ gdb_send("E01"); return; }
    mem[35] = (uint8)(buf[0]);
    mem[36] = (uint8)(buf[1]);
    mem[37] = (uint8)(buf[2]);
    mem[38] = (uint8)(buf[3]);
   }else{
    gdb_send("E01");
    return;
   }
   gdb_regs_set(&(mem[0]));
   gdb_send("OK");
   return;

  case 'm':
   cp = p + 1;
   if (gdb_hexnum(&cp, &addr) == 0U){ gdb_send("E01"); return; }
   if (*cp != ','){ gdb_send("E01"); return; }
   cp ++;
   if (gdb_hexnum(&cp, &n) == 0U){ gdb_send("E01"); return; }
   if (n > ((GDB_PACKETSZ / 2U) - 4U)){ n = (GDB_PACKETSZ / 2U) - 4U; }
   if (!gdb_mem_read(addr, n, &(mem[0]))){ gdb_send("E01"); return; }
   for (i = 0U; i < n; i++){ (void)gdb_hexbyte(mem[i], &(buf[i * 2U])); }
   buf[n * 2U] = 0;
   gdb_send(&(buf[0]));
   return;

  case 'M':
   cp = p + 1;
   if (gdb_hexnum(&cp, &addr) == 0U){ gdb_send("E01"); return; }
   if (*cp != ','){ gdb_send("E01"); return; }
   cp ++;
   if (gdb_hexnum(&cp, &n) == 0U){ gdb_send("E01"); return; }
   if (*cp != ':'){ gdb_send("E01"); return; }
   cp ++;
   if (n > sizeof(mem)){ gdb_send("E01"); return; }
   for (i = 0U; i < n; i++){
    hi = gdb_unhex1(cp[0]);
    lo = gdb_unhex1(cp[1]);
    if ((hi < 0) || (lo < 0)){ gdb_send("E01"); return; }
    mem[i] = (uint8)((hi << 4) | lo);
    cp += 2;
   }
   if (!gdb_mem_write(addr, n, &(mem[0]))){ gdb_send("E01"); return; }
   gdb_send("OK");
   return;

  case 'X':
   cp = p + 1;
   if (gdb_hexnum(&cp, &addr) == 0U){ gdb_send("E01"); return; }
   if (*cp != ','){ gdb_send("E01"); return; }
   cp ++;
   if (gdb_hexnum(&cp, &n) == 0U){ gdb_send("E01"); return; }
   if (*cp != ':'){ gdb_send("E01"); return; }
   cp ++;
   {
    char* bin = (char*)(cp);
    auint blen = gdb_unescape(bin, len - (auint)(bin - p));
    if (blen < n){ n = blen; }
    if (n == 0U){ gdb_send("OK"); return; } /* Probe for X support */
    if (!gdb_mem_write(addr, n, (uint8 const*)(bin))){
     gdb_send("E01");
     return;
    }
   }
   gdb_send("OK");
   return;

  case 'c':
  case 'C':
   gdb_resume(FALSE);
   return;

  case 's':
  case 'S':
   gdb_resume(TRUE);
   return;

  case 'b':
   /* Reverse execution. bs is a reverse step, bc a reverse continue. Not
   ** reachable unless --gdb-reverse was given, since qSupported did not
   ** advertise it, but refuse explicitly in case a client sends it anyway
   ** (some issue these without checking qSupported first). */
   if (!gdb_revena){ gdb_send(""); return; }
   if (len >= 2U){
    if (gdb_reverse((boole)(p[1] == 'c'))){
     gdb_send_stop();
    }else{
     cu_gdb_out("cuzebox: no recorded history to reverse into\n");
     gdb_send_stop();
    }
    return;
   }
   gdb_send("");
   return;

  case 'Z':
   gdb_do_z(p, TRUE);
   return;

  case 'z':
   gdb_do_z(p, FALSE);
   return;

  case 'H':
   gdb_send("OK");
   return;

  case 'T':
   gdb_send("OK");
   return;

  case 'q':
  case 'Q':
   gdb_do_q(p, len);
   return;

  case 'v':
   gdb_do_v(p, len);
   return;

  case 'D':
   gdb_send("OK");
   gdb_drop("client detached");
   return;

  case 'k':
   /* Kill. The emulator is not the client's to kill: it detaches and keeps
   ** running, so that a debugger going away cannot take an SDL window with
   ** unsaved EEPROM down with it. */
   gdb_drop("client sent kill");
   return;

  case 'R':
   cu_gdb_reset_target();
   /* R has no reply */
   return;

  default:
   gdb_send("");
   return;
 }
}



/* ---------------------------------------------------------------------- */
/* Socket servicing                                                       */
/* ---------------------------------------------------------------------- */



/*
** Consumes as many complete packets as the receive buffer holds.
*/
static void gdb_consume(void)
{
 auint pos = 0U;
 auint i;
 auint end;
 uint8 csum;
 asint hi;
 asint lo;

 while (pos < gdb_rxlen){

  if (gdb_rx[pos] == '+'){ pos ++; continue; }

  if (gdb_rx[pos] == '-'){ /* Retransmit the last packet */
   pos ++;
   if (gdb_txlen != 0U){ gdb_write(&(gdb_tx[0]), gdb_txlen); }
   continue;
  }

  if (gdb_rx[pos] == 0x03){ /* Ctrl-C */
   pos ++;
   cu_gdbbrk_halt_req();
   continue;
  }

  if (gdb_rx[pos] != '$'){ pos ++; continue; } /* Garbage */

  /* Find the terminator. The checksum needs two more bytes after it. */

  end = pos + 1U;
  while ((end < gdb_rxlen) && (gdb_rx[end] != '#')){ end ++; }
  if ((end + 2U) >= gdb_rxlen){ break; } /* Incomplete, wait for more */

  csum = 0U;
  for (i = pos + 1U; i < end; i++){ csum = (uint8)(csum + (uint8)(gdb_rx[i])); }

  hi = gdb_unhex1(gdb_rx[end + 1U]);
  lo = gdb_unhex1(gdb_rx[end + 2U]);

  if ((hi < 0) || (lo < 0) || (csum != (uint8)((hi << 4) | lo))){
   gdb_write("-", 1U);
   pos = end + 3U;
   continue;
  }

  gdb_write("+", 1U);

  gdb_pktlen = end - (pos + 1U);
  if (gdb_pktlen >= GDB_BUFSZ){ gdb_pktlen = GDB_BUFSZ - 1U; }
  memcpy(&(gdb_pkt[0]), &(gdb_rx[pos + 1U]), gdb_pktlen);
  gdb_pkt[gdb_pktlen] = 0;

  gdb_log(">> ", &(gdb_pkt[0]), gdb_pktlen);

  pos = end + 3U;

  /* The dispatch can drop the connection, which resets gdb_rxlen; anything
  ** still buffered belongs to a session that no longer exists. */
  {
   auint keep = gdb_rxlen - pos;
   memmove(&(gdb_rx[0]), &(gdb_rx[pos]), keep);
   gdb_rxlen = keep;
   pos = 0U;
  }

  gdb_dispatch();

  if (gdb_conn == GDB_BADSOCK){ return; }
 }

 if (pos != 0U){
  memmove(&(gdb_rx[0]), &(gdb_rx[pos]), gdb_rxlen - pos);
  gdb_rxlen -= pos;
 }
}



/*
** Accepts a pending connection. Only one client at a time is supported; a
** second one is accepted and closed straight away so it gets a clean refusal
** rather than hanging in the backlog.
*/
static void gdb_accept(void)
{
 gdb_sock_t s;
 int        one = 1;

 if (gdb_listen == GDB_BADSOCK){ return; }

 s = accept(gdb_listen, NULL, NULL);
 if (s == GDB_BADSOCK){ return; }

 if (gdb_conn != GDB_BADSOCK){
  /* One client at a time. A second one is closed straight away so it gets a
  ** clean refusal instead of hanging in the backlog. */
  gdb_sockclose(s);
  print_message("GDB: refused a second client, one is already attached\n");
  return;
 }

 (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char const*)(&one),
     (int)(sizeof(one)));

#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 { u_long nb = 1UL; (void)ioctlsocket(s, FIONBIO, &nb); }
#else
 (void)fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

 gdb_conn     = s;
 gdb_rxlen    = 0U;
 gdb_txlen    = 0U;
 gdb_extended = FALSE;

 cu_gdbbrk_reset();
 cu_gdb_hook_ena = TRUE;
 cu_gdbbrk_regate();

 /* A fresh client expects the target stopped, but it has to ask: GDB opens
 ** with qSupported and then "?", and an unsolicited stop reply before that
 ** confuses it. So halt quietly and let the "?" produce the reply. */
 gdb_halted   = TRUE;
 gdb_stopsent = TRUE;
 gdb_instep   = FALSE;
 gdb_cycstop  = gdb_cyc64;

 cu_gdbovl_halted(CU_GDBBRK_ST_NONE, 0U, TRUE, gdb_port);

 print_message("GDB: client attached on port %u\n", (unsigned)(gdb_port));
}



/*
** Reads whatever is waiting on the connection. Returns FALSE if the client
** went away.
*/
static boole gdb_receive(void)
{
 int res;

 if (gdb_conn == GDB_BADSOCK){ return FALSE; }
 if (gdb_rxlen >= (GDB_BUFSZ - 1U)){
  /* A packet larger than the buffer cannot be handled. Rather than
  ** "handling" it by printing a message and writing on regardless, the
  ** connection is dropped. */
  gdb_drop("receive buffer overflow");
  return FALSE;
 }

 res = recv(gdb_conn, &(gdb_rx[gdb_rxlen]),
     (int)(GDB_BUFSZ - 1U - gdb_rxlen), 0);

 if (res > 0){
  gdb_rxlen += (auint)(res);
  return TRUE;
 }

 if (res == 0){
  gdb_drop("client closed the connection");
  return FALSE;
 }

 if (!gdb_wouldblock()){
  gdb_drop("receive failed");
  return FALSE;
 }

 return TRUE;
}



/*
** Waits up to "ms" milliseconds for something to arrive on either socket,
** then services it.
*/
static void gdb_service(auint ms)
{
 fd_set         rfd;
 struct timeval tv;
 gdb_sock_t     hi = 0;
 int            res;

 if (!gdb_up){ return; }

 FD_ZERO(&rfd);
 if (gdb_listen != GDB_BADSOCK){
  FD_SET(gdb_listen, &rfd);
  if (gdb_listen > hi){ hi = gdb_listen; }
 }
 if (gdb_conn != GDB_BADSOCK){
  FD_SET(gdb_conn, &rfd);
  if (gdb_conn > hi){ hi = gdb_conn; }
 }

 tv.tv_sec  = (long)(ms / 1000U);
 tv.tv_usec = (long)((ms % 1000U) * 1000U);

 res = select((int)(hi) + 1, &rfd, NULL, NULL, &tv);
 if (res <= 0){ return; }

 if ((gdb_listen != GDB_BADSOCK) && FD_ISSET(gdb_listen, &rfd)){
  gdb_accept();
 }

 if ((gdb_conn != GDB_BADSOCK) && FD_ISSET(gdb_conn, &rfd)){
  if (gdb_receive()){ gdb_consume(); }
 }
}



/*
** Keeps the window and the audio alive while the CPU is held.
*/
static void gdb_hold_refresh(void)
{
 SDL_Event ev;

 while (SDL_PollEvent(&ev) != 0){

  /* F5 switches the paused indicator panel, so the frame underneath can be
  ** looked at. This is the only place it can live: main_loop() never gets
  ** past cu_gdb_hold() while halted, so main.c's hotkeys are all dead here,
  ** and dead anyway whenever a keyboard dongle ROM has taken the keyboard.
  **
  ** Both edges are swallowed rather than just the press. A key up reaching
  ** the dongle emits a PS/2 break prefix for any key at all, which the ROM
  ** would read back as noise on resume. The repeat check keeps a held key
  ** from flapping the panel at the auto repeat rate. */
  if ( ( ((ev.type) == SDL_KEYDOWN) ||
         ((ev.type) == SDL_KEYUP  ) ) &&
       ((ev.key.keysym.sym) == SDLK_F5) ){
   if ( ((ev.type) == SDL_KEYDOWN) &&
        ((ev.key.repeat) == 0) ){
    cu_gdbovl_enable(!cu_gdbovl_isenabled());
   }
   continue;
  }

  ginput_sendevent(&ev);
  if (ev.type == SDL_QUIT){ gdb_quitreq = TRUE; }
 }

 audio_sendframe(NULL, 262U);
 cu_gdbovl_draw();
 guicore_update(FALSE);

 /* Whatever else has to go on living while the CPU is held (main.c: the API
 ** and web servers, so the other debugger's views of the halted machine and
 ** its network links stay up) */

 if (gdb_idlehook != NULL){ gdb_idlehook(); }

 /* main_loop() turns back at cu_gdb_hold() while the CPU is held, so the SD
 ** backend's own tick never fires from there. Without this, a file the ROM
 ** wrote just before a breakpoint would not appear on the host until the
 ** debugger let go -- which is exactly when you want to look at it. */

 cu_sdb_frametick();
}



/*
** Updates the widened cycle accumulator.
*/
static void gdb_cycle_track(void)
{
 auint cur = cpu_state.cycle;

 gdb_cyc64  += (uint64_t)(WRAP32(cur - gdb_cycprev));
 gdb_cycprev = cur;
}



/* ---------------------------------------------------------------------- */
/* Public interface                                                       */
/* ---------------------------------------------------------------------- */



auint cu_gdb_parseargs(int argc, char** argv)
{
 int   i;
 int   o = 1;
 char* a;

 for (i = 1; i < argc; i++){

  a = argv[i];

  if (strncmp(a, "--gdb-wait", 10U) == 0){
   gdb_wanted  = TRUE;
   gdb_waitatt = TRUE;
   continue;
  }

  if (strncmp(a, "--gdb-reverse", 13U) == 0){
   gdb_revena = TRUE;
   continue;
  }

  if (strncmp(a, "--gdb-focus-ide", 15U) == 0){
   gdb_focuside = TRUE;
   continue;
  }

  if (strncmp(a, "--gdb-nolimit", 13U) == 0){
   gdb_nolimit = TRUE;
   continue;
  }

  if (strncmp(a, "--gdb-show", 10U) == 0){
   gdb_show = TRUE;
   continue;
  }

  if (strncmp(a, "--gdb-log", 9U) == 0){
   gdb_dolog = TRUE;
   if (a[9] == '='){
    strncpy(&(gdb_logpath[0]), &(a[10]), sizeof(gdb_logpath) - 1U);
    gdb_logpath[sizeof(gdb_logpath) - 1U] = 0;
   }
   continue;
  }

  if (strncmp(a, "--gdb", 5U) == 0){
   if ((a[5] == 0) || (a[5] == '=')){
    gdb_wanted = TRUE;
    if (a[5] == '='){
     auint p = (auint)(strtoul(&(a[6]), NULL, 10));
     if ((p != 0U) && (p < 65536U)){ gdb_port = p; }
    }
    continue;
   }
  }

  argv[o] = a;
  o ++;
 }

 argv[o] = NULL;
 return (auint)(o);
}



void  cu_gdb_focus_capture(void)
{
#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 if (!gdb_focuside){ return; }
 gdb_idewin = GetForegroundWindow();
#endif
}



void  cu_gdb_init(void)
{
 struct sockaddr_in addr;
 int                one = 1;

 if (!gdb_wanted){ return; }

#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0){
   print_error("GDB: WSAStartup failed, stub disabled\n");
   return;
  }
 }
#endif

 gdb_listen = socket(AF_INET, SOCK_STREAM, 0);
 if (gdb_listen == GDB_BADSOCK){
  print_error("GDB: cannot create a socket, stub disabled\n");
  return;
 }

 (void)setsockopt(gdb_listen, SOL_SOCKET, SO_REUSEADDR,
     (char const*)(&one), (int)(sizeof(one)));

 memset(&addr, 0, sizeof(addr));
 addr.sin_family      = AF_INET;
 addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 addr.sin_port        = htons((unsigned short)(gdb_port));

 /* sizeof(addr), not sizeof(&addr): uzem passes the size of a pointer here,
 ** which is a real bug in that code. */
 if (bind(gdb_listen, (struct sockaddr*)(&addr), (int)(sizeof(addr))) != 0){
  print_error("GDB: cannot bind port %u, stub disabled\n",
      (unsigned)(gdb_port));
  gdb_sockclose(gdb_listen);
  gdb_listen = GDB_BADSOCK;
  return;
 }

 if (listen(gdb_listen, 1) != 0){
  print_error("GDB: cannot listen on port %u, stub disabled\n",
      (unsigned)(gdb_port));
  gdb_sockclose(gdb_listen);
  gdb_listen = GDB_BADSOCK;
  return;
 }

#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 { u_long nb = 1UL; (void)ioctlsocket(gdb_listen, FIONBIO, &nb); }
#else
 (void)fcntl(gdb_listen, F_SETFL,
     fcntl(gdb_listen, F_GETFL, 0) | O_NONBLOCK);
#endif

 gdb_up      = TRUE;
 gdb_cycprev = cpu_state.cycle;
 gdb_cyc64   = 0U;

 print_message("GDB: listening on localhost:%u\n", (unsigned)(gdb_port));
 fflush(stdout); /* Piped IDE terminals fully-buffer stdout; force it out now */

 if (gdb_waitatt){
  print_message("GDB: waiting for a client before starting\n");
  fflush(stdout); /* Same as above -- this is the line preLaunchTask watches for */
  gdb_halted   = TRUE;
  gdb_stopsent = TRUE; /* No client to tell yet */
  cu_gdbovl_halted(CU_GDBBRK_ST_NONE, 0U, FALSE, gdb_port);
 }else{
  cu_gdbovl_running(FALSE, gdb_port);
 }
}



void  cu_gdb_quit(void)
{
 if (!gdb_up){ return; }

 if (gdb_conn != GDB_BADSOCK){ gdb_sockclose(gdb_conn); }
 if (gdb_listen != GDB_BADSOCK){ gdb_sockclose(gdb_listen); }
 gdb_conn   = GDB_BADSOCK;
 gdb_listen = GDB_BADSOCK;

 gdb_snap_free();

 if (gdb_logf != NULL){ fclose(gdb_logf); gdb_logf = NULL; }

#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 WSACleanup();
#endif

 gdb_up = FALSE;
}



boole cu_gdb_ishalted(void)
{
 return gdb_halted;
}



void  cu_gdb_setidle(void (*hook)(void))
{
 gdb_idlehook = hook;
}



boole cu_gdb_ownsbreak(void)
{
 return (gdb_up && (gdb_halted || (cu_gdbbrk_getstop(NULL) != CU_GDBBRK_ST_NONE)));
}



void  cu_gdb_foreignstop(void)
{
 if ((!gdb_up) || (gdb_conn == GDB_BADSOCK) || gdb_halted){ return; }
 cu_gdbbrk_extstop();  /* cu_gdb_hold() reports it to the client as a halt */
 gdb_foreign = TRUE;
}



boole cu_gdb_foreignresumed(void)
{
 if ( gdb_foreign && (!gdb_halted) &&
      (cu_gdbbrk_getstop(NULL) == CU_GDBBRK_ST_NONE) ){
  gdb_foreign = FALSE;
  return TRUE;
 }
 return FALSE;
}



boole cu_gdb_startnolimit(void)
{
 return gdb_nolimit;
}



/*
** An unattended run (--gdb-nolimit: the test runner and agent probes) creates
** its window hidden, so dozens of them do not pop up over the desktop;
** --gdb-show keeps it visible. Screenshots read the pixel buffer, not the
** window, and input comes through the stub, so neither needs it shown.
*/
boole cu_gdb_starthidden(void)
{
 return (gdb_nolimit && (!gdb_show));
}



boole cu_gdb_isenabled(void)
{
 return gdb_wanted;
}



uint64_t cu_gdb_getcycle64(void)
{
 gdb_cycle_track(); /* Also kept without a client */
 return gdb_cyc64;
}


uint64_t cu_gdb_getframe(void)
{
 return gdb_cyc64 / (uint64_t)(CU_GDB_FRAMECY);
}


uint64_t cu_gdb_getsincestop(void)
{
 return gdb_cyc64 - gdb_cycstop;
}



void  cu_gdb_reset_target(void)
{
 cu_avr_reset();
 if (cu_gdbbrk_anomaly_get(CU_GDBBRK_AN_UNINIT_READ)){ cu_gdbbrk_poison(); }

 /* The reverse execution history describes a machine that no longer exists,
 ** and the widened cycle counter has to restart with it or every derived
 ** figure (frame numbers, since_stop) keeps counting from before the reset. */
 gdb_snap_free();
 gdb_cyc64   = 0U;
 gdb_cycprev = cpu_state.cycle;
 gdb_cycstop = 0U;

 cu_gdbmon_reset();
}



boole cu_gdb_hold(void)
{
 auint tick;
 auint dummy;

 if (!gdb_up){ return FALSE; }

 gdb_cycle_track();

 /* A break condition tripped inside the instruction loop */

 if ( (!gdb_halted) &&
      (cu_gdbbrk_getstop(&dummy) != CU_GDBBRK_ST_NONE) ){
  gdb_enter_halt();
 }

 gdb_service(0U);

 if (!gdb_halted){
  cu_gdbmon_frame();  /* Input recording and replay */
  gdb_snap_periodic();
  return gdb_quitreq;
 }

 /* Hold here, servicing the client. Stepping is run inline rather than by
 ** returning to the main loop, because a round trip through the main loop
 ** costs a display refresh and a source level "step" is many instruction
 ** steps. */

 tick = SDL_GetTicks();

 while (gdb_halted && (!gdb_quitreq)){

  gdb_service(2U);

  if ( gdb_focuspend && gdb_halted &&
       (((auint)(SDL_GetTicks()) - gdb_focustick) >= GDB_FOCUS_HOLD_MS) ){
   gdb_focuspend = FALSE;
   gdb_focus_restore();
  }

  if ((!gdb_halted) && gdb_instep){
   (void)gdb_run_until_break(4U);
   gdb_cycle_track();
   if (cu_gdbbrk_getstop(&dummy) != CU_GDBBRK_ST_NONE){
    gdb_enter_halt();
   }
   continue;
  }

  if ((SDL_GetTicks() - tick) >= 15U){
   tick = SDL_GetTicks();
   gdb_hold_refresh();
  }
 }

 return gdb_quitreq;
}



#endif /* CU_GDB */
