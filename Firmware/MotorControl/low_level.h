/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __LOW_LEVEL_H
#define __LOW_LEVEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <cmsis_os.h>
#include <stdbool.h>
#include <adc.h>

/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Exported variables --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported functions --------------------------------------------------------*/

//Note: to control without feed forward, set feed forward terms to 0.0f.

void pwm_trig_adc_cb(ADC_HandleTypeDef* hadc, bool injected);
void vbus_sense_adc_cb(ADC_HandleTypeDef* hadc, bool injected);

// Initalisation
void start_adc_pwm();
void start_pwm(TIM_HandleTypeDef* htim);
void sync_timers(TIM_HandleTypeDef* htim_a, TIM_HandleTypeDef* htim_b,
        uint16_t TIM_CLOCKSOURCE_ITRx, uint16_t count_offset);
void start_general_purpose_adc();

float get_adc_voltage(GPIO_TypeDef* GPIO_port, uint16_t GPIO_pin);

void update_brake_current();

#ifdef __cplusplus
}
#endif

#endif //__LOW_LEVEL_H
