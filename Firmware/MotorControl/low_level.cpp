/* Includes ------------------------------------------------------------------*/

// Because of broken cmsis_os.h, we need to include arm_math first,
// otherwise chip specific defines are ommited
#include <stm32f405xx.h>
#include <stm32f4xx_hal.h>  // Sets up the correct chip specifc defines required by arm_math
#define ARM_MATH_CM4
#include <arm_math.h>

#include <low_level.h>

#include <cmsis_os.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <adc.h>
#include <gpio.h>
#include <main.h>
#include <spi.h>
#include <tim.h>
#include <utils.h>

#include "odrive_main.hpp"

/* Private defines -----------------------------------------------------------*/

// #define DEBUG_PRINT

/* Private macros ------------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Global constant data ------------------------------------------------------*/
/* Global variables ----------------------------------------------------------*/
// This value is updated by the DC-bus reading ADC.
// Arbitrary non-zero inital value to avoid division by zero if ADC reading is late
float vbus_voltage = 12.0f;

/* Private constant data -----------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Function implementations --------------------------------------------------*/

void start_adc_pwm() {
    // Enable ADC and interrupts
    __HAL_ADC_ENABLE(&hadc1);
    __HAL_ADC_ENABLE(&hadc2);
    __HAL_ADC_ENABLE(&hadc3);
    // Warp field stabilize.
    osDelay(2);
    __HAL_ADC_ENABLE_IT(&hadc1, ADC_IT_JEOC);
    __HAL_ADC_ENABLE_IT(&hadc2, ADC_IT_JEOC);
    __HAL_ADC_ENABLE_IT(&hadc3, ADC_IT_JEOC);
    __HAL_ADC_ENABLE_IT(&hadc2, ADC_IT_EOC);
    __HAL_ADC_ENABLE_IT(&hadc3, ADC_IT_EOC);

    // Ensure that debug halting of the core doesn't leave the motor PWM running
    __HAL_DBGMCU_FREEZE_TIM1();
    __HAL_DBGMCU_FREEZE_TIM8();

    start_pwm(&htim1);
    start_pwm(&htim8);
    // TODO: explain why this offset
    sync_timers(&htim1, &htim8, TIM_CLOCKSOURCE_ITR0, TIM_1_8_PERIOD_CLOCKS / 2 - 1 * 128);

    // Motor output starts in the disabled state
    __HAL_TIM_MOE_DISABLE_UNCONDITIONALLY(&htim1);
    __HAL_TIM_MOE_DISABLE_UNCONDITIONALLY(&htim8);

    // Start brake resistor PWM in floating output configuration
    htim2.Instance->CCR3 = 0;
    htim2.Instance->CCR4 = TIM_APB1_PERIOD_CLOCKS + 1;
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);
}

void start_pwm(TIM_HandleTypeDef* htim) {
    // Init PWM
    int half_load = TIM_1_8_PERIOD_CLOCKS / 2;
    htim->Instance->CCR1 = half_load;
    htim->Instance->CCR2 = half_load;
    htim->Instance->CCR3 = half_load;

    // This hardware obfustication layer really is getting on my nerves
    HAL_TIM_PWM_Start(htim, TIM_CHANNEL_1);
    HAL_TIMEx_PWMN_Start(htim, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(htim, TIM_CHANNEL_2);
    HAL_TIMEx_PWMN_Start(htim, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(htim, TIM_CHANNEL_3);
    HAL_TIMEx_PWMN_Start(htim, TIM_CHANNEL_3);

    htim->Instance->CCR4 = 1;
    HAL_TIM_PWM_Start_IT(htim, TIM_CHANNEL_4);
}

void sync_timers(TIM_HandleTypeDef* htim_a, TIM_HandleTypeDef* htim_b,
                 uint16_t TIM_CLOCKSOURCE_ITRx, uint16_t count_offset) {
    // Store intial timer configs
    uint16_t MOE_store_a = htim_a->Instance->BDTR & (TIM_BDTR_MOE);
    uint16_t MOE_store_b = htim_b->Instance->BDTR & (TIM_BDTR_MOE);
    uint16_t CR2_store = htim_a->Instance->CR2;
    uint16_t SMCR_store = htim_b->Instance->SMCR;
    // Turn off output
    htim_a->Instance->BDTR &= ~(TIM_BDTR_MOE);
    htim_b->Instance->BDTR &= ~(TIM_BDTR_MOE);
    // Disable both timer counters
    htim_a->Instance->CR1 &= ~TIM_CR1_CEN;
    htim_b->Instance->CR1 &= ~TIM_CR1_CEN;
    // Set first timer to send TRGO on counter enable
    htim_a->Instance->CR2 &= ~TIM_CR2_MMS;
    htim_a->Instance->CR2 |= TIM_TRGO_ENABLE;
    // Set Trigger Source of second timer to the TRGO of the first timer
    htim_b->Instance->SMCR &= ~TIM_SMCR_TS;
    htim_b->Instance->SMCR |= TIM_CLOCKSOURCE_ITRx;
    // Set 2nd timer to start on trigger
    htim_b->Instance->SMCR &= ~TIM_SMCR_SMS;
    htim_b->Instance->SMCR |= TIM_SLAVEMODE_TRIGGER;
    // Dir bit is read only in center aligned mode, so we clear the mode for now
    uint16_t CMS_store_a = htim_a->Instance->CR1 & TIM_CR1_CMS;
    uint16_t CMS_store_b = htim_b->Instance->CR1 & TIM_CR1_CMS;
    htim_a->Instance->CR1 &= ~TIM_CR1_CMS;
    htim_b->Instance->CR1 &= ~TIM_CR1_CMS;
    // Set both timers to up-counting state
    htim_a->Instance->CR1 &= ~TIM_CR1_DIR;
    htim_b->Instance->CR1 &= ~TIM_CR1_DIR;
    // Restore center aligned mode
    htim_a->Instance->CR1 |= CMS_store_a;
    htim_b->Instance->CR1 |= CMS_store_b;
    // set counter offset
    htim_a->Instance->CNT = count_offset;
    htim_b->Instance->CNT = 0;
    // Start Timer a
    htim_a->Instance->CR1 |= (TIM_CR1_CEN);
    // Restore timer configs
    htim_a->Instance->CR2 = CR2_store;
    htim_b->Instance->SMCR = SMCR_store;
    // restore output
    htim_a->Instance->BDTR |= MOE_store_a;
    htim_b->Instance->BDTR |= MOE_store_b;
}

// @brief Floats ALL phases immediately and sets the brake current to 0.
void disable_all_pwms(Motor::Error_t error) {
    // Disable all motors NOW!
    for (size_t i = 0; i < AXIS_COUNT; ++i) {
        axes[i]->motor_.disarm();
        axes[i]->motor_.error_ = error;
    }
}

// @brief ADC1 measurements are written to this buffer by DMA
uint16_t adc_measurements_[ADC_CHANNEL_COUNT] = { 0 };

// @brief Starts the general purpose ADC on the ADC1 peripheral.
// The measured ADC voltages can be read with get_adc_voltage().
//
// ADC1 is set up to continuously sample all channels 0 to 15 in a
// round-robin fashion.
// DMA is used to copy the measured 12-bit values to adc_measurements_.
//
// The injected (high priority) channel of ADC1 is used to sample vbus_voltage.
// This conversion is triggered by TIM1 at the frequency of the motor control loop.
void start_general_purpose_adc() {
    ADC_ChannelConfTypeDef sConfig;

    // Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode = ENABLE;
    hadc1.Init.ContinuousConvMode = ENABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = ADC_CHANNEL_COUNT;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        _Error_Handler((char*)__FILE__, __LINE__);
    }

    // Set up sampling sequence (channel 0 ... channel 15)
    sConfig.SamplingTime = ADC_SAMPLETIME_15CYCLES;
    for (uint32_t channel = 0; channel < ADC_CHANNEL_COUNT; ++channel) {
        sConfig.Channel = channel << ADC_CR1_AWDCH_Pos;
        sConfig.Rank = channel + 1; // rank numbering starts at 1
        if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
            _Error_Handler((char*)__FILE__, __LINE__);
    }

    HAL_ADC_Start_DMA(&hadc1, reinterpret_cast<uint32_t*>(adc_measurements_), ADC_CHANNEL_COUNT);
}

// @brief Returns the ADC voltage associated with the specified pin.
// GPIO_set_to_analog() must be called first to put the Pin into
// analog mode.
// Returns NaN if the pin has no associated ADC1 channel.
//
// On ODrive 3.3 and 3.4 the following pins can be used with this function:
//  GPIO_1, GPIO_2, GPIO_3, GPIO_4 and some pins that are connected to
//  on-board sensors (M0_TEMP, M1_TEMP, AUX_TEMP)
//
// The ADC values are sampled in background at ~30kHz without
// any CPU involvement.
//
// Details: each of the 16 conversion takes (15+26) ADC clock
// cycles and the ADC, so the update rate of the entire sequence is:
//  21000kHz / (15+26) / 16 = 32kHz
// The true frequency is slightly lower because of the injected vbus
// measurements
float get_adc_voltage(GPIO_TypeDef* GPIO_port, uint16_t GPIO_pin) {
    uint32_t channel = UINT32_MAX;
    if (GPIO_port == GPIOA) {
        if (GPIO_pin == GPIO_PIN_0)
            channel = 0;
        else if (GPIO_pin == GPIO_PIN_1)
            channel = 1;
        else if (GPIO_pin == GPIO_PIN_2)
            channel = 2;
        else if (GPIO_pin == GPIO_PIN_3)
            channel = 3;
        else if (GPIO_pin == GPIO_PIN_4)
            channel = 4;
        else if (GPIO_pin == GPIO_PIN_5)
            channel = 5;
        else if (GPIO_pin == GPIO_PIN_6)
            channel = 6;
        else if (GPIO_pin == GPIO_PIN_7)
            channel = 7;
    } else if (GPIO_port == GPIOB) {
        if (GPIO_pin == GPIO_PIN_0)
            channel = 8;
        else if (GPIO_pin == GPIO_PIN_1)
            channel = 9;
    } else if (GPIO_port == GPIOC) {
        if (GPIO_pin == GPIO_PIN_0)
            channel = 10;
        else if (GPIO_pin == GPIO_PIN_1)
            channel = 11;
        else if (GPIO_pin == GPIO_PIN_2)
            channel = 12;
        else if (GPIO_pin == GPIO_PIN_3)
            channel = 13;
        else if (GPIO_pin == GPIO_PIN_4)
            channel = 14;
        else if (GPIO_pin == GPIO_PIN_5)
            channel = 15;
    }
    if (channel < ADC_CHANNEL_COUNT)
        return ((float)adc_measurements_[channel]) * (3.3f / (float)(1 << 12));
    else
        return 0.0f / 0.0f; // NaN
}

//--------------------------------
// IRQ Callbacks
//--------------------------------


void vbus_sense_adc_cb(ADC_HandleTypeDef* hadc, bool injected) {
    static const float voltage_scale = 3.3f * VBUS_S_DIVIDER_RATIO / (float)(1 << 12);
    // Only one conversion in sequence, so only rank1
    uint32_t ADCValue = HAL_ADCEx_InjectedGetValue(hadc, ADC_INJECTED_RANK_1);
    vbus_voltage = ADCValue * voltage_scale;
}

// This is the callback from the ADC that we expect after the PWM has triggered an ADC conversion.
// TODO: Document how the phasing is done, link to timing diagram
void pwm_trig_adc_cb(ADC_HandleTypeDef* hadc, bool injected) {
#define calib_tau 0.2f  //@TOTO make more easily configurable
    static const float calib_filter_k = CURRENT_MEAS_PERIOD / calib_tau;

    // Ensure ADCs are expected ones to simplify the logic below
    if (!(hadc == &hadc2 || hadc == &hadc3)) {
        disable_all_pwms(Motor::ERROR_ADC_FAILED);
        return;
    };

    // Motor 0 is on Timer 1, which triggers ADC 2 and 3 on an injected conversion
    // Motor 1 is on Timer 8, which triggers ADC 2 and 3 on a regular conversion
    // If the corresponding timer is counting up, we just sampled in SVM vector 0, i.e. real current
    // If we are counting down, we just sampled in SVM vector 7, with zero current
    Axis& axis = injected ? *axes[0] : *axes[1];
    Axis& other_axis = injected ? *axes[1] : *axes[0];
    bool counting_down = axis.motor_.hw_config_.timer->Instance->CR1 & TIM_CR1_DIR;

    bool current_meas_not_DC_CAL = !counting_down;
    bool update_timings = false;
    if (hadc == &hadc2) {
        if (&axis == axes[1] && counting_down)
            update_timings = true; // update timings of M0
        else if (&axis == axes[0] && !counting_down)
            update_timings = true; // update timings of M1
    }

    // Load next timings for the motor that we're not currently sampling
    if (update_timings) {
        if (other_axis.motor_.next_timings_valid_ && !other_axis.missed_control_deadline_) {
            other_axis.motor_.next_timings_valid_ = false;
            other_axis.motor_.hw_config_.timer->Instance->CCR1 = other_axis.motor_.next_timings_[0];
            other_axis.motor_.hw_config_.timer->Instance->CCR2 = other_axis.motor_.next_timings_[1];
            other_axis.motor_.hw_config_.timer->Instance->CCR3 = other_axis.motor_.next_timings_[2];
            __HAL_TIM_MOE_ENABLE(other_axis.motor_.hw_config_.timer);  // enable pwm outputs
            update_brake_current();
        } else {
            // the motor control loop failed to update the timings in time
            // we must assume that it died and therefore float all phases
            other_axis.motor_.disarm();
        }
    }

    // Check the timing of the sequencing
    if (current_meas_not_DC_CAL)
        axis.motor_.log_timing(Motor::TIMING_LOG_ADC_CB_I);
    else
        axis.motor_.log_timing(Motor::TIMING_LOG_ADC_CB_DC);

    uint32_t ADCValue;
    if (injected) {
        ADCValue = HAL_ADCEx_InjectedGetValue(hadc, ADC_INJECTED_RANK_1);
    } else {
        ADCValue = HAL_ADC_GetValue(hadc);
    }
    float current = axis.motor_.phase_current_from_adcval(ADCValue);

    if (current_meas_not_DC_CAL) {
        // ADC2 and ADC3 record the phB and phC currents concurrently,
        // and their interrupts should arrive on the same clock cycle.
        // We dispatch the callbacks in order, so ADC2 will always be processed before ADC3.
        // Therefore we store the value from ADC2 and signal the thread that the
        // measurement is ready when we receive the ADC3 measurement

        // return or continue
        if (hadc == &hadc2) {
            axis.motor_.current_meas_.phB = current - axis.motor_.DC_calib_.phB;
            return;
        } else {
            axis.motor_.current_meas_.phC = current - axis.motor_.DC_calib_.phC;
        }
        // Trigger axis thread
        axis.signal_current_meas();
    } else {
        // DC_CAL measurement
        if (hadc == &hadc2) {
            axis.motor_.DC_calib_.phB += (current - axis.motor_.DC_calib_.phB) * calib_filter_k;
        } else {
            axis.motor_.DC_calib_.phC += (current - axis.motor_.DC_calib_.phC) * calib_filter_k;
        }
    }
}

// @brief Sums up the Ibus contribution of each motor and updates the
// brake resistor PWM accordingly.
void update_brake_current() {
    float Ibus_sum = 0.0f;
    for (size_t i = 0; i < AXIS_COUNT; ++i) {
        Ibus_sum += axes[i]->motor_.current_control_.Ibus;
    }
    float brake_current = -Ibus_sum;
    // Clip negative values to 0.0f
    if (brake_current < 0.0f) brake_current = 0.0f;
    float brake_duty = brake_current * board_config.brake_resistance / vbus_voltage;

    // Duty limit at 90% to allow bootstrap caps to charge
    if (brake_duty > 0.9f) brake_duty = 0.9f;
    int high_on = TIM_APB1_PERIOD_CLOCKS * (1.0f - brake_duty);
    int low_off = high_on - TIM_APB1_DEADTIME_CLOCKS;
    if (low_off < 0) low_off = 0;

    // Safe update of low and high side timings
    // To avoid race condition, first reset timings to safe state
    // ch3 is low side, ch4 is high side
    htim2.Instance->CCR3 = 0;
    htim2.Instance->CCR4 = TIM_APB1_PERIOD_CLOCKS + 1;
    htim2.Instance->CCR3 = low_off;
    htim2.Instance->CCR4 = high_on;
}
