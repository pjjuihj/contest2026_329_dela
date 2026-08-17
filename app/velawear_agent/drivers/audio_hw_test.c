/*
 * VelaWear - SF32LB52 on-chip audio hardware test.
 *
 * This finite diagnostic exercises the board's internal AUDCODEC directly.
 * The current NuttX port has no /dev/audio registration yet.
 */

#include <nuttx/config.h>

#ifndef SOC_BF0_HCPU
#  define SOC_BF0_HCPU 1
#endif
#ifndef SF32LB52X
#  define SF32LB52X 1
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "bf0_hal.h"
#include "audio_hw_test.h"

#define AUDIO_TEST_RATE       16000
#define AUDIO_TEST_SECONDS    2
#define AUDIO_TEST_SAMPLES    (AUDIO_TEST_RATE * AUDIO_TEST_SECONDS)
#define AUDIO_FIFO_WATERMARK  8
#define AUDIO_PA_PIN          42

static const int16_t g_tone[32] =
{
   0,  6393, 12539, 18204, 23170, 27245, 30273, 32137,
   32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
   0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
  -32768,-32137,-30273,-27245,-23170,-18204,-12539,-6393
};

static const AUDCODE_DAC_CLK_CONFIG_TYPE g_dac_clock =
{
  AUDIO_TEST_RATE, 0, 1, 4, 0x14d, 0, 5, 4, 2, 20, 20, 0
};

static const AUDCODE_ADC_CLK_CONFIG_TYPE g_adc_clock =
{
  AUDIO_TEST_RATE, 0, 10, 1, 0, 0, 5, 2
};

static void audio_pa_set(bool enabled)
{
  GPIO_InitTypeDef init;

  memset(&init, 0, sizeof(init));
  init.Pin = AUDIO_PA_PIN;
  init.Mode = GPIO_MODE_OUTPUT;
  init.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init((GPIO_TypeDef *)hwp_gpio1, &init);
  HAL_GPIO_WritePin((GPIO_TypeDef *)hwp_gpio1, AUDIO_PA_PIN,
                    enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint32_t audio_dac_fifo_count(void)
{
  return (hwp_audcodec->APB_STAT &
          AUDCODEC_APB_STAT_DAC_CH0_FIFO_CNT_Msk) >>
         AUDCODEC_APB_STAT_DAC_CH0_FIFO_CNT_Pos;
}

static uint32_t audio_adc_fifo_count(void)
{
  return (hwp_audcodec->APB_STAT &
          AUDCODEC_APB_STAT_ADC_CH0_FIFO_CNT_Msk) >>
         AUDCODEC_APB_STAT_ADC_CH0_FIFO_CNT_Pos;
}

static void audio_dac_stop(AUDCODEC_HandleTypeDef *codec)
{
  HAL_AUDCODEC_Config_DACPath(codec, 1);
  __HAL_AUDCODEC_DAC_DISABLE(codec);
  HAL_AUDCODEC_Close_Analog_DACPath();
}

static void audio_adc_stop(AUDCODEC_HandleTypeDef *codec)
{
  __HAL_AUDCODEC_ADC_DISABLE(codec);
  HAL_AUDCODEC_Close_Analog_ADCPath();
}

int velawear_audio_hw_test(void)
{
  AUDCODEC_HandleTypeDef codec;
  AUDCODEC_DACCfgTypeDef dac_cfg;
  AUDCODEC_ADCCfgTypeDef adc_cfg;
  int ret = HAL_OK;
  int i;

  memset(&codec, 0, sizeof(codec));
  codec.Instance = hwp_audcodec;
  codec.Init.dac_cfg.opmode = 1;
  codec.Init.adc_cfg.opmode = 1;

  syslog(LOG_INFO, "[AudioTest] SF32LB52 AUDCODEC test begin\n");

  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  ret = HAL_AUDCODEC_Init(&codec);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] HAL_AUDCODEC_Init failed: %d\n", ret);
      HAL_PMU_EnableAudio(0);
      return -1;
    }

  dac_cfg.opmode = 1;
  dac_cfg.dac_clk = (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clock;
  ret = HAL_AUDCODEC_Config_TChanel(&codec, 0, &dac_cfg);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] DAC channel config failed: %d\n", ret);
      goto cleanup;
    }

  __HAL_AUDCODEC_DAC_ENABLE(&codec);
  HAL_AUDCODEC_Config_DACPath(&codec, 0);
  HAL_TURN_ON_PLL();
  ret = HAL_AUDCODEC_Config_Analog_DACPath(
          (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clock);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] analog DAC path failed: %d\n", ret);
      goto cleanup;
    }

  audio_pa_set(true);
  syslog(LOG_INFO, "[AudioTest] SPEAKER: 1 kHz tone for %d seconds\n",
         AUDIO_TEST_SECONDS);

  for (i = 0; i < AUDIO_TEST_SAMPLES; i++)
    {
      uint32_t guard = 0;
      while (audio_dac_fifo_count() >= AUDIO_FIFO_WATERMARK &&
             guard++ < 100)
        {
          usleep(20);
        }
      hwp_audcodec->DAC_CH0_ENTRY = (uint16_t)g_tone[i & 31];
    }

  audio_pa_set(false);
  audio_dac_stop(&codec);
  syslog(LOG_INFO, "[AudioTest] SPEAKER: tone feed complete\n");

  adc_cfg.opmode = 1;
  adc_cfg.adc_clk = (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clock;
  ret = HAL_AUDCODEC_Config_RChanel(&codec, 0, &adc_cfg);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] ADC channel config failed: %d\n", ret);
      goto cleanup;
    }

  HAL_AUDCODEC_Config_ADCPath_Volume(&codec, 0, 0);
  ret = HAL_AUDCODEC_Config_Analog_ADCPath(
          (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clock);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] analog ADC path failed: %d\n", ret);
      goto cleanup;
    }

  __HAL_AUDCODEC_ADC_ENABLE(&codec);
  {
    int32_t min_sample = INT32_MAX;
    int32_t max_sample = INT32_MIN;
    uint64_t sum_abs = 0;
    uint32_t samples = 0;
    uint32_t idle_loops = 0;

    syslog(LOG_INFO, "[AudioTest] MICROPHONE: sampling for %d seconds\n",
           AUDIO_TEST_SECONDS);

    while (samples < AUDIO_TEST_SAMPLES && idle_loops < 20000)
      {
        if (audio_adc_fifo_count() == 0)
          {
            usleep(100);
            idle_loops++;
            continue;
          }

        {
          int32_t sample = (int16_t)(hwp_audcodec->ADC_CH0_ENTRY & 0xffff);
          if (sample < min_sample)
            {
              min_sample = sample;
            }
          if (sample > max_sample)
            {
              max_sample = sample;
            }
          sum_abs += (uint32_t)(sample < 0 ? -sample : sample);
          samples++;
        }
      }

    if (samples == 0)
      {
        syslog(LOG_ERR, "[AudioTest] MICROPHONE: no ADC samples\n");
      }
    else
      {
        syslog(LOG_INFO,
               "[AudioTest] MICROPHONE: samples=%lu min=%ld max=%ld avg_abs=%lu\n",
               (unsigned long)samples, (long)min_sample, (long)max_sample,
               (unsigned long)(sum_abs / samples));
      }
  }

  audio_adc_stop(&codec);

cleanup:
  audio_pa_set(false);
  HAL_AUDCODEC_DeInit(&codec);
  HAL_TURN_OFF_PLL();
  HAL_PMU_EnableAudio(0);
  syslog(LOG_INFO, "[AudioTest] complete\n");
  return ret == HAL_OK ? 0 : -1;
}
