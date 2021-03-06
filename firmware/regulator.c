#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/l1/adc.h>
#include <libopencm3/cm3/nvic.h>

#include "regulator.h"

static const uint32_t vsense1_ch = ADC_CHANNEL4;
static const uint32_t isense1_ch = ADC_CHANNEL3;
static const uint32_t vsense2_ch = ADC_CHANNEL21;
static const uint32_t isense2_ch = ADC_CHANNEL20;

static enum tim_oc_id ch2_oc = TIM_OC3;

struct feedback_gains {
  fixed32_t prop_gain1, prop_gain2; // gains for each channel
};
  
/*
 * This ties together the various parameters needed by a single
 * channel feedback loop. 
 */
struct regulator_t {
  uint32_t vsense_gain; // codepoints per volt
  uint32_t isense_gain; // codepoints per amp
  uint32_t period; // period in cycles
  fract32_t duty1;
  fract32_t duty2;
  uint16_t isense; // current in codepoints
  uint16_t vsense; // voltage in codepoints
  enum feedback_mode mode;
  uint16_t isetpoint, vlimit; // in codepoints, only used in current_fb mode
  uint16_t vsetpoint, ilimit; // in codepoints, only used in voltage_fb mode
  struct feedback_gains i_gains, v_gains;
  fixed32_t i1_prop_gain, i2_prop_gain; // current feedback gains
  void (*enable_func)(void);
  int (*configure_func)(void);
  void (*disable_func)(void);
  void (*update_duty_func)(void);
};

static void enable_ch1(void);
static int configure_ch1(void);
static void disable_ch1(void);
static void update_duty_ch1(void);

struct regulator_t chan1 = {
  .period = 2000000 / 5000,
  .mode = DISABLED,
  .vsense_gain = (1<<12) / 3.3 * 33/(33+68),
  .isense_gain = (1<<12) / (3.3 / 0.05 / 10),
  .vlimit = 0xffff,
  .ilimit = 0xffff,
  .v_gains = { 0x10000, 0x10000 },
  .i_gains = { 0x10000, 0x10000 },
  .enable_func = enable_ch1,
  .configure_func = configure_ch1,
  .disable_func = disable_ch1,
  .update_duty_func = update_duty_ch1
};

static void enable_ch2(void);
static int configure_ch2(void);
static void disable_ch2(void);
static void update_duty_ch2(void);

struct regulator_t chan2 = {
  .period = 2000000 / 5000,
  .mode = DISABLED,
  .vsense_gain = (1<<12) / 3.3 * 33/(33+68),
  .isense_gain = (1<<12) / (3.3 / 0.05 / 47),
  .vlimit = 0xffff,
  .ilimit = 0xffff,
  .v_gains = { 0x10000, 0x10000 },
  .i_gains = { 0x10000, 0x10000 },
  .enable_func = enable_ch2,
  .configure_func = configure_ch2,
  .disable_func = disable_ch2,
  .update_duty_func = update_duty_ch2
};

/*
 * Switching voltage regulator core
 *
 * This is the logic for driving the two switching regulator channels.
 * Channel 1 is a buck-boost regulator with both current and voltage sensing.
 * Channel 2 is a buck regulator with only voltage sensing.
 * 
 *  == Common peripherals ==
 *
 *   ADC1:   Sample voltages and current sense
 *   TIM7:   ADC trigger
 *   GPIOA:  MOSFET driver enable
 * 
 *  == Channel 1 peripherals ==
 *
 *   TIM2:   Buck regulator switch PWM
 *   TIM4:   Boost regulator switch PWM
 *
 *  == Channel 2 peripherals ==
 *
 *   TIM3:   Buck regulator switch PWM
 *
 */

static void setup_common_peripherals(void)
{
  uint8_t sequence[] = { vsense1_ch, isense1_ch, vsense2_ch, isense2_ch };

  if (chan1.mode == DISABLED && chan2.mode == DISABLED) {
    if (RCC_APB2ENR & RCC_APB2ENR_ADC1EN)
      adc_off(ADC1);
    rcc_peripheral_disable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM7EN);
    rcc_peripheral_disable_clock(&RCC_APB2ENR, RCC_APB2ENR_ADC1EN);
    rcc_osc_off(HSI);
  } else {
    // ADCCLK is derived from HSI
    rcc_osc_on(HSI);
    rcc_wait_for_osc_ready(HSI);

    rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM7EN);
    rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_ADC1EN);

    nvic_enable_irq(NVIC_ADC1_IRQ);
    adc_enable_external_trigger_injected(ADC1, ADC_CR2_JEXTEN_RISING,
                                         ADC_CR2_JEXTSEL_TIM7_TRGO);
    adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_96CYC);
    //adc_set_resolution(ADC1, ADC_CR1_RES_12BIT);
    adc_enable_eoc_interrupt_injected(ADC1);
    adc_set_clk_prescale(ADC_CCR_ADCPRE_DIV4);
    adc_set_injected_sequence(ADC1, 4, sequence);
    adc_enable_scan_mode(ADC1);
    adc_power_on(ADC1);
    while (!(ADC1_SR & ADC_SR_ADONS));
    while (ADC1_SR & ADC_SR_JCNR);

    timer_reset(TIM7);
    timer_continuous_mode(TIM7);
    timer_set_prescaler(TIM7, 0x1);
    timer_set_period(TIM7, 2097000 / 1000);
    timer_set_master_mode(TIM7, TIM_CR2_MMS_UPDATE);
    timer_enable_counter(TIM7);
  }
}

static void set_pwm_duty(uint32_t timer, enum tim_oc_id oc, uint32_t period, fract32_t d)
{
  uint32_t t = ((uint64_t) d * period) >> 16;
  if (t > 0xffff) while (1); // uh oh
  timer_set_oc_value(timer, oc, t);
}

/* Configure center-aligned PWM output
 *
 *           period
 *   ╭────────────────────╮
 *       t
 *   ╭────────╮
 * 1 ┌────────┐          
 * 0 ┘        └───────────┘
 * 
 * pol = 1
 */
static int configure_pwm(uint32_t timer, enum tim_oc_id oc,
                         uint32_t period, bool pol, uint16_t t)
{
  timer_reset(timer);
  timer_continuous_mode(timer);

  timer_set_oc_mode(timer, oc, pol ? TIM_OCM_PWM1 : TIM_OCM_PWM2);
  timer_set_oc_value(timer, oc, t);
  timer_enable_oc_preload(timer, oc);
  timer_enable_oc_output(timer, oc);

  timer_set_mode(timer, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_CENTER_3, TIM_CR1_DIR_UP);
  timer_enable_preload(timer);
  timer_set_period(timer, period);
  timer_generate_event(timer, TIM_EGR_UG);

  return 0;
}

/* Configure two center-aligned PWM outputs with a relative phase.
 *
 *             period
 *      ╭────────────────────╮
 * 
 *        ta
 *      ╭────────╮
 *    1 ┌────────┐          
 * A: 0 ┘        └───────────┘
 *
 *    1    ┌──┐               
 * B: 0 ───┘  └───────────────
 *
 *         ╰──╯
 *          tb
 *      ╰──╯  
 *       dt
 *
 * timer_b is configured as a gated slave to timer_a.
 */
static int configure_dual_pwm(uint32_t timer_a, enum tim_oc_id oc_a,
                              uint32_t timer_b, enum tim_oc_id oc_b,
                              uint32_t slave_trigger_src,
                              uint32_t period, uint16_t ta, uint16_t tb, uint32_t dt)
{
  // Configure PWMs independently
  configure_pwm(timer_a, oc_a, period, 1, ta);
  configure_pwm(timer_b, oc_b, period, 1, tb);

  // Setup A in master mode
  timer_set_master_mode(timer_a, TIM_CR2_MMS_ENABLE);

  // Setup B in trigger slave mode
  timer_slave_set_mode(timer_b, TIM_SMCR_SMS_GM);
  timer_slave_set_trigger(timer_b, slave_trigger_src);

  // Configure offset
  timer_set_period(timer_a, period - dt);
  timer_generate_event(timer_a, TIM_EGR_UG);
  timer_set_period(timer_a, period);
  return 0;
}

static void regulator_feedback_error(struct regulator_t *reg,
                                     struct feedback_gains *gains,
                                     int32_t error
) {
  const int32_t fudge = 2000;
  // negative error == voltage too low
  if (error < 0 && (reg->duty1 > 0xffff - fudge) && (reg->duty2 > 0xffff - fudge)) {
    // voltage collapsed: fall back
    reg->duty1 = 0xffff/2;
    reg->duty2 = 0xffff/2;
  } else if (error < 0 && reg->duty1 > 0xffff - fudge) {
    // switch 1 overflow, start increasing switch 2
    reg->duty2 -= ((int64_t) error * gains->prop_gain2) >> 16;
  } else if (error > 0 && reg->duty1 < fudge) {
    // switch 1 underflow, start decreasing switch 2
    reg->duty2 -= ((int64_t) error * gains->prop_gain2) >> 16;
  } else if (error < 0 && reg->duty2 > fudge) {
    // ???
    reg->duty2 += ((int64_t) error * gains->prop_gain2) >> 16;
  } else {
    // normal operating conditions, regulate with switch 1
    reg->duty1 -= ((int64_t) error * gains->prop_gain1) >> 16;
  }

  // Ensure switch 2 is never on when switch 1 is off
  if (reg->duty2 > reg->duty1)
    reg->duty2 = reg->duty1;
}                                     

static void regulator_feedback(struct regulator_t *reg)
{
  if (reg->mode == DISABLED) {
    return;
  } else if (reg->mode == CONST_DUTY) {
    return;
  } else if (reg->mode == VOLTAGE_FB) {
    if (reg->isense > reg->ilimit) {
      reg->duty1 /= 2;
      reg->duty2 /= 2;
    } else {
      int32_t error = reg->vsense - reg->vsetpoint;
      regulator_feedback_error(reg, &reg->v_gains, error);
    }
  } else if (reg->mode == CURRENT_FB) {
    if (reg->vsense > reg->vlimit) {
      reg->duty1 /= 2;
      reg->duty2 /= 2;
    } else {
      int32_t error = reg->isense - reg->isetpoint;
      regulator_feedback_error(reg, &reg->i_gains, error);
    }
  }

  if (reg->duty1 < 0x0000) reg->duty1 = 0;
  if (reg->duty1 > 0xffff) reg->duty1 = 0xffff;
  if (reg->duty2 < 0x0000) reg->duty2 = 0;
  if (reg->duty2 > 0xffff) reg->duty2 = 0xffff;
  reg->update_duty_func();
}


/*******************************
 * Channel 1
 *******************************/
static int configure_ch1(void)
{
  uint32_t dt = 0x10;
  uint16_t ta = ((uint64_t) chan1.period * chan1.duty1) >> 16;
  uint16_t tb = ((uint64_t) chan1.period * chan1.duty2) >> 16;
  int ret = configure_dual_pwm(TIM2, TIM_OC3,
                               TIM4, TIM_OC3,
                               TIM_SMCR_TS_ITR1,
                               chan1.period, ta, tb, dt);
  if (ret) return ret;
  timer_enable_counter(TIM4);
  timer_enable_counter(TIM2);
  return 0;
}

static void set_vsense1_en(bool enabled)
{
  if (enabled) {
    gpio_set(GPIOA, GPIO5);
  } else
    gpio_clear(GPIOA, GPIO5);
}

static void enable_ch1(void)
{
  set_vsense1_en(true);
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM2EN);
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM4EN);
  setup_common_peripherals();
}

static void disable_ch1(void)
{
  timer_disable_oc_output(TIM2, TIM_OC3);
  timer_disable_oc_output(TIM4, TIM_OC3);
  timer_disable_counter(TIM2);
  timer_disable_counter(TIM4);
  rcc_peripheral_disable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM2EN);
  rcc_peripheral_disable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM4EN);
  setup_common_peripherals();
  set_vsense1_en(false);
}

static void update_duty_ch1(void)
{
  timer_disable_counter(TIM2);
  set_pwm_duty(TIM2, TIM_OC3, chan1.period, chan1.duty1);
  set_pwm_duty(TIM4, TIM_OC3, chan1.period, chan1.duty2);
  timer_enable_counter(TIM2);
}


/*******************************
 * Channel 2
 *******************************/
int regulator_set_ch2_source(enum ch2_source_t src)
{
  if (chan2.mode != DISABLED) return -1;
  ch2_oc = (src == BATTERY) ? TIM_OC1 : TIM_OC3;
  timer_disable_oc_output(TIM3, TIM_OC1);
  timer_disable_oc_output(TIM3, TIM_OC3);
  configure_ch2();
  return 0;
}

static int configure_ch2()
{
  uint32_t t = chan2.period * chan2.duty1 / 0xffff;
  int ret = configure_pwm(TIM3, ch2_oc, chan2.period, true, t);
  if (ret) return ret;
  timer_enable_counter(TIM3);
  return 0;
}
 
static void enable_ch2(void)
{
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM3EN);
  setup_common_peripherals();
}

static void disable_ch2(void)
{
  timer_disable_oc_output(TIM3, TIM_OC1);
  timer_disable_oc_output(TIM3, TIM_OC3);
  timer_disable_counter(TIM3);
  rcc_peripheral_disable_clock(&RCC_APB1ENR, RCC_APB1ENR_TIM3EN);
  setup_common_peripherals();
}

static void update_duty_ch2(void) 
{
  set_pwm_duty(TIM3, ch2_oc, chan2.period, chan2.duty1);
}


/*******************************
 * Public interface
 *******************************/
void adc1_isr(void)
{
  ADC1_SR &= ~ADC_SR_JEOC;
  chan1.vsense = adc_read_injected(ADC1, 1);
  chan1.isense = adc_read_injected(ADC1, 2);
  chan2.vsense = adc_read_injected(ADC1, 3);
  chan2.isense = adc_read_injected(ADC1, 4);
  regulator_feedback(&chan1);
  regulator_feedback(&chan2);
}

int regulator_set_mode(struct regulator_t *reg, enum feedback_mode mode)
{
  int ret;
  enum feedback_mode old_mode = reg->mode;

  reg->mode = mode;
  if (old_mode == DISABLED && mode != DISABLED)
    reg->enable_func();
  else if (mode == DISABLED)
    reg->disable_func();

  if (mode != DISABLED) {
    ret = reg->configure_func();
    if (ret != 0) {
      reg->mode = DISABLED;
      reg->disable_func();
      return ret;
    }
  }

  return 0;
}

enum feedback_mode regulator_get_mode(struct regulator_t *reg)
{
  return reg->mode;
}

int regulator_set_duty_cycle(struct regulator_t *reg, fract32_t d1, fract32_t d2)
{
  if (reg->mode != CONST_DUTY && reg->mode != DISABLED)
    return 1;
  if (d2 > d1)
    return 2;
  reg->duty1 = d1;
  reg->duty2 = d2;
  reg->configure_func();
  return 0;
}

fract32_t regulator_get_duty_cycle_1(struct regulator_t *reg)
{
  return reg->duty1;
}

fract32_t regulator_get_duty_cycle_2(struct regulator_t *reg)
{
  return reg->duty2;
}

int regulator_set_vsetpoint(struct regulator_t *reg, fixed32_t setpoint)
{
  uint16_t v = ((uint32_t) (reg->vsense_gain * setpoint) >> 16);
  if (v > reg->vlimit) return 1;
  reg->vsetpoint = v;
  return 0;
}

fixed32_t regulator_get_vsetpoint(struct regulator_t *reg)
{
  return (reg->vsetpoint << 16) / reg->vsense_gain;
}

int regulator_set_isetpoint(struct regulator_t *reg, fixed32_t setpoint)
{
  uint16_t i = ((uint32_t) (reg->isense_gain * setpoint) >> 16);
  if (i > reg->ilimit) return 1;
  reg->isetpoint = i;
  return 0;
}

fixed32_t regulator_get_isetpoint(struct regulator_t *reg)
{
  return (reg->isetpoint << 16) / reg->isense_gain;
}

fixed32_t regulator_get_vsense(struct regulator_t *reg)
{
  return (reg->vsense << 16) / reg->vsense_gain;
}

fixed32_t regulator_get_isense(struct regulator_t *reg)
{
  return (reg->isense << 16) / reg->isense_gain;
}

void regulator_init(void)
{
  regulator_set_mode(&chan1, DISABLED);
  regulator_set_mode(&chan2, DISABLED);
}

int regulator_set_period(struct regulator_t *reg, unsigned int period)
{
  if (reg->mode != DISABLED)
    return -1;
  reg->period = period;
  return 0;
}

unsigned int regulator_get_period(struct regulator_t *reg)
{
  return reg->period;
}
