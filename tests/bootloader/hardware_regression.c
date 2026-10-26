#define TIMER1_TEST_ENTRY timer1_regression_main
#include "timer1_regression.c"
#if FLAG_AVR_GPIO
#define cu_avr_exec cu_avr_exec_table
#include "../../cu_avr_e.h"
#undef cu_avr_exec
#endif

#if FLAG_AVR_TIMER1_CLOCK
static void advance_to(auint cycle)
{
 while ((uint32)(cycle_next_event-cpu_state.cycle) <= (uint32)(cycle-cpu_state.cycle)){
  next();
 }
 cpu_state.cycle=cycle;
}

static int timer_clock_test(void)
{
 static const auint periods[]={1U,8U,64U,256U,1024U};
 for(auint i=0U;i<5U;i++){
  auint period=periods[i];
  setup(0U,0U,9U,19U,FALSE);
  cu_avr_write_io(CU_IO_TCCR1B,i+1U);
  advance_to(period*10U-1U);
  CHECK(timer1_value()==9U && !(cpu_state.iors[CU_IO_TIFR1]&2U),"divider compare not early");
  advance_to(period*10U);
  CHECK(timer1_value()==10U && (cpu_state.iors[CU_IO_TIFR1]&2U),"divider compare at deadline");
  advance_to(period*80U);
  CHECK(timer1_value()==80U,"all internal dividers count timer ticks");
 }

 setup(0U,7U,99U,199U,FALSE);
 cu_avr_write_io(CU_IO_TCCR1B,2U);
 advance_to(3U);
 cu_avr_write_io(CU_IO_TCCR1B,0U);
 advance_to(79U);
 CHECK(timer1_value()==7U,"stopped counter remains frozen");
 cu_avr_write_io(CU_IO_TCCR1B,2U);
 advance_to(80U);
 CHECK(timer1_value()==8U,"restart uses free-running prescaler phase");
 cu_avr_write_io(CU_IO_TCCR1B,3U);
 advance_to(127U);CHECK(timer1_value()==8U,"divider change preserves shared phase");
 advance_to(128U);CHECK(timer1_value()==9U,"changed divider edge");

 cu_avr_write_io(CU_IO_GTCCR,0x81U);
 advance_to(250U);CHECK(timer1_value()==9U,"TSM holds synchronous prescaler reset");
 cu_avr_write_io(CU_IO_GTCCR,0U);
 advance_to(313U);CHECK(timer1_value()==9U,"release restarts divider phase");
 advance_to(314U);CHECK(timer1_value()==10U,"first edge after release");
 cu_avr_write_io(CU_IO_GTCCR,1U);
 CHECK(!(cpu_state.iors[CU_IO_GTCCR]&1U),"PSRSYNC strobe auto-clears");
 advance_to(378U);CHECK(timer1_value()==11U,"PSRSYNC resets running divider");

 setup(0U,1U,9U,19U,FALSE);
 cu_avr_write_io(CU_IO_TCCR1B,2U);
 write_counter(9U);
 advance_to(7U);CHECK(timer1_compare_blocked,"TCNT inhibition waits for actual timer clock");
 advance_to(8U);CHECK(!timer1_compare_blocked && !(cpu_state.iors[CU_IO_TIFR1]&2U),"TCNT inhibition consumes one divided tick");
 advance_to(88U);CHECK(cpu_state.iors[CU_IO_TIFR1]&4U,"divided B compare survives blocked A");
 setup(0U,1U,9U,19U,TRUE);
 cu_avr_write_io(CU_IO_TCCR1B,10U);
 advance_to(72U);CHECK(timer1_value()==0U && (cpu_state.iors[CU_IO_TIFR1]&2U),"divided CTC clear");
 advance_to(152U);CHECK(timer1_value()==0U,"divided CTC repeats without phase drift");

 setup(0xfffffff8U,0U,9U,19U,FALSE);
 cu_avr_write_io(CU_IO_TCCR1B,2U);
 advance_to(72U);
 CHECK(timer1_value()==10U && (cpu_state.iors[CU_IO_TIFR1]&2U),"divider crosses CPU cycle wrap");
 return 0;
}
#endif

#if FLAG_AVR_GPIO
static void opcode(auint pc,auint word)
{
 cpu_state.crom[pc*2U]=word&255U;
 cpu_state.crom[pc*2U+1U]=word>>8;
 cu_avr_crom_update(pc*2U,2U);
}
static int gpio_test(void (*step)(void))
{
 memset(&cpu_state,0,sizeof(cpu_state));
 cpu_state.fuse[1]=0xd3U;
 cu_avr_reset();
 for(auint p=0U;p<4U;p++){
  auint pin=CU_IO_PINA+p*3U;
  cu_avr_write_io(pin+2U,0xa5U);
  CHECK(cpu_state.iors[pin+2U]==0xa5U,"PORT retains latch while input");
  cu_avr_write_io(pin+1U,0xffU);
  CHECK(cu_avr_read_io(pin)==0xa5U,"PIN reads output latch on each port");
  cu_avr_write_io(pin,0x24U);
  CHECK(cu_avr_read_io(pin)==0x81U,"PIN write-one toggles latch on each port");
 }
 cu_avr_write_io(CU_IO_DDRB,0U);
 cu_avr_write_io(CU_IO_PORTB,0xa5U);
 CHECK(cu_avr_read_io(CU_IO_PINB)==0xa5U,"input pullups follow PORT latch");
 cu_avr_write_io(CU_IO_MCUCR,0x10U);
 CHECK(cu_avr_read_io(CU_IO_PINB)==0U,"PUD disables internal pullups");
 cu_avr_gpio_set_input(1U,0xffU,0x5aU);
 CHECK(cu_avr_read_io(CU_IO_PINB)==0x5aU,"external input fixture overrides pulls");
 cu_avr_write_io(CU_IO_DDRB,0x0fU);
 CHECK(cu_avr_read_io(CU_IO_PINB)==0x55U,"mixed input and output resolution");
 cu_avr_write_io(CU_IO_PORTC,0xa5U);
 cu_avr_write_io(CU_IO_DDRC,0xf0U);
 CHECK(CU_AVR_PIXEL_VALUE==0xa0U && cpu_state.iors[CU_IO_PORTC]==0xa5U,"pixel cache masks DDR without losing latch");
 cpu_state.iors[16]=0x5aU;
 opcode(0U,0xb908U); /* OUT PORTC,r16: optimized PIXEL instruction. */
 cpu_state.pc=0U;cycle_next_event=cpu_state.cycle+100U;
 step();
 CHECK(cpu_state.iors[CU_IO_PORTC]==0x5aU && CU_AVR_PIXEL_VALUE==0x50U,"decoder PIXEL keeps latch and effective output");
 cu_avr_write_io(CU_IO_DDRC,0xffU);
 CHECK(CU_AVR_PIXEL_VALUE==0x5aU,"DDR change reveals retained pixel latch");
 return 0;
}
#endif

static int state_test(void)
{
 savestate_blob_t state={0};
 uint32 phase,mask,value;
 main_headless=TRUE;cu_avr_reset();cu_spir_set_size(1U);cu_vfat_reset();
 setup(0U,0U,99U,199U,FALSE);
#if FLAG_AVR_TIMER1_CLOCK
 cu_avr_write_io(CU_IO_TCCR1B,2U);
 advance_to(5U);
#endif
 cu_avr_gpio_set_input(1U,0xffU,0x5aU);
 CHECK(savestate_capture_to_mem(&state) && savestate_validate_blob(&state),"strict snapshot validation matches build");
 cu_avr_hardware_state(&phase,&mask,&value);
 cu_avr_hardware_restore(0U,0U,0U);
 CHECK(savestate_restore_from_mem(&state),"restore hardware snapshot");
 {
  uint32 p,m,v;cu_avr_hardware_state(&p,&m,&v);
  CHECK(p==phase && m==mask && v==value,"snapshot restores divider phase and external inputs");
 }
#if FLAG_AVR_TIMER1_CLOCK
 advance_to(8U);CHECK(timer1_value()==1U,"restored fractional prescaler reaches original next edge");
#endif
#if CU_HW_FEATURES
 {
  uint32 features=CU_HW_FEATURES^1U;
  auint cycle=cpu_state.cycle;
  memcpy(state.data+state.size-16U,&features,sizeof(features));
  CHECK(!savestate_validate_blob(&state) && !savestate_restore_from_mem(&state),"different compiled model rejected");
  CHECK(cpu_state.cycle==cycle,"model rejection precedes state mutation");
 }
#endif
 savestate_free_blob(&state);
 return 0;
}
int main(void)
{
#if FLAG_AVR_TIMER1_CLOCK
 CHECK(!timer_clock_test(),"Timer1 internal clock model");
#endif
#if FLAG_AVR_GPIO
 CHECK(!gpio_test(cu_avr_exec),"switch decoder GPIO");
 CHECK(!gpio_test(cu_avr_exec_table),"table decoder GPIO");
#endif
 CHECK(!state_test(),"hardware snapshots");
 printf("Hardware regression: PASS (Timer1 clock=%u, GPIO=%u)\n",FLAG_AVR_TIMER1_CLOCK,FLAG_AVR_GPIO);
 return 0;
}
