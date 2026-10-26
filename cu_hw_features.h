#ifndef CU_HW_FEATURES_H
#define CU_HW_FEATURES_H
#ifndef FLAG_AVR_TIMER1_CLOCK
#define FLAG_AVR_TIMER1_CLOCK 0
#endif
#ifndef FLAG_AVR_GPIO
#define FLAG_AVR_GPIO 0
#endif
#if (FLAG_AVR_TIMER1_CLOCK != 0 && FLAG_AVR_TIMER1_CLOCK != 1) || \
    (FLAG_AVR_GPIO != 0 && FLAG_AVR_GPIO != 1)
#error "AVR hardware model flags must be 0 or 1"
#endif
#define CU_HW_FEATURES ((FLAG_AVR_TIMER1_CLOCK ? 1U : 0U) | (FLAG_AVR_GPIO ? 2U : 0U))
#endif
