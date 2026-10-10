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



#include "cu_gdbmon.h"

#ifdef CU_GDB

#include "cu_gdb.h"
#include "cu_gdbbrk.h"
#include "cu_gdbovl.h"
#include "cu_avr.h"
#include "cu_ctr.h"
#include "cu_kbd.h"
#include "cu_mouse.h"
#include "cu_spir.h"
#include "guicore.h"
#include "frame.h"

#include <stdarg.h>

#if !defined(_WIN32) && !defined(TARGET_WINDOWS_MINGW)
 #include <limits.h>
#endif



extern cu_state_cpu_t cpu_state;
extern auint          video_pulsectr;
extern auint          video_cycle;


/* Cycles in a scanline. The row ends at the ROM's sync pulse, which for a
** correctly syncing video kernel lands at 1820; cu_avr.c's VIDEO_CY_MAX of
** 1920 is only the ceiling above which the row is called a sync error. See
** CU_GDB_FRAMECY in cu_gdb.h for how this was measured. */
#define MON_ROWCY 1820U

/* Formatted output cap, in bytes. An agent pays context tokens for every one
** of them, so terse structured output is not a nicety. */
#define MON_CAP 4096U



/* ---------------------------------------------------------------------- */
/* Output accumulator                                                     */
/* ---------------------------------------------------------------------- */

/*
** Commands never write the outer JSON braces: mon_begin() opens the object
** and mon_end() closes it, appending "truncated": true when the cap was hit.
** mon_add() stops once the cap is reached, mon_fadd() does not, so structural
** output (array closers, the trailing brace) always makes it out.
*/

static char  mon_buf[MON_CAP + 512U];
static auint mon_len    = 0U;
static boole mon_trunc  = FALSE;
static boole mon_json   = FALSE;
static boole mon_overlay = FALSE;
static boole mon_wait   = FALSE;
static boole mon_jfirst = TRUE;
static boole mon_failed = FALSE;


static void mon_vadd(char const* fmt, va_list ap)
{
 int n;

 if (mon_len >= (sizeof(mon_buf) - 1U)){ mon_trunc = TRUE; return; }

 n = vsnprintf(&(mon_buf[mon_len]), (sizeof(mon_buf) - mon_len), fmt, ap);

 if (n < 0){ return; }

 if ((auint)(n) >= (sizeof(mon_buf) - mon_len)){
  mon_len   = sizeof(mon_buf) - 1U;
  mon_trunc = TRUE;
  return;
 }

 mon_len += (auint)(n);
 if (mon_len >= MON_CAP){ mon_trunc = TRUE; }
}


/* Content. Stops at the cap. */
static void mon_add(char const* fmt, ...)
{
 va_list ap;

 if (mon_trunc){ return; }
 va_start(ap, fmt);
 mon_vadd(fmt, ap);
 va_end(ap);
}


/* Structure. Always emitted, so the output stays well formed. */
static void mon_fadd(char const* fmt, ...)
{
 va_list ap;

 va_start(ap, fmt);
 mon_vadd(fmt, ap);
 va_end(ap);
}


static void mon_begin(void)
{
 mon_len    = 0U;
 mon_trunc  = FALSE;
 mon_jfirst = TRUE;
 mon_failed = FALSE;
 mon_buf[0] = 0;

 if (mon_json){ mon_fadd("{"); }
}


static void mon_end(void)
{
 if (mon_failed){ return; } /* mon_error() already sent its own reply */

 if (mon_json){
  if (mon_trunc){
   mon_fadd("%s\"truncated\":true", mon_jfirst ? "" : ",");
  }
  mon_fadd("}\n");
 }else{
  if (mon_trunc){ mon_fadd("truncated: true\n"); }
 }

 mon_buf[mon_len] = 0;
 cu_gdb_out(&(mon_buf[0]));
}


static void mon_jstr(char const* val);


/*
** Errors are a single line, never silence, and never a partially built
** structure.
*/
static void mon_error(char const* fmt, ...)
{
 va_list ap;
 char    txt[256];

 va_start(ap, fmt);
 vsnprintf(&(txt[0]), sizeof(txt), fmt, ap);
 va_end(ap);

 mon_len    = 0U;
 mon_trunc  = FALSE;
 mon_buf[0] = 0;

 if (mon_json){
  mon_fadd("{\"error\":");
  mon_jstr(&(txt[0]));   /* Messages quote the offending word: must escape */
  mon_fadd("}\n");
 }else{
  mon_fadd("error: %s\n", &(txt[0]));
 }

 mon_buf[mon_len] = 0;
 cu_gdb_out(&(mon_buf[0]));
 mon_failed = TRUE;
}


/* Emits a JSON member name, or nothing in text mode. */
static void mon_key(char const* key)
{
 if (!mon_json){ return; }
 mon_fadd("%s\"%s\":", mon_jfirst ? "" : ",", key);
 mon_jfirst = FALSE;
}


static void mon_kv_num(char const* key, unsigned long long val)
{
 if (mon_json){ mon_key(key); mon_add("%llu", val); }
 else         { mon_add("%s: %llu\n", key, val); }
}


/* A value that can be negative, such as a mouse delta */
static void mon_kv_snum(char const* key, long long val)
{
 if (mon_json){ mon_key(key); mon_add("%lld", val); }
 else         { mon_add("%s: %lld\n", key, val); }
}


/*
** A hexadecimal value. JSON gets a number, humans get 0x notation.
*/
static void mon_kv_hex(char const* key, auint val, auint digits)
{
 if (mon_json){
  mon_key(key);
  mon_add("%u", (unsigned)(val));
 }else{
  mon_add("%s: 0x%0*X\n", key, (int)(digits), (unsigned)(val));
 }
}


/*
** Emits a JSON string literal, escaping what RFC 8259 requires. Not
** cosmetic: file paths on Windows are full of backslashes, and an error
** message can quote the offending word, either of which produces output no
** JSON parser will accept. Anything below 0x20 goes out as \u00XX; the
** monitor never emits non-ASCII, so UTF-8 needs no handling here.
*/
static void mon_jstr(char const* val)
{
 auint i;

 /* The quotes are structure and so always go out, even once the content cap
 ** has been hit: a truncated value must still close its string. */
 mon_fadd("\"");
 for (i = 0U; val[i] != 0; i ++){
  unsigned char c = (unsigned char)(val[i]);
  switch (c){
   case '\"': mon_add("\\\""); break;
   case '\\': mon_add("\\\\"); break;
   case '\n': mon_add("\\n");  break;
   case '\r': mon_add("\\r");  break;
   case '\t': mon_add("\\t");  break;
   case '\b': mon_add("\\b");  break;
   case '\f': mon_add("\\f");  break;
   default:
    if (c < 0x20U){ mon_add("\\u%04X", (unsigned)(c)); }
    else          { mon_add("%c", (int)(c)); }
    break;
  }
 }
 mon_fadd("\"");
}


static void mon_kv_str(char const* key, char const* val)
{
 if (mon_json){ mon_key(key); mon_jstr(val); }
 else         { mon_add("%s: %s\n", key, val); }
}


static void mon_kv_bool(char const* key, boole val)
{
 if (mon_json){ mon_key(key); mon_add("%s", val ? "true" : "false"); }
 else         { mon_add("%s: %s\n", key, val ? "on" : "off"); }
}



/* ---------------------------------------------------------------------- */
/* Small portable string helpers                                          */
/* ---------------------------------------------------------------------- */

static char mon_lower(char c)
{
 if ((c >= 'A') && (c <= 'Z')){ return (char)(c + ('a' - 'A')); }
 return c;
}


/* Case insensitive compare. strcasecmp is not portable enough to rely on. */
static boole mon_ieq(char const* a, char const* b)
{
 while ((*a != 0) && (*b != 0)){
  if (mon_lower(*a) != mon_lower(*b)){ return FALSE; }
  a ++;
  b ++;
 }
 return (boole)((*a == 0) && (*b == 0));
}


/* Value of a hexadecimal digit, or -1 */
static asint mon_nibble(char c)
{
 if ((c >= '0') && (c <= '9')){ return (asint)(c - '0'); }
 if ((c >= 'a') && (c <= 'f')){ return (asint)(c - 'a') + 10; }
 if ((c >= 'A') && (c <= 'F')){ return (asint)(c - 'A') + 10; }
 return -1;
}


static auint mon_parsenum(char const* s, boole* ok)
{
 char* end;
 auint v;

 if (ok != NULL){ *ok = TRUE; }

 if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))){
  v = (auint)(strtoul(&(s[2]), &end, 16));
 }else{
  v = (auint)(strtoul(s, &end, 10));
 }

 if ((end == s) || (*end != 0)){
  if (ok != NULL){ *ok = FALSE; }
 }

 return v;
}


/*
** Signed decimal. Mouse movement is a delta, so it has to be able to go
** backwards, which strtoul cannot express.
*/
static sint32 mon_parsesnum(char const* s, boole* ok)
{
 char* end;
 long  v;

 if (ok != NULL){ *ok = TRUE; }

 v = strtol(s, &end, 10);

 if ((end == s) || (*end != 0)){
  if (ok != NULL){ *ok = FALSE; }
 }

 return (sint32)(v);
}



/* ---------------------------------------------------------------------- */
/* ATmega644 I/O register table                                           */
/* ---------------------------------------------------------------------- */

/*
** The Microchip Studio style register view. Bit names come from the
** ATmega644 datasheet; where a register's bits are simply numbered (the
** ports, the pin change masks) "bitpfx" gives the prefix and the number is
** appended, which keeps the table readable.
*/

typedef struct{
 char const* name;
 uint16      addr;
 char const* bitpfx;
 char const* bits[8];  /* Index 0 is bit 0; NULL for a reserved bit */
}mon_reg_t;

#define NB {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL}

static mon_reg_t const mon_regs[] = {
 {"PINA",   0x20U, "PINA",  NB},
 {"DDRA",   0x21U, "DDA",   NB},
 {"PORTA",  0x22U, "PORTA", NB},
 {"PINB",   0x23U, "PINB",  NB},
 {"DDRB",   0x24U, "DDB",   NB},
 {"PORTB",  0x25U, "PORTB", NB},
 {"PINC",   0x26U, "PINC",  NB},
 {"DDRC",   0x27U, "DDC",   NB},
 {"PORTC",  0x28U, "PORTC", NB},
 {"PIND",   0x29U, "PIND",  NB},
 {"DDRD",   0x2AU, "DDD",   NB},
 {"PORTD",  0x2BU, "PORTD", NB},
 {"TIFR0",  0x35U, NULL, {"TOV0","OCF0A","OCF0B",NULL,NULL,NULL,NULL,NULL}},
 {"TIFR1",  0x36U, NULL, {"TOV1","OCF1A","OCF1B",NULL,NULL,"ICF1",NULL,NULL}},
 {"TIFR2",  0x37U, NULL, {"TOV2","OCF2A","OCF2B",NULL,NULL,NULL,NULL,NULL}},
 {"PCIFR",  0x3BU, NULL, {"PCIF0","PCIF1","PCIF2","PCIF3",NULL,NULL,NULL,NULL}},
 {"EIFR",   0x3CU, NULL, {"INTF0","INTF1","INTF2",NULL,NULL,NULL,NULL,NULL}},
 {"EIMSK",  0x3DU, NULL, {"INT0","INT1","INT2",NULL,NULL,NULL,NULL,NULL}},
 {"GPIOR0", 0x3EU, NULL, NB},
 {"EECR",   0x3FU, NULL, {"EERE","EEPE","EEMPE","EERIE","EEPM0","EEPM1",NULL,NULL}},
 {"EEDR",   0x40U, NULL, NB},
 {"EEARL",  0x41U, NULL, NB},
 {"EEARH",  0x42U, NULL, NB},
 {"GTCCR",  0x43U, NULL, {"PSRSYNC","PSRASY",NULL,NULL,NULL,NULL,NULL,"TSM"}},
 {"TCCR0A", 0x44U, NULL, {"WGM00","WGM01",NULL,NULL,"COM0B0","COM0B1","COM0A0","COM0A1"}},
 {"TCCR0B", 0x45U, NULL, {"CS00","CS01","CS02","WGM02",NULL,NULL,"FOC0B","FOC0A"}},
 {"TCNT0",  0x46U, NULL, NB},
 {"OCR0A",  0x47U, NULL, NB},
 {"OCR0B",  0x48U, NULL, NB},
 {"GPIOR1", 0x4AU, NULL, NB},
 {"GPIOR2", 0x4BU, NULL, NB},
 {"SPCR",   0x4CU, NULL, {"SPR0","SPR1","CPHA","CPOL","MSTR","DORD","SPE","SPIE"}},
 {"SPSR",   0x4DU, NULL, {"SPI2X",NULL,NULL,NULL,NULL,NULL,"WCOL","SPIF"}},
 {"SPDR",   0x4EU, NULL, NB},
 {"ACSR",   0x50U, NULL, {"ACIS0","ACIS1","ACIC","ACIE","ACI","ACO","ACBG","ACD"}},
 {"OCDR",   0x51U, NULL, NB},
 {"SMCR",   0x53U, NULL, {"SE","SM0","SM1","SM2",NULL,NULL,NULL,NULL}},
 {"MCUSR",  0x54U, NULL, {"PORF","EXTRF","BORF","WDRF","JTRF",NULL,NULL,NULL}},
 {"MCUCR",  0x55U, NULL, {"IVCE","IVSEL",NULL,NULL,"PUD",NULL,NULL,"JTD"}},
 {"SPMCSR", 0x57U, NULL, {"SPMEN","PGERS","PGWRT","BLBSET","RWWSRE","SIGRD","RWWSB","SPMIE"}},
 {"SPL",    0x5DU, NULL, NB},
 {"SPH",    0x5EU, NULL, NB},
 {"SREG",   0x5FU, NULL, {"C","Z","N","V","S","H","T","I"}},
 {"WDTCSR", 0x60U, NULL, {"WDP0","WDP1","WDP2","WDE","WDCE","WDP3","WDIE","WDIF"}},
 {"CLKPR",  0x61U, NULL, {"CLKPS0","CLKPS1","CLKPS2","CLKPS3",NULL,NULL,NULL,"CLKPCE"}},
 {"PRR",    0x64U, NULL, {"PRADC","PRUSART0","PRSPI","PRTIM1",NULL,"PRTIM0","PRTIM2","PRTWI"}},
 {"OSCCAL", 0x66U, NULL, NB},
 {"PCICR",  0x68U, NULL, {"PCIE0","PCIE1","PCIE2","PCIE3",NULL,NULL,NULL,NULL}},
 {"EICRA",  0x69U, NULL, {"ISC00","ISC01","ISC10","ISC11","ISC20","ISC21",NULL,NULL}},
 {"PCMSK0", 0x6BU, "PCINT",  NB},
 {"PCMSK1", 0x6CU, "PCINT1", NB},
 {"PCMSK2", 0x6DU, "PCINT2", NB},
 {"TIMSK0", 0x6EU, NULL, {"TOIE0","OCIE0A","OCIE0B",NULL,NULL,NULL,NULL,NULL}},
 {"TIMSK1", 0x6FU, NULL, {"TOIE1","OCIE1A","OCIE1B",NULL,NULL,"ICIE1",NULL,NULL}},
 {"TIMSK2", 0x70U, NULL, {"TOIE2","OCIE2A","OCIE2B",NULL,NULL,NULL,NULL,NULL}},
 {"PCMSK3", 0x73U, "PCINT3", NB},
 {"ADCL",   0x78U, NULL, NB},
 {"ADCH",   0x79U, NULL, NB},
 {"ADCSRA", 0x7AU, NULL, {"ADPS0","ADPS1","ADPS2","ADIE","ADIF","ADATE","ADSC","ADEN"}},
 {"ADCSRB", 0x7BU, NULL, {"ADTS0","ADTS1","ADTS2",NULL,NULL,NULL,"ACME",NULL}},
 {"ADMUX",  0x7CU, NULL, {"MUX0","MUX1","MUX2","MUX3","MUX4","ADLAR","REFS0","REFS1"}},
 {"DIDR0",  0x7EU, "ADC",   NB},
 {"DIDR1",  0x7FU, NULL, {"AIN0D","AIN1D",NULL,NULL,NULL,NULL,NULL,NULL}},
 {"TCCR1A", 0x80U, NULL, {"WGM10","WGM11",NULL,NULL,"COM1B0","COM1B1","COM1A0","COM1A1"}},
 {"TCCR1B", 0x81U, NULL, {"CS10","CS11","CS12","WGM12","WGM13",NULL,"ICES1","ICNC1"}},
 {"TCCR1C", 0x82U, NULL, {NULL,NULL,NULL,NULL,NULL,NULL,"FOC1B","FOC1A"}},
 {"TCNT1L", 0x84U, NULL, NB},
 {"TCNT1H", 0x85U, NULL, NB},
 {"ICR1L",  0x86U, NULL, NB},
 {"ICR1H",  0x87U, NULL, NB},
 {"OCR1AL", 0x88U, NULL, NB},
 {"OCR1AH", 0x89U, NULL, NB},
 {"OCR1BL", 0x8AU, NULL, NB},
 {"OCR1BH", 0x8BU, NULL, NB},
 {"TCCR2A", 0xB0U, NULL, {"WGM20","WGM21",NULL,NULL,"COM2B0","COM2B1","COM2A0","COM2A1"}},
 {"TCCR2B", 0xB1U, NULL, {"CS20","CS21","CS22","WGM22",NULL,NULL,"FOC2B","FOC2A"}},
 {"TCNT2",  0xB2U, NULL, NB},
 {"OCR2A",  0xB3U, NULL, NB},
 {"OCR2B",  0xB4U, NULL, NB},
 {"ASSR",   0xB6U, NULL, {"TCR2BUB","TCR2AUB","OCR2BUB","OCR2AUB","TCN2UB","AS2","EXCLK",NULL}},
 {"TWBR",   0xB8U, NULL, NB},
 {"TWSR",   0xB9U, NULL, {"TWPS0","TWPS1",NULL,"TWS3","TWS4","TWS5","TWS6","TWS7"}},
 {"TWAR",   0xBAU, NULL, {"TWGCE","TWA0","TWA1","TWA2","TWA3","TWA4","TWA5","TWA6"}},
 {"TWDR",   0xBBU, NULL, NB},
 {"TWCR",   0xBCU, NULL, {"TWIE",NULL,"TWEN","TWWC","TWSTO","TWSTA","TWEA","TWINT"}},
 {"TWAMR",  0xBDU, NULL, NB},
 {"UCSR0A", 0xC0U, NULL, {"MPCM0","U2X0","UPE0","DOR0","FE0","UDRE0","TXC0","RXC0"}},
 {"UCSR0B", 0xC1U, NULL, {"TXB80","RXB80","UCSZ02","TXEN0","RXEN0","UDRIE0","TXCIE0","RXCIE0"}},
 {"UCSR0C", 0xC2U, NULL, {"UCPOL0","UCSZ00","UCSZ01","USBS0","UPM00","UPM01","UMSEL00","UMSEL01"}},
 {"UBRR0L", 0xC4U, NULL, NB},
 {"UBRR0H", 0xC5U, NULL, NB},
 {"UDR0",   0xC6U, NULL, NB}
};

#define MON_REGCOUNT (sizeof(mon_regs) / sizeof(mon_regs[0]))



/*
** Returns the name of bit "bit" of a register, or NULL when it is reserved.
*/
static char const* mon_bitname(mon_reg_t const* r, auint bit, char* scratch)
{
 if (r->bits[bit] != NULL){ return r->bits[bit]; }
 if (r->bitpfx != NULL){
  sprintf(scratch, "%s%u", r->bitpfx, (unsigned)(bit));
  return scratch;
 }
 return NULL;
}


static void mon_reg_print(mon_reg_t const* r, boole first)
{
 auint       val = cpu_state.iors[r->addr];
 auint       i;
 char        scratch[32];
 char const* bn;
 boole       any = FALSE;

 if (mon_json){

  mon_add("%s{\"name\":\"%s\",\"addr\":%u,\"value\":%u,\"bits\":{",
      first ? "" : ",", r->name, (unsigned)(r->addr), (unsigned)(val));
  for (i = 8U; i > 0U; i--){
   bn = mon_bitname(r, i - 1U, &(scratch[0]));
   if (bn == NULL){ continue; }
   mon_add("%s\"%s\":%u", any ? "," : "", bn,
       (unsigned)((val >> (i - 1U)) & 1U));
   any = TRUE;
  }
  mon_add("}}");

 }else{

  mon_add("%-6s: 0x%02X", r->name, (unsigned)(val));
  for (i = 8U; i > 0U; i--){
   bn = mon_bitname(r, i - 1U, &(scratch[0]));
   if (bn == NULL){ continue; }
   mon_add("%s%s=%u", any ? " " : "  [", bn,
       (unsigned)((val >> (i - 1U)) & 1U));
   any = TRUE;
  }
  if (any){ mon_add("]"); }
  mon_add("\n");

 }
}



/* ---------------------------------------------------------------------- */
/* Minimal PNG writer                                                     */
/* ---------------------------------------------------------------------- */

/*
** Only as much of PNG as a framebuffer dump needs: 8 bit RGB, no interlace,
** and a zlib stream built from stored (uncompressed) deflate blocks. That
** avoids taking zlib on as a new dependency for what is a debugging
** convenience.
*/

static uint32 png_crctab[256];
static boole  png_crcready = FALSE;


static uint32 png_crc(uint32 crc, uint8 const* buf, auint len)
{
 auint  i;
 auint  k;
 uint32 c;

 if (!png_crcready){
  for (i = 0U; i < 256U; i++){
   c = (uint32)(i);
   for (k = 0U; k < 8U; k++){
    if ((c & 1U) != 0U){ c = 0xEDB88320UL ^ (c >> 1); }
    else               { c = c >> 1; }
   }
   png_crctab[i] = c;
  }
  png_crcready = TRUE;
 }

 for (i = 0U; i < len; i++){
  crc = png_crctab[(crc ^ buf[i]) & 0xFFU] ^ (crc >> 8);
 }

 return crc;
}


static void png_be32(uint8* d, uint32 v)
{
 d[0] = (uint8)((v >> 24) & 0xFFU);
 d[1] = (uint8)((v >> 16) & 0xFFU);
 d[2] = (uint8)((v >>  8) & 0xFFU);
 d[3] = (uint8)((v      ) & 0xFFU);
}


static boole png_chunk(FILE* f, char const* type, uint8 const* data, auint len)
{
 uint8  hdr[8];
 uint8  crcb[4];
 uint32 crc;

 png_be32(&(hdr[0]), (uint32)(len));
 hdr[4] = (uint8)(type[0]);
 hdr[5] = (uint8)(type[1]);
 hdr[6] = (uint8)(type[2]);
 hdr[7] = (uint8)(type[3]);

 crc = png_crc(0xFFFFFFFFUL, &(hdr[4]), 4U);
 if (len != 0U){ crc = png_crc(crc, data, len); }
 crc ^= 0xFFFFFFFFUL;
 png_be32(&(crcb[0]), crc);

 if (fwrite(&(hdr[0]), 1U, 8U, f) != 8U){ return FALSE; }
 if (len != 0U){ if (fwrite(data, 1U, len, f) != len){ return FALSE; } }
 if (fwrite(&(crcb[0]), 1U, 4U, f) != 4U){ return FALSE; }

 return TRUE;
}


/*
** A small deflate: one block of the fixed codes, matches found through a
** table of the last place each three bytes were seen. An emulator's picture
** is runs and repeats (and a screenshot's lines come in pairs), so this takes
** a megabyte to a few tens of kilobytes without a library. "out" must hold
** len + len / 8 + 16 bytes. Returns the bytes written.
*/
static auint  png_zpos;
static uint32 png_zacc;
static auint  png_zbits;

static void png_zput(uint8* out, auint v, auint n)
{
 png_zacc  |= (uint32)(v) << png_zbits;
 png_zbits += n;
 while (png_zbits >= 8U){
  out[png_zpos] = (uint8)(png_zacc & 0xFFU);
  png_zpos  ++;
  png_zacc  >>= 8;
  png_zbits -= 8U;
 }
}

/* A code of the fixed trees: its bits go most significant first */
static void png_zcode(uint8* out, auint c, auint n)
{
 auint r = 0U;
 auint i;
 for (i = 0U; i < n; i++){ r = (r << 1) | ((c >> i) & 1U); }
 png_zput(out, r, n);
}

static void png_zsym(uint8* out, auint s)
{
 if      (s < 144U){ png_zcode(out, 0x30U  + s, 8U); }
 else if (s < 256U){ png_zcode(out, 0x190U + (s - 144U), 9U); }
 else if (s < 280U){ png_zcode(out, s - 256U, 7U); }
 else              { png_zcode(out, 0xC0U  + (s - 280U), 8U); }
}

static auint png_deflate(uint8* out, uint8 const* in, auint len)
{
 static uint16 const lbase[29] = {3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U, 15U, 17U, 19U, 23U, 27U, 31U,
                                  35U, 43U, 51U, 59U, 67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U};
 static uint8  const lxtra[29] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U, 2U, 2U, 2U,
                                  3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U};
 static uint16 const dbase[30] = {1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U, 33U, 49U, 65U, 97U, 129U, 193U,
                                  257U, 385U, 513U, 769U, 1025U, 1537U, 2049U, 3073U, 4097U, 6145U,
                                  8193U, 12289U, 16385U, 24577U};
 static uint8  const dxtra[30] = {0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 4U, 5U, 5U, 6U, 6U,
                                  7U, 7U, 8U, 8U, 9U, 9U, 10U, 10U, 11U, 11U, 12U, 12U, 13U, 13U};
 static uint32 head[1U << 15];
 auint pos = 0U;
 auint h;
 auint c;
 auint n;
 auint d;
 auint k;

 png_zpos  = 0U;
 png_zacc  = 0U;
 png_zbits = 0U;
 memset(&(head[0]), 0xFF, sizeof(head));

 png_zput(out, 1U, 1U); /* BFINAL */
 png_zput(out, 1U, 2U); /* BTYPE = fixed codes */

 while (pos < len){
  n = 0U;
  d = 0U;
  if ((pos + 3U) <= len){
   h = (((auint)(in[pos]) << 10) ^ ((auint)(in[pos + 1U]) << 5) ^ (auint)(in[pos + 2U])) & 0x7FFFU;
   c = head[h];
   head[h] = (uint32)(pos);
   if ((c != 0xFFFFFFFFUL) && ((pos - c) <= 32768U)){
    while ((n < 258U) && ((pos + n) < len) && (in[c + n] == in[pos + n])){ n ++; }
    d = pos - c;
   }
  }
  if (n >= 3U){
   k = 28U; while (lbase[k] > n){ k --; }
   png_zsym(out, 257U + k);
   png_zput(out, n - lbase[k], lxtra[k]);
   k = 29U; while (dbase[k] > d){ k --; }
   png_zcode(out, k, 5U);
   png_zput(out, d - dbase[k], dxtra[k]);
   pos += n;
  }else{
   png_zsym(out, in[pos]);
   pos ++;
  }
 }

 png_zsym(out, 256U); /* End of block */
 if (png_zbits != 0U){ png_zput(out, 0U, 8U - png_zbits); }

 return png_zpos;
}


/*
** Writes an RGB888 image. "rgb" holds w * h * 3 bytes.
*/
static boole png_write(char const* path, uint8 const* rgb, auint w, auint h)
{
 static uint8 const sig[8] = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
 FILE*  f;
 uint8  ihdr[13];
 uint8* raw;
 uint8* zbuf;
 auint  rawlen = ((w * 3U) + 1U) * h;
 auint  zlen;
 auint  y;
 auint  pos;
 auint  zpos;
 auint  i;
 uint32 a  = 1U;
 uint32 b  = 0U;
 boole  ok = TRUE;

 if ((w == 0U) || (h == 0U)){ return FALSE; }

 raw = (uint8*)(malloc(rawlen));
 if (raw == NULL){ return FALSE; }

 pos = 0U;
 for (y = 0U; y < h; y++){
  raw[pos] = 0U; /* Filter type: none */
  pos ++;
  memcpy(&(raw[pos]), &(rgb[y * w * 3U]), w * 3U);
  pos += w * 3U;
 }

 for (i = 0U; i < rawlen; i++){
  a = (a + raw[i]) % 65521UL;
  b = (b + a) % 65521UL;
 }

 /* Two byte zlib header, one deflate block of fixed codes (9 bits a byte at
 ** worst, and its end), and a four byte Adler-32 trailer */
 zlen = 2U + rawlen + (rawlen >> 3) + 16U + 4U;
 zbuf = (uint8*)(malloc(zlen));
 if (zbuf == NULL){ free(raw); return FALSE; }

 zbuf[0] = 0x78U; /* CM = deflate, CINFO = 7 */
 zbuf[1] = 0x01U; /* No preset dictionary, fastest */
 zpos = 2U + png_deflate(&(zbuf[2]), raw, rawlen);
 (void)(pos);

 png_be32(&(zbuf[zpos]), (uint32)((b << 16) | a));
 zpos += 4U;

 f = fopen(path, "wb");
 if (f == NULL){ free(raw); free(zbuf); return FALSE; }

 if (fwrite(&(sig[0]), 1U, 8U, f) != 8U){ ok = FALSE; }

 png_be32(&(ihdr[0]), (uint32)(w));
 png_be32(&(ihdr[4]), (uint32)(h));
 ihdr[8]  = 8U; /* Bit depth */
 ihdr[9]  = 2U; /* Colour type: truecolour */
 ihdr[10] = 0U; /* Compression method */
 ihdr[11] = 0U; /* Filter method */
 ihdr[12] = 0U; /* Interlace method */

 if (ok && (!png_chunk(f, "IHDR", &(ihdr[0]), 13U))){ ok = FALSE; }
 if (ok && (!png_chunk(f, "IDAT", zbuf, zpos))){ ok = FALSE; }
 if (ok && (!png_chunk(f, "IEND", NULL, 0U))){ ok = FALSE; }

 fclose(f);
 free(raw);
 free(zbuf);

 return ok;
}


/*
** Grabs the GUI framebuffer as RGB888.
*/
static uint8* mon_grab(auint* w, auint* h)
{
 guicore_pixfmt_t fmt;
 uint32 const*    pix = guicore_getpixbuf();
 auint            ptc = guicore_getpitch();
 auint            x;
 auint            y;
 uint8*           rgb;

 *w = 640U;
 *h = 280U;

 if (pix == NULL){ return NULL; }

 guicore_getpixfmt(&fmt);

 rgb = (uint8*)(malloc((*w) * (*h) * 3U));
 if (rgb == NULL){ return NULL; }

 for (y = 0U; y < (*h); y++){
  for (x = 0U; x < (*w); x++){
   uint32 p = pix[(y * ptc) + x];
   auint  o = (((y * (*w)) + x) * 3U);
   rgb[o + 0U] = (uint8)((p >> fmt.rsh) & 0xFFU);
   rgb[o + 1U] = (uint8)((p >> fmt.gsh) & 0xFFU);
   rgb[o + 2U] = (uint8)((p >> fmt.bsh) & 0xFFU);
  }
 }

 return rgb;
}


/*
** The game's picture alone (see the header): rows 20 to 247 of the frame,
** columns 10 to 629, as the film has them; each line twice.
*/
static char mon_game[24] = "game";

void cu_gdbmon_setgame(char const* path)
{
 char const* b = path;
 auint       i;

 if (path == NULL){ return; }
 for (i = 0U; path[i] != 0; i++){
  if ((path[i] == '/') || (path[i] == '\\') || (path[i] == ':')){ b = path + i + 1U; }
 }
 for (i = 0U; (i < (sizeof(mon_game) - 1U)) && (b[i] != 0) && (b[i] != '-') && (b[i] != '.'); i++){
  mon_game[i] = b[i];
 }
 if (i != 0U){ mon_game[i] = 0; }
}


void cu_gdbmon_gameshot(void)
{
 auint  w;
 auint  h;
 auint  y;
 auint  n;
 uint8* rgb;
 uint8* out;
 boole  hid;
 FILE*  f;
 char   name[48];

 for (n = 1U; n < 10000U; n++){
  snprintf(&(name[0]), sizeof(name), "%s-snapshot-%04u.png", &(mon_game[0]), (unsigned)(n));
  f = fopen(&(name[0]), "rb");
  if (f == NULL){ break; }
  fclose(f);
 }
 hid = cu_gdbovl_hide();
 rgb = mon_grab(&w, &h);
 if (hid){ cu_gdbovl_show(); }
 if (rgb == NULL){ return; }
 out = (uint8*)(malloc(620U * 456U * 3U));
 if (out != NULL){
  for (y = 0U; y < 456U; y++){
   memcpy(out + (y * 620U * 3U), rgb + ((((20U + (y >> 1)) * w) + 10U) * 3U), 620U * 3U);
  }
  if (png_write(&(name[0]), out, 620U, 456U)){ print_message("Screenshot: %s\n", &(name[0])); }
  free(out);
 }
 free(rgb);
}


/*
** Resolves a path to an absolute one, so an agent can hand it straight to a
** file reading tool.
*/
static void mon_abspath(char const* in, char* out, auint outsz)
{
#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 char* r = _fullpath(NULL, in, 0);
 if (r != NULL){
  strncpy(out, r, outsz - 1U);
  out[outsz - 1U] = 0;
  free(r);
  return;
 }
#else
 char  tmp[PATH_MAX];
 if (realpath(in, &(tmp[0])) != NULL){
  strncpy(out, &(tmp[0]), outsz - 1U);
  out[outsz - 1U] = 0;
  return;
 }
#endif
 strncpy(out, in, outsz - 1U);
 out[outsz - 1U] = 0;
}


static void mon_tmppath(char* out, auint outsz, char const* ext)
{
 static auint seq = 0U;
 char const*  dir;

#if defined(_WIN32) || defined(TARGET_WINDOWS_MINGW)
 dir = getenv("TEMP");
 if (dir == NULL){ dir = getenv("TMP"); }
 if (dir == NULL){ dir = "."; }
#else
 dir = getenv("TMPDIR");
 if (dir == NULL){ dir = "/tmp"; }
#endif

 snprintf(out, outsz, "%s/cuzebox-%u.%s", dir, (unsigned)(seq), ext);
 seq ++;
}



/* ---------------------------------------------------------------------- */
/* Input recording and replay                                             */
/* ---------------------------------------------------------------------- */

/*
** Deterministic input replay is what turns a save state from "approximately
** reproducible" into reproducible, since the controller is the one emulator
** input that is not a function of the saved state.
*/

static FILE* mon_inrec = NULL;
static FILE* mon_inrep = NULL;



/* ---------------------------------------------------------------------- */
/* Injected input                                                         */
/* ---------------------------------------------------------------------- */

/*
** Everything an agent can do to the emulator's inputs is timed off one tick,
** cu_gdbmon_frame(), which the main loop already runs once per emulated
** frame. That is the only clock here worth having: wall time does not map to
** frames (F4 lets the emulator free run well above 60 Hz, F9 stops it dead),
** and a monitor command is serviced once per frame anyway.
**
** Driving input from here rather than from the host's window system is what
** makes it usable by a script: it is frame aligned, it is acknowledged, and
** it works with the window unfocused, minimised, or absent.
*/

/* Host key events waiting to be handed to SDL. They are queued rather than
** pushed as the command is parsed for two reasons: a tap has to put its press
** and its release on different frames to be an edge at all, and while the CPU
** is halted main_loop() never reaches its own event pump, so anything pushed
** then would be drained and dropped by gdb_hold_refresh(). Queued, a key
** issued during a break simply lands when the emulator runs again. */
#define MON_KEYQ_SZ 256U

typedef struct{
 SDL_Keycode sym;
 boole       down;
}mon_keyev_t;

static mon_keyev_t mon_keyq[MON_KEYQ_SZ];
static auint       mon_keyq_in  = 0U;
static auint       mon_keyq_out = 0U;

/* Frames left before a timed controller press is released, per player */
static auint mon_press_left[2] = {0U, 0U};

/* Frames left to run before halting, 0 when not armed */
static auint mon_runframes = 0U;

/* Frame limiter: what main_loop() last reported, and a change "monitor speed"
** wants it to make. Deliberately not cleared by cu_gdbmon_reset(): the speed
** belongs to the emulator, not to the ROM being reset. */
static boole mon_nolimit_cur = FALSE;
static boole mon_nolimit_req = FALSE;
static boole mon_nolimit_pending = FALSE;

/* Emulated cycle count at the previous tick, which is how a frame that was
** really emulated is told from a main loop iteration that produced none */
static uint64_t mon_lastcyc = 0U;
static boole    mon_lastset = FALSE;

/*
** --wait: halt once injected input has actually reached the ROM.
**
** It cannot block. cu_gdbmon_exec() runs inside packet dispatch, which
** cu_gdb_hold() reaches above main_loop()'s frame_run(), so waiting inside the
** command would stop the emulation that drains the queue. It arms a halt
** instead, exactly as "frames" does, and the caller continues.
**
** The condition is an exact signal rather than a margin: our own queue empty,
** and the dongle's byte queue empty too, which means cu_kbd_process() has
** clocked every byte out to the ROM. Then a few settle frames for the ROM to
** finish parsing what it read.
*/
#define MON_WAIT_NONE     0U
#define MON_WAIT_PENDING  1U
#define MON_WAIT_DRAINED  2U
#define MON_WAIT_TIMEOUT  3U

/* Frames allowed beyond delivery before a wait is called wedged. Budgeted per
** wait rather than flat: delivery alone is a frame per key event, so a long
** string must not time out merely for being long. */
#define MON_WAIT_MARGIN 600U

/* Frames held after the queues drain, for the ROM to parse what it read */
#define MON_WAIT_SETTLE 3U

static boole mon_waitarm    = FALSE;
static auint mon_waitsettle = 0U;   /* Frames to hold after the queues drain */
static auint mon_waitleft   = 0U;   /* Settle countdown */
static auint mon_waitcap    = 0U;   /* Safety timeout, in emulated frames */
static auint mon_waitres    = MON_WAIT_NONE;


extern SDL_Window* guicore_window;


/*
** Number of key events queued.
*/
static auint mon_keyq_count(void)
{
 return (mon_keyq_in - mon_keyq_out) % MON_KEYQ_SZ;
}


/*
** Queues a host key event, FALSE if the queue is full.
*/
static boole mon_key_push(SDL_Keycode sym, boole down)
{
 auint nxt = (mon_keyq_in + 1U) % MON_KEYQ_SZ;

 if (nxt == mon_keyq_out){ return FALSE; }

 mon_keyq[mon_keyq_in].sym  = sym;
 mon_keyq[mon_keyq_in].down = down;
 mon_keyq_in = nxt;

 return TRUE;
}


/*
** Hands one queued key to SDL as though the window had produced it.
** main_loop() picks it up in the same iteration and runs it through exactly
** the path a real key takes: ginput_sendevent() first, then the emulator's
** own hotkey switch. So this one function reaches the controller mapping, the
** keyboard dongle and the F keys alike, with no duplicated key handling and
** nothing added to main.c.
**
** One event per frame, deliberately: it is what separates a tap's two edges.
*/
static void mon_key_flush(void)
{
 SDL_Event ev;

 if (mon_keyq_out == mon_keyq_in){ return; }

 memset(&ev, 0, sizeof(ev));

 ev.key.type      = mon_keyq[mon_keyq_out].down ? SDL_KEYDOWN : SDL_KEYUP;
 ev.key.timestamp = SDL_GetTicks();
 ev.key.state     = mon_keyq[mon_keyq_out].down ? SDL_PRESSED : SDL_RELEASED;
 ev.key.repeat    = 0U;
 ev.key.keysym.sym      = mon_keyq[mon_keyq_out].sym;
 ev.key.keysym.scancode = SDL_GetScancodeFromKey(mon_keyq[mon_keyq_out].sym);
 ev.key.keysym.mod      = KMOD_NONE;
 if (guicore_window != NULL){
  ev.key.windowID = SDL_GetWindowID(guicore_window);
 }

 (void)SDL_PushEvent(&ev);

 mon_keyq_out = (mon_keyq_out + 1U) % MON_KEYQ_SZ;
}


/*
** Arms a --wait for whatever was just queued, sizing its safety timeout from
** the work in hand.
*/
static void mon_wait_arm(void)
{
 mon_waitarm    = TRUE;
 mon_waitleft   = mon_waitsettle;
 mon_waitcap    = mon_keyq_count() + mon_waitsettle + MON_WAIT_MARGIN;
 mon_waitres    = MON_WAIT_PENDING;
}


/*
** How the last --wait ended, for monitor status. The outcome is only known
** when the halt fires, long after the command that armed it has replied, so
** this is where a caller reads it back.
*/
static char const* mon_wait_str(void)
{
 switch (mon_waitres){
  case MON_WAIT_PENDING: return "pending";
  case MON_WAIT_DRAINED: return "drained";
  case MON_WAIT_TIMEOUT: return "timeout";
  default:               return "none";
 }
}


void  cu_gdbmon_reset(void)
{
 mon_keyq_in  = 0U;
 mon_keyq_out = 0U;

 mon_press_left[0] = 0U;
 mon_press_left[1] = 0U;

 mon_runframes = 0U;

 mon_waitarm  = FALSE;
 mon_waitleft = 0U;
 mon_waitcap  = 0U;
 mon_waitres  = MON_WAIT_NONE;

 /* The cycle counter is zeroed by the reset, so the next tick must not read
 ** the old value and conclude no frame happened. */
 mon_lastset = FALSE;
}


void  cu_gdbmon_resumed(void)
{
 mon_lastcyc = cu_gdb_getcycle64();
 mon_lastset = TRUE;
}


void  cu_gdbmon_frame(void)
{
 auint    p0;
 auint    p1;
 auint    i;
 char     line[128];
 uint64_t cyc = cu_gdb_getcycle64();

 /* Keys go out per main loop iteration, above the guard below, because they
 ** are host events rather than emulated ones. It is not a detail: F9 pauses
 ** the emulator, and a queue that only drained on emulated frames could never
 ** deliver the F9 that resumes it. Consecutive events still land on separate
 ** iterations, which is what keeps a tap's two edges apart. */
 mon_key_flush();

 /* cu_gdb_hold() is called above main_loop()'s pause test, so it keeps
 ** running when F9 has the emulator stopped and no frame is being produced.
 ** Counting those would make every timer below drift, and would mistime the
 ** input recording that was already using this tick; emulated cycles are the
 ** only honest evidence that a frame happened. */
 if (mon_lastset && (cyc == mon_lastcyc)){ return; }
 mon_lastcyc = cyc;
 mon_lastset = TRUE;

 for (i = 0U; i < 2U; i ++){
  if (mon_press_left[i] != 0U){
   mon_press_left[i] --;
   if (mon_press_left[i] == 0U){ cu_ctr_setsnes(i, 0U); }
  }
 }

 /* Halting through the same request Ctrl-C uses, so the stop is reported by
 ** the existing path with no new stop reason to invent. */
 if (mon_runframes != 0U){
  mon_runframes --;
  if (mon_runframes == 0U){ cu_gdbbrk_halt_req(); }
 }

 /* --wait. Below the frame guard deliberately: the timeout is counted in
 ** emulated frames, so an emulator paused with F9 neither drains nor times
 ** out. A paused emulator is not a wedged ROM. */
 if (mon_waitarm){

  if ( (mon_keyq_count() == 0U) &&
       (cu_kbd_queue_in == cu_kbd_queue_out) ){
   /* Everything queued has been clocked out to the ROM; let it parse. */
   if (mon_waitleft != 0U){ mon_waitleft --; }
   if (mon_waitleft == 0U){
    mon_waitarm = FALSE;
    mon_waitres = MON_WAIT_DRAINED;
    cu_gdbbrk_halt_req();
   }
  }else{
   mon_waitleft = mon_waitsettle; /* More arrived: settle again after it */
  }

  if (mon_waitcap != 0U){ mon_waitcap --; }

  if (mon_waitarm && (mon_waitcap == 0U)){
   mon_waitarm = FALSE;
   mon_waitres = MON_WAIT_TIMEOUT;
   /* Said out loud, not just recorded: a script that reads the ROM's state
   ** after a silent timeout is reading garbage and has no idea why. */
   cu_gdb_out("cuzebox: --wait timed out, the ROM stopped reading the"
              " keyboard\n");
   cu_gdbbrk_halt_req();
  }
 }

 /* A line a frame: not one more for a pass that goes on with a frame a halt
 ** cut (a breakpoint, a frames countdown), or every frame after it would be
 ** recorded or replayed one late */
 if (frame_iscut()){ return; }

 if (mon_inrec != NULL){
  (void)cu_ctr_getsnes_state(0U, &p0);
  (void)cu_ctr_getsnes_state(1U, &p1);
  fprintf(mon_inrec, "%04X %04X\n", (unsigned)(p0), (unsigned)(p1));
 }

 if (mon_inrep != NULL){
  if (fgets(&(line[0]), (int)(sizeof(line)), mon_inrep) != NULL){
   unsigned a;
   unsigned b;
   if (sscanf(&(line[0]), "%x %x", &a, &b) == 2){
    cu_ctr_setsnes(0U, (auint)(a));
    cu_ctr_setsnes(1U, (auint)(b));
   }
  }else{
   fclose(mon_inrep);
   mon_inrep = NULL;
  }
 }
}


static struct{
 char const* name;
 auint       mask;
}const mon_btn[] = {
 {"B",      CU_CTR_SNES_M_B},
 {"Y",      CU_CTR_SNES_M_Y},
 {"SELECT", CU_CTR_SNES_M_SELECT},
 {"START",  CU_CTR_SNES_M_START},
 {"UP",     CU_CTR_SNES_M_UP},
 {"DOWN",   CU_CTR_SNES_M_DOWN},
 {"LEFT",   CU_CTR_SNES_M_LEFT},
 {"RIGHT",  CU_CTR_SNES_M_RIGHT},
 {"A",      CU_CTR_SNES_M_A},
 {"X",      CU_CTR_SNES_M_X},
 {"L",      CU_CTR_SNES_M_LSH},
 {"R",      CU_CTR_SNES_M_RSH}
};

#define MON_BTNCOUNT (sizeof(mon_btn) / sizeof(mon_btn[0]))



/* ---------------------------------------------------------------------- */
/* Anomaly breaks                                                         */
/* ---------------------------------------------------------------------- */

static struct{
 char const* name;
 auint       id;
 boole       haspar;
}const mon_anom[] = {
 {"video-overrun",  CU_GDBBRK_AN_VIDEO_OVERRUN,  FALSE},
 {"stack-overflow", CU_GDBBRK_AN_STACK_OVERFLOW, TRUE },
 {"unmapped-io",    CU_GDBBRK_AN_UNMAPPED_IO,    FALSE},
 {"uninit-read",    CU_GDBBRK_AN_UNINIT_READ,    FALSE},
 {"illegal-opcode", CU_GDBBRK_AN_ILLEGAL_OPCODE, FALSE},
 {"irq-latency",    CU_GDBBRK_AN_IRQ_LATENCY,    TRUE }
};

#define MON_ANOMCOUNT (sizeof(mon_anom) / sizeof(mon_anom[0]))



/* ---------------------------------------------------------------------- */
/* Commands                                                               */
/* ---------------------------------------------------------------------- */

static char const* const mon_helptext[] = {
 "help                                 list these commands",
 "status                               halted/running, PC, cycle, frame, scanline",
 "cycles                               cycle counters",
 "ports [<name>]                       decoded ATmega644 I/O registers",
 "spiram read <addr> <len>             read the SPI RAM",
 "spiram write <addr> <hexbytes>       write the SPI RAM",
 "screenshot [<path>] [--overlay]      write a PNG of the framebuffer",
 "savestate [<path>]                   write a full state snapshot",
 "loadstate <path>                     restore a state snapshot",
 "reset                                power-cycle reset",
 "input <player> [<buttons>]           set a controller, or report it",
 "input <player> press <btns> [frames] hold buttons for a few frames",
 "input record <path>                  record controller state per frame",
 "input replay <path>                  replay a recording",
 "input stop                           stop recording and replaying",
 "key <name> [down|up|tap]             a host key, by SDL name (F9, Escape, Left_Ctrl)",
 "type <text>                          type into the keyboard dongle (\\n, \\t)",
 "  --wait[=<settle>]                  key/type: halt once the ROM has read it",
 "mouse <dx> <dy> [left|right|both]    move the SNES mouse",
 "frames [<n>]                         run n frames on resume, then halt",
 "speed [limited|unlimited]            the 60Hz frame limiter (F4)",
 "break-on [<name> [on|off|<par>]]     Uzebox anomaly breaks",
 "profile [clear|on|off]               per-scanline cycle cost",
 "overlay [on|off]                     the paused indicator panel (F5)",
 "reverse depth [<frames>]             reverse execution history depth",
 "",
 "every command above also takes --json",
 NULL
};


static void mon_do_status(void)
{
 cu_state_spir_t* sr = cu_spir_get_state();

 (void)cu_avr_get_state(); /* Refreshes TCNT1H / TCNT1L */

 mon_kv_str("state", cu_gdb_ishalted() ? "halted" : "running");
 mon_kv_hex("pc", (cpu_state.pc & 0x7FFFU) << 1, 6U);
 mon_kv_hex("sreg", cpu_state.iors[CU_IO_SREG], 2U);
 mon_kv_hex("sp", ((auint)(cpu_state.iors[CU_IO_SPH]) << 8) |
                   (auint)(cpu_state.iors[CU_IO_SPL]), 4U);
 mon_kv_num("cycle", (unsigned long long)(cu_gdb_getcycle64()));
 mon_kv_num("frame", (unsigned long long)(cu_gdb_getframe()));
 mon_kv_num("scanline", (unsigned long long)(video_pulsectr));
 mon_kv_num("scanline_cycle", (unsigned long long)(video_cycle));
 mon_kv_num("spiram_size", (unsigned long long)(sr->size));
 mon_kv_str("waited", mon_wait_str());
 mon_kv_str("sound", cu_gdb_starthidden() ? "muted" : "played");
}


static void mon_do_cycles(void)
{
 auint rem  = (video_cycle < MON_ROWCY) ? (MON_ROWCY - video_cycle) : 0U;
 auint frem = (video_pulsectr < 262U)
              ? (((262U - video_pulsectr) * MON_ROWCY) - video_cycle) : 0U;

 mon_kv_num("total", (unsigned long long)(cu_gdb_getcycle64()));
 mon_kv_num("since_stop", (unsigned long long)(cu_gdb_getsincestop()));
 mon_kv_num("scanline_remaining", (unsigned long long)(rem));
 mon_kv_num("frame_remaining", (unsigned long long)(frem));
}


static void mon_do_ports(char const* name)
{
 auint i;
 boole first = TRUE;

 (void)cu_avr_get_state();

 if (name != NULL){
  for (i = 0U; i < MON_REGCOUNT; i++){
   if (!mon_ieq(name, mon_regs[i].name)){ continue; }
   if (mon_json){ mon_key("registers"); mon_fadd("["); }
   mon_reg_print(&(mon_regs[i]), TRUE);
   if (mon_json){ mon_fadd("]"); }
   return;
  }
  mon_error("no such register: %s", name);
  return;
 }

 if (mon_json){ mon_key("registers"); mon_fadd("["); }
 for (i = 0U; i < MON_REGCOUNT; i++){
  if (mon_trunc){ break; }
  mon_reg_print(&(mon_regs[i]), first);
  first = FALSE;
 }
 if (mon_json){ mon_fadd("]"); }
}


static void mon_do_spiram_read(auint addr, auint len)
{
 cu_state_spir_t* sr = cu_spir_get_state();
 auint            i;

 if ((sr->ram == NULL) || (sr->size == 0U)){
  mon_error("no SPI RAM configured");
  return;
 }
 if (addr >= sr->size){
  mon_error("address 0x%X is past the end of the %u byte SPI RAM",
      (unsigned)(addr), (unsigned)(sr->size));
  return;
 }
 if (len > (sr->size - addr)){
  mon_error("length %u runs past the end of the SPI RAM", (unsigned)(len));
  return;
 }

 mon_kv_num("addr", (unsigned long long)(addr));
 mon_kv_num("len", (unsigned long long)(len));

 if (mon_json){
  mon_key("data");
  mon_fadd("\"");
  for (i = 0U; i < len; i++){
   if (mon_trunc){ break; }
   mon_add("%02X", (unsigned)(sr->ram[addr + i]));
  }
  mon_fadd("\"");
 }else{
  for (i = 0U; i < len; i += 16U){
   auint n = len - i;
   auint j;
   if (mon_trunc){ break; }
   if (n > 16U){ n = 16U; }
   mon_add("%06X:", (unsigned)(addr + i));
   for (j = 0U; j < n; j++){
    mon_add(" %02X", (unsigned)(sr->ram[addr + i + j]));
   }
   mon_add("\n");
  }
 }
}


static void mon_do_break_on(char const* name, char const* arg)
{
 auint i;
 boole ok;
 char  key[64];

 if (name == NULL){
  for (i = 0U; i < MON_ANOMCOUNT; i++){
   mon_kv_bool(mon_anom[i].name, cu_gdbbrk_anomaly_get(mon_anom[i].id));
   if (mon_anom[i].haspar){
    snprintf(&(key[0]), sizeof(key), "%s_par", mon_anom[i].name);
    mon_kv_num(&(key[0]),
        (unsigned long long)(cu_gdbbrk_anomaly_getpar(mon_anom[i].id)));
   }
  }
  return;
 }

 for (i = 0U; i < MON_ANOMCOUNT; i++){

  if (strcmp(name, mon_anom[i].name) != 0){ continue; }

  if       (arg == NULL){
   cu_gdbbrk_anomaly_set(mon_anom[i].id, TRUE);
  }else if (strcmp(arg, "on") == 0){
   cu_gdbbrk_anomaly_set(mon_anom[i].id, TRUE);
  }else if (strcmp(arg, "off") == 0){
   cu_gdbbrk_anomaly_set(mon_anom[i].id, FALSE);
  }else{
   auint v;
   if (!mon_anom[i].haspar){
    mon_error("%s takes no parameter", name);
    return;
   }
   v = mon_parsenum(arg, &ok);
   if (!ok){
    mon_error("expected on, off or a number, got %s", arg);
    return;
   }
   cu_gdbbrk_anomaly_par(mon_anom[i].id, v);
   cu_gdbbrk_anomaly_set(mon_anom[i].id, TRUE);
  }

  mon_kv_bool(name, cu_gdbbrk_anomaly_get(mon_anom[i].id));
  if (mon_anom[i].haspar){
   snprintf(&(key[0]), sizeof(key), "%s_par", name);
   mon_kv_num(&(key[0]),
       (unsigned long long)(cu_gdbbrk_anomaly_getpar(mon_anom[i].id)));
  }

  /* The uninit-read break needs a recognisable pattern in the SRAM to have
  ** anything to detect, and that is only laid down at reset. */
  if ( (mon_anom[i].id == CU_GDBBRK_AN_UNINIT_READ) &&
       (cu_gdbbrk_anomaly_get(mon_anom[i].id)) ){
   mon_kv_str("note", "run \"monitor reset\" to poison the SRAM first");
  }

  return;
 }

 mon_error("no such break condition: %s", name);
}


static void mon_do_profile(char const* arg)
{
 auint rows;
 auint total;
 auint worst;
 auint wrow;
 auint over;

 if (arg != NULL){
  if       (strcmp(arg, "clear") == 0){ cu_gdbbrk_profile_clear(); }
  else if  (strcmp(arg, "on"   ) == 0){ cu_gdbbrk_profile_ena(TRUE); }
  else if  (strcmp(arg, "off"  ) == 0){ cu_gdbbrk_profile_ena(FALSE); }
  else{
   mon_error("expected clear, on or off, got %s", arg);
   return;
  }
 }else{
  cu_gdbbrk_profile_ena(TRUE); /* Asking for a profile turns it on */
 }

 cu_gdbbrk_profile_get(&rows, &total, &worst, &wrow, &over);

 mon_kv_bool("enabled", cu_gdbbrk_profile_ison());
 mon_kv_num("rows", (unsigned long long)(rows));
 mon_kv_num("total_cycles", (unsigned long long)(total));
 mon_kv_num("avg_cycles",
     (unsigned long long)((rows != 0U) ? (total / rows) : 0U));
 mon_kv_num("worst_cycles", (unsigned long long)(worst));
 mon_kv_num("worst_row", (unsigned long long)(wrow));
 mon_kv_num("overruns", (unsigned long long)(over));
 mon_kv_num("budget", (unsigned long long)(CU_GDBBRK_ROWBUDGET));
}


/*
** The paused indicator panel. No argument reports rather than toggling: an
** agent which cannot see the screen should be able to ask without changing
** anything, and a blind flip is not a useful primitive for one.
*/
static void mon_do_overlay(char const* arg)
{
 if (arg != NULL){
  if       (strcmp(arg, "on" ) == 0){ cu_gdbovl_enable(TRUE); }
  else if  (strcmp(arg, "off") == 0){ cu_gdbovl_enable(FALSE); }
  else{
   mon_error("expected on or off, got %s", arg);
   return;
  }
 }

 mon_kv_bool("enabled", cu_gdbovl_isenabled());
}


/*
** Parses a comma separated list of button names into a mask.
*/
static boole mon_buttons(char const* s, auint* mask, char* bad, auint badsz)
{
 char  tok[32];
 auint n;
 auint j;

 *mask = 0U;

 while (*s != 0){

  n = 0U;
  while ((*s != 0) && (*s != ',') && (n < (sizeof(tok) - 1U))){
   tok[n] = *s;
   n ++;
   s ++;
  }
  tok[n] = 0;
  while ((*s != 0) && (*s != ',')){ s ++; } /* Skip an over-long token */
  if (*s == ','){ s ++; }

  if (n == 0U){ continue; }

  for (j = 0U; j < MON_BTNCOUNT; j++){
   if (mon_ieq(&(tok[0]), mon_btn[j].name)){ break; }
  }
  if (j == MON_BTNCOUNT){
   strncpy(bad, &(tok[0]), badsz - 1U);
   bad[badsz - 1U] = 0;
   return FALSE;
  }

  *mask |= mon_btn[j].mask;
 }

 return TRUE;
}


static void mon_do_input(char** av, auint ac)
{
 auint player;
 auint mask   = 0U;
 auint first;
 auint last;
 auint frames = 2U;
 boole press  = FALSE;
 auint i;
 boole ok;
 char  bad[32];

 if (ac < 2U){
  mon_error("usage: input <player> [press] <buttons> [frames]"
            " | record <path> | replay <path> | stop");
  return;
 }

 if (strcmp(av[1], "stop") == 0){
  if (mon_inrec != NULL){ fclose(mon_inrec); mon_inrec = NULL; }
  if (mon_inrep != NULL){ fclose(mon_inrep); mon_inrep = NULL; }
  mon_kv_bool("recording", FALSE);
  mon_kv_bool("replaying", FALSE);
  return;
 }

 if (strcmp(av[1], "record") == 0){
  if (ac < 3U){ mon_error("usage: input record <path>"); return; }
  if (mon_inrec != NULL){ fclose(mon_inrec); }
  mon_inrec = fopen(av[2], "w");
  if (mon_inrec == NULL){
   mon_error("cannot open %s for writing", av[2]);
   return;
  }
  mon_kv_str("recording", av[2]);
  return;
 }

 if (strcmp(av[1], "replay") == 0){
  if (ac < 3U){ mon_error("usage: input replay <path>"); return; }
  if (mon_inrep != NULL){ fclose(mon_inrep); }
  mon_inrep = fopen(av[2], "r");
  if (mon_inrep == NULL){ mon_error("cannot open %s", av[2]); return; }
  /* A recorded line is sampled at the top of a main loop iteration, before
  ** that iteration's host events, so line n is what frame n-1 ran with; a
  ** replayed line is applied at the same point and is what frame n runs
  ** with. Dropping the first line (the state before the first frame) lines
  ** the two up, so a replay reproduces the recorded play frame for frame. */
  {
   char skip[128];
   (void)(fgets(&(skip[0]), (int)(sizeof(skip)), mon_inrep));
  }
  mon_kv_str("replaying", av[2]);
  return;
 }

 player = mon_parsenum(av[1], &ok);
 if ((!ok) || (player > 1U)){ mon_error("player must be 0 or 1"); return; }

 /* Bare "input <player>" reports instead of erroring. An agent cannot see the
 ** screen, so being able to read the controller back is worth a command, and
 ** it is the only way to watch a timed press expire. */
 if (ac < 3U){
  (void)cu_ctr_getsnes_state(player, &mask);
  mon_kv_num("player", (unsigned long long)(player));
  mon_kv_hex("buttons", mask, 3U);
  mon_kv_num("frames_left", (unsigned long long)(mon_press_left[player]));
  return;
 }

 first = 2U;

 if (strcmp(av[2], "press") == 0){
  press = TRUE;
  first = 3U;
  if (ac < 4U){
   mon_error("usage: input <player> press <buttons> [frames]");
   return;
  }
 }

 last = ac;

 /* A trailing frame count, when there is something before it to be the
 ** buttons. No button is named a number, so there is nothing to disambiguate
 ** against. */
 if (press && (ac > (first + 1U))){
  auint f = mon_parsenum(av[ac - 1U], &ok);
  if (ok){
   if (f == 0U){ mon_error("frames must be 1 or more"); return; }
   frames = f;
   last   = ac - 1U;
  }
 }

 if (strcmp(av[first], "none") != 0){
  if ((av[first][0] == '0') &&
      ((av[first][1] == 'x') || (av[first][1] == 'X'))){
   mask = mon_parsenum(av[first], &ok);
   if (!ok){ mon_error("bad button mask: %s", av[first]); return; }
  }else{
   for (i = first; i < last; i++){
    auint part;
    if (!mon_buttons(av[i], &part, &(bad[0]), sizeof(bad))){
     mon_error("no such button: %s", &(bad[0]));
     return;
    }
    mask |= part;
   }
  }
 }

 cu_ctr_setsnes(player, mask);

 /* A plain set cancels a press in flight, rather than letting the old
 ** countdown clear buttons the caller has just asked for. */
 mon_press_left[player] = press ? frames : 0U;

 mon_kv_num("player", (unsigned long long)(player));
 mon_kv_hex("buttons", mask, 3U);
 mon_kv_num("frames_left", (unsigned long long)(mon_press_left[player]));
}


/*
** A host key, by SDL's own name for it: F9, Escape, Return, a, Left. Using
** SDL_GetKeyFromName() means there is no key table here to fall out of step
** with SDL's, and SDL_GetKeyName() echoes back what was actually resolved so
** a caller can tell a typo from a hit.
**
** What the key then does is entirely up to the emulator, exactly as if it had
** been typed: the F keys reach main.c's hotkey switch, letters reach the
** controller mapping, and everything reaches the keyboard dongle when a ROM
** has enabled it. Note that a dongle ROM makes main.c skip its switch (as it
** does for a real key), so the F keys stop working then, and that a key
** issued while the CPU is halted lands on resume, since main_loop() is not
** running to receive it -- "monitor overlay" is the way to reach the paused
** panel during a break.
*/
static void mon_do_key(char** av, auint ac)
{
 SDL_Keycode sym;
 char const* mode;
 boole       ok = TRUE;

 char  name[64];
 auint i;

 if (ac < 2U){ mon_error("usage: key <name> [down|up|tap]"); return; }

 mode = (ac > 2U) ? av[2] : "tap";

 /* An underscore stands in for a space, since the monitor line arrives already
 ** split on whitespace. Every modifier SDL names has one -- "Left Ctrl", "Left
 ** Shift", "Right Alt" -- as do "Caps Lock" and "Page Up", so without this no
 ** modifier could be pressed at all and Ctrl+<key> was untestable. */
 for (i = 0U; (i < (sizeof(name) - 1U)) && (av[1][i] != 0); i++){
  name[i] = (av[1][i] == '_') ? ' ' : av[1][i];
 }
 name[i] = 0;

 sym = SDL_GetKeyFromName(name);
 if (sym == SDLK_UNKNOWN){
  mon_error("no such key: %s (SDL key names, such as F9, Escape, Return, a; "
      "use '_' for a space, as in Left_Ctrl)", av[1]);
  return;
 }

 if       (strcmp(mode, "tap") == 0){
  /* Both edges or neither, so a full queue cannot leave a key stuck down */
  if (mon_keyq_count() > (MON_KEYQ_SZ - 3U)){ ok = FALSE; }
  else{
   ok = mon_key_push(sym, TRUE);
   if (ok){ ok = mon_key_push(sym, FALSE); }
  }
 }else if (strcmp(mode, "down") == 0){
  ok = mon_key_push(sym, TRUE);
 }else if (strcmp(mode, "up") == 0){
  ok = mon_key_push(sym, FALSE);
 }else{
  mon_error("expected down, up or tap, got %s", mode);
  return;
 }

 if (!ok){ mon_error("key queue full"); return; }

 if (mon_wait){ mon_wait_arm(); }

 mon_kv_str("key", SDL_GetKeyName(sym));
 mon_kv_str("mode", mode);
 mon_kv_num("queued", (unsigned long long)(mon_keyq_count()));
 mon_kv_bool("waiting", mon_wait);
}


/*
** ASCII to a US layout key plus whether shift is held. The emulated keyboard
** only knows unshifted keys and the shift key itself (see the scancode tables
** in cu_kbd.c), so a capital has to go out the way a person would produce
** one.
*/
static boole mon_char2key(char c, SDL_Keycode* sym, boole* shift)
{
 static char const mon_shdigit[] = ")!@#$%^&*(";  /* shift + 0..9 */
 char const*       p;

 *shift = FALSE;

 if ((c >= 'a') && (c <= 'z')){ *sym = SDLK_a + (c - 'a'); return TRUE; }
 if ((c >= '0') && (c <= '9')){ *sym = SDLK_0 + (c - '0'); return TRUE; }
 if ((c >= 'A') && (c <= 'Z')){
  *sym   = SDLK_a + (c - 'A');
  *shift = TRUE;
  return TRUE;
 }

 if (c != 0){
  p = strchr(&(mon_shdigit[0]), c);
  if (p != NULL){
   *sym   = SDLK_0 + (auint)(p - &(mon_shdigit[0]));
   *shift = TRUE;
   return TRUE;
  }
 }

 switch (c){
  case ' ':  *sym = SDLK_SPACE;                       return TRUE;
  case '\n': *sym = SDLK_RETURN;                      return TRUE;
  case '\t': *sym = SDLK_TAB;                         return TRUE;
  case '-':  *sym = SDLK_MINUS;                       return TRUE;
  case '=':  *sym = SDLK_EQUALS;                      return TRUE;
  case '[':  *sym = SDLK_LEFTBRACKET;                 return TRUE;
  case ']':  *sym = SDLK_RIGHTBRACKET;                return TRUE;
  case '\\': *sym = SDLK_BACKSLASH;                   return TRUE;
  case ';':  *sym = SDLK_SEMICOLON;                   return TRUE;
  case '\'': *sym = SDLK_QUOTE;                       return TRUE;
  case ',':  *sym = SDLK_COMMA;                       return TRUE;
  case '.':  *sym = SDLK_PERIOD;                      return TRUE;
  case '/':  *sym = SDLK_SLASH;                       return TRUE;
  case '`':  *sym = SDLK_BACKQUOTE;                   return TRUE;
  case '_':  *sym = SDLK_MINUS;        *shift = TRUE; return TRUE;
  case '+':  *sym = SDLK_EQUALS;       *shift = TRUE; return TRUE;
  case '{':  *sym = SDLK_LEFTBRACKET;  *shift = TRUE; return TRUE;
  case '}':  *sym = SDLK_RIGHTBRACKET; *shift = TRUE; return TRUE;
  case '|':  *sym = SDLK_BACKSLASH;    *shift = TRUE; return TRUE;
  case ':':  *sym = SDLK_SEMICOLON;    *shift = TRUE; return TRUE;
  case '\"': *sym = SDLK_QUOTE;        *shift = TRUE; return TRUE;
  case '<':  *sym = SDLK_COMMA;        *shift = TRUE; return TRUE;
  case '>':  *sym = SDLK_PERIOD;       *shift = TRUE; return TRUE;
  case '?':  *sym = SDLK_SLASH;        *shift = TRUE; return TRUE;
  case '~':  *sym = SDLK_BACKQUOTE;    *shift = TRUE; return TRUE;
  default: break;
 }

 return FALSE;
}


/*
** Types a string. The tokens are joined with single spaces, since the command
** line has already been split on whitespace, and surrounding quotes are
** dropped if present.
**
** "\n" and "\t" are understood as escapes ("\\" for a literal backslash),
** because the monitor line cannot carry a real newline: it arrives already
** split on whitespace, so a newline would simply end the argument. Without
** them there is no way to press Return in the middle of a string, which is
** most of the point of typing into a ROM that reads lines.
**
** "\"" types a literal quote, which is the only way to send a string that is
** itself quoted end to end -- a BASIC LOAD "NAME" as the whole text. The
** unquoting is reported back as "unquoted" when it fires, since two missing
** characters otherwise present as the ROM mis-parsing a correct command.
**
** One key event goes out per frame, so the reported "frames" is how long the
** string takes to arrive -- worth reading, because a shifted character costs
** four events rather than two.
*/
static void mon_do_type(char** av, auint ac)
{
 char        text[512];
 auint       n = 0U;
 auint       i;
 auint       j;
 auint       keys  = 0U;
 auint       queued;
 SDL_Keycode sym;
 boole       shift;
 boole       unquoted = FALSE;

 if (ac < 2U){ mon_error("usage: type <text>"); return; }

 for (i = 1U; i < ac; i++){
  if ((i > 1U) && (n < (sizeof(text) - 1U))){ text[n] = ' '; n ++; }
  for (j = 0U; av[i][j] != 0; j++){
   if (n >= (sizeof(text) - 1U)){ break; }
   text[n] = av[i][j];
   n ++;
  }
 }
 text[n] = 0;

 if ((n >= 2U) && (text[0] == '\"') && (text[n - 1U] == '\"')){
  text[n - 1U] = 0;
  memmove(&(text[0]), &(text[1]), n - 1U);
  n -= 2U;
  unquoted = TRUE;
 }

 /* Escapes, collapsed in place */
 j = 0U;
 for (i = 0U; i < n; i++){
  if ((text[i] == '\\') && ((i + 1U) < n)){
   i ++;
   switch (text[i]){
    case 'n':  text[j] = '\n'; break;
    case 't':  text[j] = '\t'; break;
    case '\\': text[j] = '\\'; break;
    /* The way to type a string that is itself quoted, such as a BASIC
    ** LOAD "NAME": the unquoting above would otherwise eat both quotes,
    ** and this runs after it, so \" never reaches that test. */
    case '\"': text[j] = '\"'; break;
    default:
     mon_error("unknown escape \\%c at offset %u",
         text[i], (unsigned)(i - 1U));
     return;
   }
  }else{
   text[j] = text[i];
  }
  j ++;
 }
 n       = j;
 text[n] = 0;

 queued = mon_keyq_count();

 for (i = 0U; i < n; i++){

  if (!mon_char2key(text[i], &sym, &shift)){
   mon_error("cannot type character 0x%02X at offset %u",
       (unsigned)((unsigned char)(text[i])), (unsigned)(i));
   return;
  }

  /* Room for the whole character, so a full queue cannot strand a shift */
  if (mon_keyq_count() > (MON_KEYQ_SZ - 6U)){
   mon_error("key queue full after %u characters", (unsigned)(keys));
   return;
  }

  if (shift){ (void)mon_key_push(SDLK_LSHIFT, TRUE); }
  (void)mon_key_push(sym, TRUE);
  (void)mon_key_push(sym, FALSE);
  if (shift){ (void)mon_key_push(SDLK_LSHIFT, FALSE); }

  keys ++;
 }

 if (mon_wait){ mon_wait_arm(); }

 mon_kv_num("characters", (unsigned long long)(keys));
 mon_kv_num("frames", (unsigned long long)(mon_keyq_count() - queued));
 /* Only when it happened, so the usual reply is unchanged. Dropping a pair of
 ** quotes is silent otherwise, and the two characters missing from a string
 ** the ROM then mis-parses look exactly like a bug in the ROM. */
 if (unquoted){ mon_kv_bool("unquoted", TRUE); }
 mon_kv_bool("waiting", mon_wait);
}


/*
** The SNES mouse. This writes the same state the real mouse path writes in
** ginput_sendevent(), a movement delta and a button mask.
**
** Be aware that cu_mouse_process() is commented out in this tree, so nothing
** hands that state to the ROM yet: the command sets the emulator's mouse
** state and reads it back, and will drive a game the moment that function is
** filled in, but it is not an end to end input path today.
*/
static void mon_do_mouse(char** av, auint ac)
{
 sint32 dx;
 sint32 dy;
 auint  btn = 0U;
 sint32 rx;
 sint32 ry;
 boole  ok;

 if (ac < 3U){
  mon_error("usage: mouse <dx> <dy> [left|right|both|none]");
  return;
 }

 dx = mon_parsesnum(av[1], &ok);
 if (!ok){ mon_error("bad dx: %s", av[1]); return; }
 dy = mon_parsesnum(av[2], &ok);
 if (!ok){ mon_error("bad dy: %s", av[2]); return; }

 if (ac > 3U){
  if       (mon_ieq(av[3], "none" )){ btn = 0U; }
  else if  (mon_ieq(av[3], "left" )){ btn = SDL_BUTTON_LMASK; }
  else if  (mon_ieq(av[3], "right")){ btn = SDL_BUTTON_RMASK; }
  else if  (mon_ieq(av[3], "both" )){ btn = SDL_BUTTON_LMASK | SDL_BUTTON_RMASK; }
  else{
   mon_error("expected left, right, both or none, got %s", av[3]);
   return;
  }
 }else{
  btn = cu_mouse_get_buttons();
 }

 cu_mouse_set_position(dx, dy);
 cu_mouse_set_buttons((uint8)(btn));

 cu_mouse_get_position(&rx, &ry);
 mon_kv_snum("dx", (long long)(rx));
 mon_kv_snum("dy", (long long)(ry));
 mon_kv_hex("buttons", cu_mouse_get_buttons(), 2U);
 mon_kv_bool("enabled", cu_mouse_get_enabled() != 0U);
}


/*
** Runs a definite number of frames and then halts. This is what makes
** injected input verifiable: press, advance a known number of frames, look at
** the result -- rather than sleeping and hoping, which does not map to frames
** at all (the emulator can be free running or paused).
**
** Arms and returns; the caller then continues. The halt itself goes through
** cu_gdbbrk_halt_req(), the same request Ctrl-C uses, so it is reported by
** the existing stop path.
*/
static void mon_do_frames(char const* arg)
{
 boole ok;

 if (arg != NULL){
  auint n = mon_parsenum(arg, &ok);
  if (!ok){ mon_error("bad frame count: %s", arg); return; }
  mon_runframes = n;
 }

 mon_kv_num("frames", (unsigned long long)(mon_runframes));
 mon_kv_bool("armed", mon_runframes != 0U);
}


/*
** "speed [limited|unlimited]". The change lands on the next main loop
** iteration, so from a halted target that is after the resume: "pending"
** says it has been asked for and not yet made.
*/
static void mon_do_speed(char const* arg)
{
 if (arg != NULL){
  if       (strcmp(arg, "unlimited") == 0){ mon_nolimit_req = TRUE;
  }else if (strcmp(arg, "limited")   == 0){ mon_nolimit_req = FALSE;
  }else{ mon_error("bad speed: %s (limited or unlimited)", arg); return; }
  mon_nolimit_pending = (boole)(mon_nolimit_req != mon_nolimit_cur);
 }

 mon_kv_str("speed", (mon_nolimit_pending ? mon_nolimit_req : mon_nolimit_cur) ?
                     "unlimited" : "limited");
 mon_kv_bool("pending", mon_nolimit_pending);
}


boole cu_gdbmon_nolimit_sync(boole cur)
{
 mon_nolimit_cur = cur;
 if (mon_nolimit_pending){
  mon_nolimit_pending = FALSE;
  mon_nolimit_cur     = mon_nolimit_req;
 }
 return mon_nolimit_cur;
}


static void mon_do_spiram_write(char const* addrs, char const* hex)
{
 cu_state_spir_t* sr = cu_spir_get_state();
 auint            addr;
 auint            n = 0U;
 boole            ok;
 asint            hi;
 asint            lo;

 addr = mon_parsenum(addrs, &ok);
 if (!ok){ mon_error("bad address: %s", addrs); return; }

 if ((sr->ram == NULL) || (sr->size == 0U)){
  mon_error("no SPI RAM configured");
  return;
 }

 while (hex[0] != 0){
  hi = mon_nibble(hex[0]);
  lo = (hex[1] != 0) ? mon_nibble(hex[1]) : -1;
  if ((hi < 0) || (lo < 0)){
   mon_error("bad hex byte at offset %u", (unsigned)(n));
   return;
  }
  if ((addr + n) >= sr->size){
   mon_error("write runs past the end of the SPI RAM");
   return;
  }
  sr->ram[addr + n] = (uint8)((hi << 4) | lo);
  n ++;
  hex += 2;
 }

 mon_kv_num("addr", (unsigned long long)(addr));
 mon_kv_num("written", (unsigned long long)(n));
}



/*
** Splits a command line into whitespace separated tokens.
*/
static auint mon_split(char* s, char** av, auint maxv)
{
 auint ac = 0U;

 while ((*s != 0) && (ac < maxv)){
  while ((*s == ' ') || (*s == '\t') || (*s == '\n') || (*s == '\r')){
   *s = 0;
   s ++;
  }
  if (*s == 0){ break; }
  av[ac] = s;
  ac ++;
  while ((*s != 0) && (*s != ' ') && (*s != '\t') &&
         (*s != '\n') && (*s != '\r')){ s ++; }
 }

 return ac;
}



void  cu_gdbmon_exec(char const* cmd)
{
 char  line[1024];
 char* av[32];
 auint ac;
 auint i;
 auint o;
 boole ok;
 char  path[1024];
 char  abs[1024];
 char  err[256];

 strncpy(&(line[0]), cmd, sizeof(line) - 1U);
 line[sizeof(line) - 1U] = 0;

 ac = mon_split(&(line[0]), &(av[0]), 32U);

 /* Flags are accepted wherever they sit on the line */
 mon_json    = FALSE;
 mon_overlay = FALSE;
 mon_wait    = FALSE;
 mon_waitsettle = MON_WAIT_SETTLE;

 /* --json is picked up on a pass of its own, before any flag that can fail to
 ** parse. Otherwise "--wait=zz --json" would report its error as plain text,
 ** the flag having been rejected before the one asking for JSON was reached,
 ** and a caller parsing the reply would see the contract broken exactly when
 ** it had made a mistake. */
 for (i = 0U; i < ac; i++){
  if (strcmp(av[i], "--json") == 0){ mon_json = TRUE; }
 }

 o = 0U;
 for (i = 0U; i < ac; i++){
  if (strcmp(av[i], "--json"   ) == 0){ continue; }
  if (strcmp(av[i], "--overlay") == 0){ mon_overlay = TRUE; continue; }
  if (strcmp(av[i], "--wait"   ) == 0){ mon_wait    = TRUE; continue; }
  if (strncmp(av[i], "--wait=", 7U) == 0){
   auint n = mon_parsenum(&(av[i][7]), &ok);
   if (!ok){
    mon_begin();
    mon_error("bad settle frame count: %s", &(av[i][7]));
    return;
   }
   mon_wait       = TRUE;
   mon_waitsettle = n;
   continue;
  }
  av[o] = av[i];
  o ++;
 }
 ac = o;

 mon_begin();

 if (ac == 0U){
  mon_error("empty command, try \"monitor help\"");
  return;
 }

 if (strcmp(av[0], "help") == 0){
  if (mon_json){
   mon_key("commands");
   mon_fadd("[");
   for (i = 0U; mon_helptext[i] != NULL; i++){
    if (mon_helptext[i][0] == 0){ continue; }
    mon_add("%s\"%s\"", (i != 0U) ? "," : "", mon_helptext[i]);
   }
   mon_fadd("]");
  }else{
   for (i = 0U; mon_helptext[i] != NULL; i++){
    mon_add("%s\n", mon_helptext[i]);
   }
  }

 }else if (strcmp(av[0], "status") == 0){
  mon_do_status();

 }else if (strcmp(av[0], "cycles") == 0){
  mon_do_cycles();

 }else if (strcmp(av[0], "ports") == 0){
  mon_do_ports((ac > 1U) ? av[1] : NULL);

 }else if (strcmp(av[0], "spiram") == 0){
  if       (ac < 2U){
   mon_error("usage: spiram read <addr> <len> | write <addr> <hexbytes>");
  }else if (strcmp(av[1], "read") == 0){
   auint addr;
   auint len;
   if (ac < 4U){
    mon_error("usage: spiram read <addr> <len>");
   }else{
    addr = mon_parsenum(av[2], &ok);
    if (!ok){ mon_error("bad address: %s", av[2]); return; }
    len = mon_parsenum(av[3], &ok);
    if (!ok){ mon_error("bad length: %s", av[3]); return; }
    mon_do_spiram_read(addr, len);
   }
  }else if (strcmp(av[1], "write") == 0){
   if (ac < 4U){ mon_error("usage: spiram write <addr> <hexbytes>"); }
   else        { mon_do_spiram_write(av[2], av[3]); }
  }else{
   mon_error("expected read or write, got %s", av[1]);
  }

 }else if (strcmp(av[0], "screenshot") == 0){
  auint  w;
  auint  h;
  uint8* rgb;
  boole  hid = FALSE;
  if (ac > 1U){
   strncpy(&(path[0]), av[1], sizeof(path) - 1U);
   path[sizeof(path) - 1U] = 0;
  }else{
   mon_tmppath(&(path[0]), sizeof(path), "png");
  }
  /* While halted the paused indicator sits over the middle of the game
  ** area, which is rarely what a capture is wanted for, so it is lifted for
  ** the grab and put back afterwards. --overlay keeps it, which is how the
  ** indicator itself gets checked. */
  if (!mon_overlay){ hid = cu_gdbovl_hide(); }
  rgb = mon_grab(&w, &h);
  if (hid){ cu_gdbovl_show(); }
  if (rgb == NULL){ mon_error("cannot read the framebuffer"); return; }
  if (!png_write(&(path[0]), rgb, w, h)){
   free(rgb);
   mon_error("cannot write %s", &(path[0]));
   return;
  }
  free(rgb);
  mon_abspath(&(path[0]), &(abs[0]), sizeof(abs));
  mon_kv_str("path", &(abs[0]));
  mon_kv_num("width", (unsigned long long)(w));
  mon_kv_num("height", (unsigned long long)(h));

 }else if (strcmp(av[0], "savestate") == 0){
  if (ac > 1U){
   strncpy(&(path[0]), av[1], sizeof(path) - 1U);
   path[sizeof(path) - 1U] = 0;
  }else{
   mon_tmppath(&(path[0]), sizeof(path), "cuzstate");
  }
  if (!cu_gdb_state_save(&(path[0]), &(err[0]), sizeof(err))){
   mon_error("%s", &(err[0]));
   return;
  }
  mon_abspath(&(path[0]), &(abs[0]), sizeof(abs));
  mon_kv_str("path", &(abs[0]));

 }else if (strcmp(av[0], "loadstate") == 0){
  if (ac < 2U){ mon_error("usage: loadstate <path>"); return; }
  if (!cu_gdb_state_load(av[1], &(err[0]), sizeof(err))){
   mon_error("%s", &(err[0]));
   return;
  }
  mon_kv_str("loaded", av[1]);

 }else if (strcmp(av[0], "reset") == 0){
  cu_gdb_reset_target();
  mon_kv_str("reset", "ok");

 }else if (strcmp(av[0], "input") == 0){
  mon_do_input(&(av[0]), ac);

 }else if (strcmp(av[0], "key") == 0){
  mon_do_key(&(av[0]), ac);

 }else if (strcmp(av[0], "type") == 0){
  mon_do_type(&(av[0]), ac);

 }else if (strcmp(av[0], "mouse") == 0){
  mon_do_mouse(&(av[0]), ac);

 }else if (strcmp(av[0], "frames") == 0){
  mon_do_frames((ac > 1U) ? av[1] : NULL);

 }else if (strcmp(av[0], "speed") == 0){
  mon_do_speed((ac > 1U) ? av[1] : NULL);

 }else if (strcmp(av[0], "break-on") == 0){
  mon_do_break_on((ac > 1U) ? av[1] : NULL, (ac > 2U) ? av[2] : NULL);

 }else if (strcmp(av[0], "overlay") == 0){
  mon_do_overlay((ac > 1U) ? av[1] : NULL);

 }else if (strcmp(av[0], "profile") == 0){
  mon_do_profile((ac > 1U) ? av[1] : NULL);

 }else if (strcmp(av[0], "reverse") == 0){
  if ((ac > 2U) && (strcmp(av[1], "depth") == 0)){
   auint d = mon_parsenum(av[2], &ok);
   if (!ok){ mon_error("bad depth: %s", av[2]); return; }
   cu_gdb_reverse_setdepth(d);
  }
  mon_kv_num("depth", (unsigned long long)(cu_gdb_reverse_getdepth()));

 }else{
  mon_error("no such command: %s, try \"monitor help\"", av[0]);
 }

 mon_end();
}



#endif /* CU_GDB */
