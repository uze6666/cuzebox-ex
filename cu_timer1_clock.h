/* Internal Timer1 clock and shared synchronous prescaler phase. */
#ifndef CU_TIMER1_CLOCK_H
#define CU_TIMER1_CLOCK_H
#include <stdint.h>
typedef struct {
 uint32_t cycle, phase_origin;
 uint16_t count;
 uint8_t source, held;
} cu_timer1_clock_t;
static inline uint32_t cu_timer1_period(const cu_timer1_clock_t *s)
{
 static const uint16_t periods[8] = {0, 1, 8, 64, 256, 1024, 0, 0};
 return s->held ? 0U : periods[s->source & 7U];
}
static inline uint32_t cu_timer1_advance(cu_timer1_clock_t *s, uint32_t cycle)
{
 uint32_t delta = cycle - s->cycle;
 uint32_t period = cu_timer1_period(s), ticks = 0U;
 if (period){
  uint32_t first = period - ((s->cycle - s->phase_origin) & (period - 1U));
  if (delta >= first){ ticks = 1U + (delta - first) / period; }
 }
 s->count = (uint16_t)(s->count + ticks);
 s->cycle = cycle;
 return ticks;
}
/* ticks is a positive number of future counter edges. */
static inline uint32_t cu_timer1_until(const cu_timer1_clock_t *s, uint32_t ticks)
{
 uint32_t period = cu_timer1_period(s);
 return period ? period - ((s->cycle - s->phase_origin) & (period - 1U)) +
                 (ticks - 1U) * period : UINT32_MAX;
}
#endif
