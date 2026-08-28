/*
 * VelaWear - finite SF32LB52 audio hardware test.
 *
 * This deliberately uses the native AUDCODEC HAL.  The current board port
 * does not register a NuttX /dev/audio device yet, so this test isolates the
 * silicon, codec, MEMS microphone and PA wiring before adding that adapter.
 */

#include <nuttx/config.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>

#ifndef SOC_BF0_HCPU
#  define SOC_BF0_HCPU
#endif
#ifndef SF32LB52X
#  define SF32LB52X
#endif

#include "bf0_hal.h"
#include "audio_hw_test.h"

#define AUDIO_RATE       16000
#define AUDIO_TONE_SEC   2
#define AUDIO_MIC_SEC    1
#define AUDIO_PA_PIN     42

static const int16_t g_tone[32] =
{
    0, 6383, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6383,
    0, -6383, -12539, -18204, -23170, -27245, -30273, -32137,
    -32768, -32137, -30273, -27245, -23170, -18204, -12539, -6383
};

static AUDCODE_DAC_CLK_CONFIG_TYPE g_dac_clock =
    {16000, 0, 1, 4, 0x14d, 0, 5, 4, 2, 20, 20, 0};
static AUDCODE_ADC_CLK_CONFIG_TYPE g_adc_clock =
    {16000, 0, 10, 1, 0, 0, 5, 2};

static void audio_pa_set(int on)
{
    GPIO_InitTypeDef init;

    init.Pin = AUDIO_PA_PIN;
    init.Mode = GPIO_MODE_OUTPUT;
    init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(hwp_gpio1, &init);
    HAL_GPIO_WritePin(hwp_gpio1, AUDIO_PA_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int audio_codec_open(AUDCODEC_HandleTypeDef *codec)
{
    int ret;

    *codec = (AUDCODEC_HandleTypeDef){0};
    codec->Instance = hwp_audcodec;
    codec->Init.samplerate_index = 3;
    codec->Init.dac_cfg.opmode = 1;
    codec->Init.dac_cfg.dac_clk = &g_dac_clock;
    codec->Init.adc_cfg.opmode = 1;
    codec->Init.adc_cfg.adc_clk = &g_adc_clock;

    HAL_PMU_EnableAudio(1);
    HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);

    ret = HAL_AUDCODEC_Init(codec);
    if (ret != HAL_OK)
        return ret;

    HAL_TURN_ON_PLL();
    return HAL_OK;
}

static void audio_codec_close(AUDCODEC_HandleTypeDef *codec)
{
    audio_pa_set(0);
    __HAL_AUDCODEC_DAC_DISABLE(codec);
    __HAL_AUDCODEC_ADC_DISABLE(codec);
    HAL_AUDCODEC_Close_Analog_DACPath();
    HAL_AUDCODEC_Close_Analog_ADCPath();
    HAL_AUDCODEC_DeInit(codec);
    HAL_TURN_OFF_PLL();
    HAL_RCC_DisableModule(RCC_MOD_AUDCODEC);
    HAL_PMU_EnableAudio(0);
}

static int audio_play_tone(AUDCODEC_HandleTypeDef *codec)
{
    AUDCODEC_DACCfgTypeDef cfg = codec->Init.dac_cfg;
    uint32_t total = AUDIO_RATE * AUDIO_TONE_SEC;
    uint32_t sent = 0;
    uint32_t spins = 0;
    int ret;

    ret = HAL_AUDCODEC_Config_TChanel(codec, 0, &cfg);
    if (ret != HAL_OK)
        return ret;
    ret = HAL_AUDCODEC_Config_DACPath_Volume(codec, 0, 0);
    if (ret != HAL_OK)
        return ret;
    ret = HAL_AUDCODEC_Config_DACPath(codec, 1);
    if (ret != HAL_OK)
        return ret;
    ret = HAL_AUDCODEC_Config_Analog_DACPath(&g_dac_clock);
    if (ret != HAL_OK)
        return ret;

    __HAL_AUDCODEC_DAC_ENABLE(codec);
    audio_pa_set(1);
    syslog(LOG_INFO, "[AudioTest] SPEAKER: 1 kHz tone for %d s; listen now\n",
           AUDIO_TONE_SEC);

    while (sent < total && spins < 500000)
    {
        uint32_t stat = codec->Instance->APB_STAT;
        uint32_t count = (stat & AUDCODEC_APB_STAT_DAC_CH0_FIFO_CNT_Msk) >>
                         AUDCODEC_APB_STAT_DAC_CH0_FIFO_CNT_Pos;

        if (count < 8)
        {
            codec->Instance->DAC_CH0_ENTRY =
                (uint32_t)(uint16_t)g_tone[sent % 32];
            sent++;
        }
        else
        {
            usleep(10);
        }
        spins++;
    }

    usleep(30000);
    audio_pa_set(0);
    __HAL_AUDCODEC_DAC_DISABLE(codec);

    syslog(LOG_INFO, "[AudioTest] SPEAKER: queued=%lu/%lu\n",
           (unsigned long)sent, (unsigned long)total);
    return sent == total ? HAL_OK : HAL_TIMEOUT;
}

static int audio_capture_mic(AUDCODEC_HandleTypeDef *codec)
{
    AUDCODEC_ADCCfgTypeDef cfg = codec->Init.adc_cfg;
    uint32_t target = AUDIO_RATE * AUDIO_MIC_SEC;
    uint32_t got = 0;
    uint32_t spins = 0;
    int32_t min = INT32_MAX;
    int32_t max = INT32_MIN;
    uint64_t abs_sum = 0;
    int ret;

    ret = HAL_AUDCODEC_Config_RChanel(codec, 0, &cfg);
    if (ret != HAL_OK)
        return ret;
    ret = HAL_AUDCODEC_Config_ADCPath_Volume(codec, 0, 0);
    if (ret != HAL_OK)
        return ret;
    HAL_AUDCODEC_Config_Analog_ADCPath(&g_adc_clock);
    __HAL_AUDCODEC_ADC_ENABLE(codec);

    syslog(LOG_INFO, "[AudioTest] MICROPHONE: speak/tap near the MEMS mic for %d s\n",
           AUDIO_MIC_SEC);

    while (got < target && spins < 350000)
    {
        uint32_t stat = codec->Instance->APB_STAT;
        uint32_t count = (stat & AUDCODEC_APB_STAT_ADC_CH0_FIFO_CNT_Msk) >>
                         AUDCODEC_APB_STAT_ADC_CH0_FIFO_CNT_Pos;

        if (count != 0)
        {
            int32_t sample = (int16_t)(codec->Instance->ADC_CH0_ENTRY & 0xffff);
            int32_t a = sample < 0 ? -sample : sample;

            if (sample < min) min = sample;
            if (sample > max) max = sample;
            abs_sum += (uint32_t)a;
            got++;
        }
        else
        {
            usleep(10);
        }
        spins++;
    }

    __HAL_AUDCODEC_ADC_DISABLE(codec);
    if (got == 0)
    {
        syslog(LOG_ERR, "[AudioTest] MICROPHONE: no ADC samples\n");
        return HAL_TIMEOUT;
    }

    syslog(LOG_INFO,
           "[AudioTest] MICROPHONE: samples=%lu min=%ld max=%ld avg_abs=%lu\n",
           (unsigned long)got, (long)min, (long)max,
           (unsigned long)(abs_sum / got));
    return got == target ? HAL_OK : HAL_TIMEOUT;
}

int velawear_audio_hw_test(void)
{
    AUDCODEC_HandleTypeDef codec;
    int speaker_ret;
    int mic_ret;

    syslog(LOG_INFO, "[AudioTest] opening SF32LB52 AUDCODEC at %d Hz\n",
           AUDIO_RATE);
    if (audio_codec_open(&codec) != HAL_OK)
    {
        syslog(LOG_ERR, "[AudioTest] codec init failed\n");
        return -1;
    }

    speaker_ret = audio_play_tone(&codec);
    mic_ret = audio_capture_mic(&codec);
    audio_codec_close(&codec);

    syslog(LOG_INFO, "[AudioTest] result speaker=%d microphone=%d\n",
           speaker_ret, mic_ret);
    return (speaker_ret == HAL_OK && mic_ret == HAL_OK) ? 0 : -1;
}
