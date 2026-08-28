/*
 * VelaWear - SF32LB52 on-chip audio hardware test.
 *
 * This finite diagnostic exercises the board's internal AUDCODEC directly.
 * The current NuttX port has no /dev/audio registration yet.
 */

#include <nuttx/config.h>
#include <nuttx/irq.h>

#ifndef SOC_BF0_HCPU
#  define SOC_BF0_HCPU 1
#endif
#ifndef SF32LB52X
#  define SF32LB52X 1
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <pthread.h>

#include "bf0_hal.h"
#include "audio_hw_test.h"
#include "velawear_agent_protocol.h"


/* Present in the HAL implementation; absent from this vendored HAL header. */
extern HAL_StatusTypeDef HAL_AUDCODEC_Config_ADCPath_Volume(
  AUDCODEC_HandleTypeDef *hacodec, int channel, int volume);
#define AUDIO_TEST_RATE       16000
#define AUDIO_TEST_SECONDS    2
#define AUDIO_MIC_ADC_GAIN_DB  24
#define AUDIO_TEST_SAMPLES    (AUDIO_TEST_RATE * AUDIO_TEST_SECONDS)
#define AUDIO_PA_PIN          42
#define AUDIO_ALERT_DAC_VOLUME -24
/* PA42 is Audio_PA_EN; keep it low during boot and enable it after DMA starts. */
#define VELAWEAR_ENABLE_EXTERNAL_PA 1
#define AUDIO_MUSIC_RATE      8000
#define AUDIO_MUSIC_MAX_MS    4000
#define AUDIO_DMA_BUFFER_WORDS \
  ((AUDIO_MUSIC_RATE * AUDIO_MUSIC_MAX_MS) / 1000)

/* The SDK AUDCODEC DMA engine transfers one 32-bit word per request.  Keep
 * the buffer in HCPU SRAM (not PSRAM) so the HAL cache maintenance and DMA
 * address mapping remain valid on SF32LB52. */
static uint32_t g_audio_dma_buffer[AUDIO_DMA_BUFFER_WORDS];
static DMA_HandleTypeDef g_audio_dma;
static volatile AUDCODEC_HandleTypeDef *g_audio_dma_codec;
static volatile bool g_audio_dma_complete;
static volatile bool g_audio_dma_rx_complete;
static volatile bool g_audio_dma_error;

/* Persistent microphone stream: two 20 ms halves keep the DMA footprint
 * small while allowing the agent thread to process completed audio without
 * touching the half currently written by the codec. */
#define AUDIO_MIC_STREAM_HALF_WORDS   320
#define AUDIO_MIC_STREAM_BUFFER_WORDS (AUDIO_MIC_STREAM_HALF_WORDS * 2)
static uint32_t g_mic_stream_buffer[AUDIO_MIC_STREAM_BUFFER_WORDS];
static DMA_HandleTypeDef g_mic_stream_dma;
static AUDCODEC_HandleTypeDef g_mic_stream_codec;
static volatile AUDCODEC_HandleTypeDef *g_mic_stream_codec_ref;
static volatile uint8_t g_mic_stream_ready;
static volatile bool g_mic_stream_error;
static volatile bool g_mic_stream_active;

/* The NuttX port uses its generic vector dispatcher when RAM vectors are
 * disabled.  Register adapters for every dynamic DMAC1 channel so the HAL
 * handlers are reached through irq_dispatch(). */
void DMAC1_CH1_IRQHandler(void) { HAL_DMAC1_CH1_IRQHandler(); }
void DMAC1_CH2_IRQHandler(void) { HAL_DMAC1_CH2_IRQHandler(); }
void DMAC1_CH3_IRQHandler(void) { HAL_DMAC1_CH3_IRQHandler(); }
void DMAC1_CH4_IRQHandler(void) { HAL_DMAC1_CH4_IRQHandler(); }
void DMAC1_CH5_IRQHandler(void) { HAL_DMAC1_CH5_IRQHandler(); }
void DMAC1_CH6_IRQHandler(void) { HAL_DMAC1_CH6_IRQHandler(); }
void DMAC1_CH7_IRQHandler(void) { HAL_DMAC1_CH7_IRQHandler(); }
void DMAC1_CH8_IRQHandler(void) { HAL_DMAC1_CH8_IRQHandler(); }

static int audio_dmac1_ch1_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH1_IRQHandler();
  return 0;
}

static int audio_dmac1_ch2_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH2_IRQHandler();
  return 0;
}

static int audio_dmac1_ch3_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH3_IRQHandler();
  return 0;
}

static int audio_dmac1_ch4_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH4_IRQHandler();
  return 0;
}

static int audio_dmac1_ch5_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH5_IRQHandler();
  return 0;
}

static int audio_dmac1_ch6_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH6_IRQHandler();
  return 0;
}

static int audio_dmac1_ch7_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH7_IRQHandler();
  return 0;
}

static int audio_dmac1_ch8_irq(int irq, void *context, void *arg)
{
  (void)irq; (void)context; (void)arg;
  HAL_DMAC1_CH8_IRQHandler();
  return 0;
}

static int audio_dma_attach_irqs(void)
{
  int ret = 0;
  int current;

#define AUDIO_ATTACH_DMA_IRQ(channel, handler) \
  do \
    { \
      current = irq_attach(DMAC1_CH##channel##_IRQn + NVIC_IRQ_FIRST, \
                           handler, NULL); \
      if (ret == 0 && current < 0) \
        { \
          ret = current; \
        } \
    } while (0)

  AUDIO_ATTACH_DMA_IRQ(1, audio_dmac1_ch1_irq);
  AUDIO_ATTACH_DMA_IRQ(2, audio_dmac1_ch2_irq);
  AUDIO_ATTACH_DMA_IRQ(3, audio_dmac1_ch3_irq);
  AUDIO_ATTACH_DMA_IRQ(4, audio_dmac1_ch4_irq);
  AUDIO_ATTACH_DMA_IRQ(5, audio_dmac1_ch5_irq);
  AUDIO_ATTACH_DMA_IRQ(6, audio_dmac1_ch6_irq);
  AUDIO_ATTACH_DMA_IRQ(7, audio_dmac1_ch7_irq);
  AUDIO_ATTACH_DMA_IRQ(8, audio_dmac1_ch8_irq);

#undef AUDIO_ATTACH_DMA_IRQ
  return ret;
}

void HAL_AUDCODEC_TxCpltCallback(AUDCODEC_HandleTypeDef *codec, int cid)
{
  if (codec == g_audio_dma_codec && cid == HAL_AUDCODEC_DAC_CH0)
    {
      g_audio_dma_complete = true;
    }
}

void HAL_AUDCODEC_RxHalfCpltCallback(AUDCODEC_HandleTypeDef *codec,
                                      int cid)
{
  if (codec == g_mic_stream_codec_ref && cid == HAL_AUDCODEC_ADC_CH0)
    {
      /* ISR path: only publish a bit; logging and DSP stay in the worker. */
      g_mic_stream_ready |= 1U;
    }
}

void HAL_AUDCODEC_RxCpltCallback(AUDCODEC_HandleTypeDef *codec, int cid)
{
  if (codec == g_audio_dma_codec && cid == HAL_AUDCODEC_ADC_CH0)
    {
      g_audio_dma_rx_complete = true;
    }

  if (codec == g_mic_stream_codec_ref && cid == HAL_AUDCODEC_ADC_CH0)
    {
      g_mic_stream_ready |= 2U;
    }
}

void HAL_AUDCODEC_ErrorCallback(AUDCODEC_HandleTypeDef *codec, int cid)
{
  if (codec == g_audio_dma_codec &&
      (cid == HAL_AUDCODEC_DAC_CH0 || cid == HAL_AUDCODEC_ADC_CH0))
    {
      g_audio_dma_error = true;
    }

  if (codec == g_mic_stream_codec_ref && cid == HAL_AUDCODEC_ADC_CH0)
    {
      g_mic_stream_error = true;
    }
}

static const int16_t g_tone[32] =
{
   0,  6393, 12539, 18204, 23170, 27245, 30273, 32137,
   32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
   0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
  -32768,-32137,-30273,-27245,-23170,-18204,-12539,-6393
};

typedef struct
{
  uint16_t frequency_hz;
  uint16_t duration_ms;
} audio_music_note_t;

/* Short embedded melody: C major arpeggio and cadence. */
static const audio_music_note_t g_music_melody[] =
{
  {262, 240}, {294, 240}, {330, 240}, {392, 240},
  {330, 240}, {294, 240}, {262, 360}, {0, 120},
  {330, 240}, {392, 240}, {440, 240}, {523, 360},
  {392, 240}, {330, 240}, {262, 480}
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

static void audio_dac_stop(AUDCODEC_HandleTypeDef *codec)
{
  HAL_AUDCODEC_Config_DACPath(codec, 1);
  if (codec->hdma[HAL_AUDCODEC_DAC_CH0] != NULL)
    {
      HAL_AUDCODEC_DMAStop(codec, HAL_AUDCODEC_DAC_CH0);
    }
  __HAL_AUDCODEC_DAC_DISABLE(codec);
  HAL_AUDCODEC_Close_Analog_DACPath();
}

static void audio_adc_stop(AUDCODEC_HandleTypeDef *codec)
{
  if (codec->hdma[HAL_AUDCODEC_ADC_CH0] != NULL)
    {
      HAL_AUDCODEC_DMAStop(codec, HAL_AUDCODEC_ADC_CH0);
    }
  __HAL_AUDCODEC_ADC_DISABLE(codec);
  HAL_AUDCODEC_Close_Analog_ADCPath();
}

static bool g_mic_only;
static pthread_mutex_t g_audio_hw_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_audio_voice_receiving;
static bool g_audio_voice_ready;
static bool g_audio_voice_playing;
static uint16_t g_audio_voice_stream_id;
static uint32_t g_audio_voice_expected_bytes;
static uint32_t g_audio_voice_received_bytes;
static uint32_t g_audio_voice_sample_count;
static uint32_t g_audio_voice_expected_crc32;
static uint32_t g_audio_voice_crc32;

static int audio_hw_test(void);
static int audio_speaker_alert(int duration_ms, int pattern);
static int audio_music_hw_test(void);
static int audio_mic_stream_start_locked(void);
static void audio_mic_stream_stop_locked(void);

static uint32_t audio_crc32_update(uint32_t crc, const uint8_t *data,
                                   size_t length)
{
  size_t i;

  for (i = 0; i < length; i++)
    {
      uint32_t bit;

      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc & 1U) != 0U ?
                (crc >> 1) ^ 0xedb88320U : crc >> 1;
        }
    }

  return crc;
}

static bool audio_mic_stream_suspend_locked(void)
{
  bool was_active = g_mic_stream_active;

  if (was_active)
    {
      audio_mic_stream_stop_locked();
    }

  return was_active;
}

static void audio_mic_stream_resume_locked(bool was_active)
{
  if (was_active && audio_mic_stream_start_locked() < 0)
    {
      syslog(LOG_WARNING, "[AudioStream] resume after playback failed\n");
    }
}

static int audio_mic_stream_start_locked(void)
{
  AUDCODEC_ADCCfgTypeDef adc_cfg;
  int ret;
  bool codec_initialized = false;

  if (g_mic_stream_active)
    {
      return 0;
    }

  memset(&g_mic_stream_codec, 0, sizeof(g_mic_stream_codec));
  memset(&g_mic_stream_dma, 0, sizeof(g_mic_stream_dma));
  memset(&adc_cfg, 0, sizeof(adc_cfg));

  g_mic_stream_codec.Instance = hwp_audcodec;
  g_mic_stream_codec.Init.adc_cfg.opmode = 1;
  g_mic_stream_codec.hdma[HAL_AUDCODEC_ADC_CH0] = &g_mic_stream_dma;
  g_mic_stream_dma.Instance = DMA1_Channel1;
  g_mic_stream_dma.Init.Request = DMA_REQUEST_39;

  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  ret = HAL_AUDCODEC_Init(&g_mic_stream_codec);
  if (ret != HAL_OK)
    {
      goto fail_power;
    }
  codec_initialized = true;

  HAL_TURN_ON_PLL();
  adc_cfg.opmode = 1;
  adc_cfg.adc_clk = (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clock;
  ret = HAL_AUDCODEC_Config_RChanel(&g_mic_stream_codec, 0, &adc_cfg);
  if (ret != HAL_OK)
    {
      goto fail_codec;
    }

  ret = HAL_AUDCODEC_Config_ADCPath_Volume(&g_mic_stream_codec, 0, AUDIO_MIC_ADC_GAIN_DB);
  if (ret != HAL_OK)
    {
      goto fail_codec;
    }

  HAL_AUDCODEC_Config_Analog_ADCPath(
      (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clock);
  __HAL_AUDCODEC_ADC_ENABLE(&g_mic_stream_codec);

  ret = audio_dma_attach_irqs();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[AudioStream] DMA IRQ attach failed: %d\n", ret);
      goto fail_adc;
    }

  memset(g_mic_stream_buffer, 0, sizeof(g_mic_stream_buffer));
  g_mic_stream_ready = 0;
  g_mic_stream_error = false;
  g_mic_stream_codec_ref = &g_mic_stream_codec;
  ret = HAL_AUDCODEC_Receive_DMA(
      &g_mic_stream_codec, (uint8_t *)g_mic_stream_buffer,
      sizeof(g_mic_stream_buffer), HAL_AUDCODEC_ADC_CH0);
  if (ret != HAL_OK)
    {
      g_mic_stream_codec_ref = NULL;
      syslog(LOG_ERR, "[AudioStream] ADC DMA start failed: %d\n", ret);
      goto fail_adc;
    }

  g_mic_stream_active = true;
  syslog(LOG_INFO,
         "[AudioStream] continuous MEMS capture started (%d Hz, half=%d samples)\n",
         AUDIO_TEST_RATE, AUDIO_MIC_STREAM_HALF_WORDS);
  return 0;

fail_adc:
  audio_adc_stop(&g_mic_stream_codec);
fail_codec:
  if (codec_initialized)
    {
      HAL_AUDCODEC_DeInit(&g_mic_stream_codec);
      HAL_TURN_OFF_PLL();
    }
fail_power:
  HAL_RCC_DisableModule(RCC_MOD_AUDCODEC);
  HAL_PMU_EnableAudio(0);
  return -1;
}

static void audio_mic_stream_stop_locked(void)
{
  if (!g_mic_stream_active && g_mic_stream_codec_ref == NULL)
    {
      return;
    }

  /* Hide the codec from ISR callbacks before aborting the DMA channel. */
  g_mic_stream_active = false;
  g_mic_stream_codec_ref = NULL;
  g_mic_stream_ready = 0;
  audio_adc_stop(&g_mic_stream_codec);
  HAL_AUDCODEC_DeInit(&g_mic_stream_codec);
  HAL_TURN_OFF_PLL();
  HAL_RCC_DisableModule(RCC_MOD_AUDCODEC);
  HAL_PMU_EnableAudio(0);
  syslog(LOG_INFO, "[AudioStream] continuous MEMS capture stopped\n");
}

int velawear_mic_stream_start(void)
{
  int ret;

  pthread_mutex_lock(&g_audio_hw_lock);
  ret = audio_mic_stream_start_locked();
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}

void velawear_mic_stream_stop(void)
{
  pthread_mutex_lock(&g_audio_hw_lock);
  audio_mic_stream_stop_locked();
  pthread_mutex_unlock(&g_audio_hw_lock);
}

int velawear_mic_stream_read_level(uint32_t *avg_abs, uint32_t *peak_abs)
{
  uint8_t ready;
  uint32_t offset;
  uint64_t sum_abs = 0;
  uint32_t peak = 0;
  irqstate_t flags;

  if (avg_abs == NULL || peak_abs == NULL)
    {
      return -1;
    }

  pthread_mutex_lock(&g_audio_hw_lock);
  if (!g_mic_stream_active)
    {
      pthread_mutex_unlock(&g_audio_hw_lock);
      return 0;
    }

  flags = up_irq_save();
  if (g_mic_stream_error)
    {
      up_irq_restore(flags);
      pthread_mutex_unlock(&g_audio_hw_lock);
      return -1;
    }

  ready = g_mic_stream_ready;
  if (ready & 1U)
    {
      g_mic_stream_ready &= (uint8_t)~1U;
      offset = 0;
    }
  else if (ready & 2U)
    {
      g_mic_stream_ready &= (uint8_t)~2U;
      offset = AUDIO_MIC_STREAM_HALF_WORDS;
    }
  else
    {
      up_irq_restore(flags);
      pthread_mutex_unlock(&g_audio_hw_lock);
      return 0;
    }
  up_irq_restore(flags);

  for (uint32_t i = 0; i < AUDIO_MIC_STREAM_HALF_WORDS; i++)
    {
      int32_t sample = (int16_t)(g_mic_stream_buffer[offset + i] & 0xffff);
      uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);

      sum_abs += magnitude;
      if (magnitude > peak)
        {
          peak = magnitude;
        }
    }

  *avg_abs = (uint32_t)(sum_abs / AUDIO_MIC_STREAM_HALF_WORDS);
  *peak_abs = peak;
  pthread_mutex_unlock(&g_audio_hw_lock);
  return 1;
}


int velawear_mic_hw_test(void)
{
  int ret;

  pthread_mutex_lock(&g_audio_hw_lock);
  bool resume_stream = audio_mic_stream_suspend_locked();
  g_mic_only = true;
  ret = audio_hw_test();
  g_mic_only = false;
  audio_mic_stream_resume_locked(resume_stream);
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}
static int audio_speaker_alert(int duration_ms, int pattern)
{
  AUDCODEC_HandleTypeDef codec;
  DMA_HandleTypeDef *dma = &g_audio_dma;
  AUDCODEC_DACCfgTypeDef dac_cfg;
  uint32_t samples;
  uint32_t i;
  uint32_t waited;
  int ret = HAL_OK;

  if (duration_ms <= 0)
    {
      return -1;
    }

  memset(&codec, 0, sizeof(codec));
  memset(dma, 0, sizeof(*dma));
  memset(&dac_cfg, 0, sizeof(dac_cfg));
  codec.Instance = hwp_audcodec;
  codec.Init.dac_cfg.opmode = 1;
  codec.hdma[HAL_AUDCODEC_DAC_CH0] = dma;
  dma->Instance = DMA1_Channel1;
  dma->Init.Request = DMA_REQUEST_41;

  samples = (uint32_t)(((uint64_t)AUDIO_TEST_RATE * duration_ms) / 1000);
  if (samples == 0 || samples > AUDIO_DMA_BUFFER_WORDS)
    {
      return -1;
    }

  for (i = 0; i < samples; i++)
    {
      uint32_t elapsed_ms = (i * 1000U) / AUDIO_TEST_RATE;
      uint32_t phase_ms = elapsed_ms % 1000U;
      bool audible = pattern != 8 ||
                     phase_ms < 120U ||
                     (phase_ms >= 200U && phase_ms < 320U) ||
                     (phase_ms >= 400U && phase_ms < 520U);
      g_audio_dma_buffer[i] = audible ?
        (uint32_t)(uint16_t)g_tone[i & 31] : 0;
    }

  syslog(LOG_INFO, "[AudioAlert] begin (%d ms, pattern=%d)\n",
         duration_ms, pattern);
  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  ret = HAL_AUDCODEC_Init(&codec);
  if (ret != HAL_OK)
    {
      goto cleanup_power;
    }

  dac_cfg.opmode = 1;
  dac_cfg.dac_clk = (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clock;
  ret = HAL_AUDCODEC_Config_TChanel(&codec, 0, &dac_cfg);
  if (ret != HAL_OK)
    {
      goto cleanup_codec;
    }

  __HAL_AUDCODEC_DAC_ENABLE(&codec);
  ret = HAL_AUDCODEC_Config_DACPath_Volume(&codec, 0, AUDIO_ALERT_DAC_VOLUME);
  if (ret != HAL_OK)
    {
      goto cleanup_dac;
    }

  HAL_AUDCODEC_Config_DACPath(&codec, 1);
  HAL_TURN_ON_PLL();
  ret = HAL_AUDCODEC_Config_Analog_DACPath(
          (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clock);
  if (ret != HAL_OK)
    {
      goto cleanup_dac;
    }

  ret = audio_dma_attach_irqs();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[AudioAlert] DMA IRQ attach failed: %d\n", ret);
      goto cleanup_dac;
    }

  g_audio_dma_codec = &codec;
  g_audio_dma_complete = false;
  g_audio_dma_error = false;
  ret = HAL_AUDCODEC_Transmit_DMA(
          &codec, (uint8_t *)g_audio_dma_buffer, samples * sizeof(uint32_t),
          HAL_AUDCODEC_DAC_CH0);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioAlert] DMA start failed: %d\n", ret);
      g_audio_dma_codec = NULL;
      goto cleanup_dac;
    }

#if VELAWEAR_ENABLE_EXTERNAL_PA
  audio_pa_set(true);
#else
  audio_pa_set(false);
  syslog(LOG_WARNING,
         "[AudioAlert] external PA disabled after reset-safe HIL\n");
#endif
  HAL_Delay_us(30000);
  HAL_AUDCODEC_Config_DACPath(&codec, 0);

  for (waited = 0; waited < (uint32_t)duration_ms + 500U; waited++)
    {
      if (g_audio_dma_complete || g_audio_dma_error)
        {
          break;
        }
      usleep(1000);
    }
  if (!g_audio_dma_complete || g_audio_dma_error)
    {
      ret = g_audio_dma_error ? HAL_ERROR : HAL_TIMEOUT;
      syslog(LOG_WARNING, "[AudioAlert] DMA %s after %lu ms\n",
             g_audio_dma_error ? "error" : "timeout",
             (unsigned long)waited);
    }
  else
    {
      syslog(LOG_INFO, "[AudioAlert] DMA complete\n");
    }
  g_audio_dma_codec = NULL;

  audio_pa_set(false);
  syslog(LOG_INFO, "[AudioAlert] tone DMA complete\n");

cleanup_dac:
  audio_dac_stop(&codec);
cleanup_codec:
  HAL_AUDCODEC_DeInit(&codec);
  HAL_TURN_OFF_PLL();
cleanup_power:
  HAL_PMU_EnableAudio(0);
  syslog(ret == HAL_OK ? LOG_INFO : LOG_WARNING,
         "[AudioAlert] speaker alert %s (%d ms, pattern=%d)\n",
         ret == HAL_OK ? "played" : "failed", duration_ms, pattern);
  return ret == HAL_OK ? 0 : -1;
}

static int audio_voice_play_locked(uint32_t samples)
{
  AUDCODEC_HandleTypeDef codec;
  DMA_HandleTypeDef *dma = &g_audio_dma;
  AUDCODEC_DACCfgTypeDef dac_cfg;
  uint32_t duration_ms;
  uint32_t waited;
  int ret = HAL_OK;

  if (samples == 0 || samples > AUDIO_DMA_BUFFER_WORDS)
    {
      return -1;
    }

  duration_ms = (uint32_t)(((uint64_t)samples * 1000U +
                            AUDIO_TEST_RATE - 1U) / AUDIO_TEST_RATE);
  memset(&codec, 0, sizeof(codec));
  memset(dma, 0, sizeof(*dma));
  memset(&dac_cfg, 0, sizeof(dac_cfg));
  codec.Instance = hwp_audcodec;
  codec.Init.dac_cfg.opmode = 1;
  codec.hdma[HAL_AUDCODEC_DAC_CH0] = dma;
  dma->Instance = DMA1_Channel1;
  dma->Init.Request = DMA_REQUEST_41;

  syslog(LOG_INFO,
         "[AudioVoice] begin stream=%u samples=%lu duration=%lu ms\n",
         (unsigned int)g_audio_voice_stream_id, (unsigned long)samples,
         (unsigned long)duration_ms);
  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  ret = HAL_AUDCODEC_Init(&codec);
  if (ret != HAL_OK)
    {
      goto cleanup_power;
    }

  dac_cfg.opmode = 1;
  dac_cfg.dac_clk = (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clock;
  ret = HAL_AUDCODEC_Config_TChanel(&codec, 0, &dac_cfg);
  if (ret != HAL_OK)
    {
      goto cleanup_codec;
    }

  __HAL_AUDCODEC_DAC_ENABLE(&codec);
  ret = HAL_AUDCODEC_Config_DACPath_Volume(&codec, 0,
                                            AUDIO_ALERT_DAC_VOLUME);
  if (ret != HAL_OK)
    {
      goto cleanup_dac;
    }

  /* Keep the DAC muted while clocks, DMA and the analog path settle. */
  HAL_AUDCODEC_Config_DACPath(&codec, 1);
  HAL_TURN_ON_PLL();
  ret = HAL_AUDCODEC_Config_Analog_DACPath(
          (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clock);
  if (ret != HAL_OK)
    {
      goto cleanup_dac;
    }

  ret = audio_dma_attach_irqs();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[AudioVoice] DMA IRQ attach failed: %d\n", ret);
      goto cleanup_dac;
    }

  g_audio_dma_codec = &codec;
  g_audio_dma_complete = false;
  g_audio_dma_error = false;
  ret = HAL_AUDCODEC_Transmit_DMA(
          &codec, (uint8_t *)g_audio_dma_buffer,
          samples * sizeof(uint32_t), HAL_AUDCODEC_DAC_CH0);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioVoice] DMA start failed: %d\n", ret);
      g_audio_dma_codec = NULL;
      goto cleanup_dac;
    }

#if VELAWEAR_ENABLE_EXTERNAL_PA
  audio_pa_set(true);
#else
  audio_pa_set(false);
  syslog(LOG_WARNING,
         "[AudioVoice] external PA disabled after reset-safe HIL\n");
#endif
  HAL_Delay_us(30000);
  HAL_AUDCODEC_Config_DACPath(&codec, 0);

  for (waited = 0; waited < duration_ms + 500U; waited++)
    {
      if (g_audio_dma_complete || g_audio_dma_error)
        {
          break;
        }
      usleep(1000);
    }

  if (!g_audio_dma_complete || g_audio_dma_error)
    {
      ret = g_audio_dma_error ? HAL_ERROR : HAL_TIMEOUT;
      syslog(LOG_WARNING, "[AudioVoice] DMA %s after %lu ms\n",
             g_audio_dma_error ? "error" : "timeout",
             (unsigned long)waited);
    }
  else
    {
      syslog(LOG_INFO, "[AudioVoice] DMA complete\n");
    }
  g_audio_dma_codec = NULL;

  audio_pa_set(false);

cleanup_dac:
  audio_dac_stop(&codec);
cleanup_codec:
  HAL_AUDCODEC_DeInit(&codec);
  HAL_TURN_OFF_PLL();
cleanup_power:
  HAL_PMU_EnableAudio(0);
  syslog(ret == HAL_OK ? LOG_INFO : LOG_WARNING,
         "[AudioVoice] speaker voice %s (stream=%u, samples=%lu)\n",
         ret == HAL_OK ? "played" : "failed",
         (unsigned int)g_audio_voice_stream_id, (unsigned long)samples);
  return ret == HAL_OK ? 0 : -1;
}


static int audio_music_hw_test(void)
{
  AUDCODEC_HandleTypeDef codec;
  DMA_HandleTypeDef *dma = &g_audio_dma;
  AUDCODEC_DACCfgTypeDef dac_cfg;
  int ret = HAL_OK;
  uint32_t phase = 0;
  uint32_t total_samples = 0;
  uint32_t total_ms = 0;
  uint32_t wait_ms;
  uint32_t waited = 0;
  size_t note_index;

  memset(&codec, 0, sizeof(codec));
  memset(dma, 0, sizeof(*dma));
  memset(&dac_cfg, 0, sizeof(dac_cfg));
  codec.Instance = hwp_audcodec;
  codec.Init.dac_cfg.opmode = 1;
  codec.hdma[HAL_AUDCODEC_DAC_CH0] = dma;
  /* The official SF32LB52 mapping uses request 41 for DAC0.  With the
   * dynamic allocator the initial channel is only a hint; the HAL moves it
   * to a free DMAC1 channel and the generic IRQ shims above dispatch it. */
  dma->Instance = DMA1_Channel1;
  dma->Init.Request = DMA_REQUEST_41;

  syslog(LOG_INFO, "[AudioMusic] DMA melody begin\n");

  /* Render the finite melody once.  The DAC DMA buffer is circular in the
   * SiFli HAL, so the first transfer-complete callback is our end marker. */
  for (note_index = 0;
       note_index < sizeof(g_music_melody) / sizeof(g_music_melody[0]);
       note_index++)
    {
      const audio_music_note_t *note = &g_music_melody[note_index];
      uint32_t samples = (uint32_t)(((uint64_t)AUDIO_MUSIC_RATE *
                                     note->duration_ms) / 1000);
      uint64_t step = note->frequency_hz == 0 ? 0 :
          (((uint64_t)note->frequency_hz << 32) / AUDIO_MUSIC_RATE);
      uint32_t i;

      total_ms += note->duration_ms;
      for (i = 0; i < samples && total_samples < AUDIO_DMA_BUFFER_WORDS;
           i++, total_samples++)
        {
          int16_t sample = note->frequency_hz == 0 ? 0 :
            g_tone[(phase >> 27) & 31];
          g_audio_dma_buffer[total_samples] = (uint32_t)(uint16_t)sample;
          phase += (uint32_t)step;
        }
    }

  if (total_samples == 0 || total_samples >= AUDIO_DMA_BUFFER_WORDS)
    {
      syslog(LOG_ERR, "[AudioMusic] DMA buffer sizing failed (%lu)\n",
             (unsigned long)total_samples);
      return -1;
    }

  audio_pa_set(false);
  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  ret = HAL_AUDCODEC_Init(&codec);
  if (ret != HAL_OK)
    {
      goto cleanup_power;
    }

  dac_cfg.opmode = 1;
  {
    static AUDCODE_DAC_CLK_CONFIG_TYPE music_clock =
      { AUDIO_MUSIC_RATE, 0, 20, 2, 0x14d, 0, 10, 2, 2, 10, 10, 0 };
    dac_cfg.dac_clk = &music_clock;
  }
  ret = HAL_AUDCODEC_Config_TChanel(&codec, 0, &dac_cfg);
  if (ret != HAL_OK)
    {
      goto cleanup_codec;
    }

  __HAL_AUDCODEC_DAC_ENABLE(&codec);
  ret = HAL_AUDCODEC_Config_DACPath_Volume(&codec, 0, 0);
  if (ret != HAL_OK)
    {
      goto cleanup_dac;
    }

  /* Keep the DAC muted while clocks, DMA and the analog path settle. */
  HAL_AUDCODEC_Config_DACPath(&codec, 1);
  HAL_TURN_ON_PLL();
  ret = HAL_AUDCODEC_Config_Analog_DACPath(
          dac_cfg.dac_clk);
  if (ret != HAL_OK)
    {
      goto cleanup_dac;
    }

  ret = audio_dma_attach_irqs();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[AudioMusic] DMA IRQ attach failed: %d\n", ret);
      goto cleanup_dac;
    }

  g_audio_dma_codec = &codec;
  g_audio_dma_complete = false;
  g_audio_dma_error = false;
  ret = HAL_AUDCODEC_Transmit_DMA(
          &codec, (uint8_t *)g_audio_dma_buffer, total_samples * sizeof(uint32_t),
          HAL_AUDCODEC_DAC_CH0);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioMusic] DMA start failed: %d\n", ret);
      g_audio_dma_codec = NULL;
      goto cleanup_dac;
    }

  audio_pa_set(true);
  HAL_Delay_us(30000);
  HAL_AUDCODEC_Config_DACPath(&codec, 0);

  syslog(LOG_INFO, "[AudioMusic] DMA started samples=%lu duration=%lu ms channel=%lu\n",
         (unsigned long)total_samples, (unsigned long)total_ms,
         (unsigned long)((dma->ChannelIndex >> 2) + 1U));
  wait_ms = total_ms + 500;
  while (!g_audio_dma_complete && !g_audio_dma_error && waited < wait_ms)
    {
      usleep(1000);
      waited++;
    }

  if (g_audio_dma_error)
    {
      ret = HAL_ERROR;
      syslog(LOG_ERR, "[AudioMusic] DMA error\n");
    }
  else if (!g_audio_dma_complete)
    {
      ret = HAL_TIMEOUT;
      syslog(LOG_ERR, "[AudioMusic] DMA timeout after %lu ms\n",
             (unsigned long)waited);
    }
  else
    {
      syslog(LOG_INFO, "[AudioMusic] DMA complete\n");
    }
  g_audio_dma_codec = NULL;

cleanup_dac:
  audio_pa_set(false);
  audio_dac_stop(&codec);
cleanup_codec:
  HAL_AUDCODEC_DeInit(&codec);
  HAL_TURN_OFF_PLL();
cleanup_power:
  HAL_PMU_EnableAudio(0);
  syslog(ret == HAL_OK ? LOG_INFO : LOG_WARNING,
         "[AudioMusic] melody %s (%d)\n",
         ret == HAL_OK ? "played" : "failed", ret);
  return ret == HAL_OK ? 0 : -1;
}

static int audio_hw_test(void)
{
  AUDCODEC_HandleTypeDef codec;
  AUDCODEC_ADCCfgTypeDef adc_cfg;
  int ret = HAL_OK;

  memset(&codec, 0, sizeof(codec));
  memset(&adc_cfg, 0, sizeof(adc_cfg));
  memset(&g_audio_dma, 0, sizeof(g_audio_dma));
  codec.Instance = hwp_audcodec;
  codec.Init.adc_cfg.opmode = 1;

  if (!g_mic_only)
    {
      ret = audio_speaker_alert(AUDIO_TEST_SECONDS * 1000, 0);
      if (ret != 0)
        {
          return ret;
        }
    }

  codec.hdma[HAL_AUDCODEC_ADC_CH0] = &g_audio_dma;
  g_audio_dma.Instance = DMA1_Channel1;
  g_audio_dma.Init.Request = DMA_REQUEST_39;

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

  adc_cfg.opmode = 1;
  HAL_TURN_ON_PLL();
  adc_cfg.adc_clk = (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clock;
  ret = HAL_AUDCODEC_Config_RChanel(&codec, 0, &adc_cfg);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] ADC channel config failed: %d\n", ret);
      goto cleanup;
    }

  ret = HAL_AUDCODEC_Config_ADCPath_Volume(&codec, 0, AUDIO_MIC_ADC_GAIN_DB);
  if (ret != HAL_OK)
    {
      syslog(LOG_ERR, "[AudioTest] ADC gain config failed: %d\n", ret);
      goto cleanup;
    }
  HAL_AUDCODEC_Config_Analog_ADCPath((AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clock);
  __HAL_AUDCODEC_ADC_ENABLE(&codec);
  {
    int32_t min_sample = INT32_MAX;
    int32_t max_sample = INT32_MIN;
    uint64_t sum_abs = 0;
    uint32_t count_abs_1000 = 0;
    uint32_t count_abs_5000 = 0;
    uint32_t peak_abs = 0;
    uint32_t samples = 0;
    uint32_t waited_ms = 0;

    memset(g_audio_dma_buffer, 0, sizeof(g_audio_dma_buffer));
    ret = audio_dma_attach_irqs();
    if (ret < 0)
      {
        syslog(LOG_ERR, "[AudioTest] DMA IRQ attach failed: %d\n", ret);
        goto cleanup;
      }

    g_audio_dma_codec = &codec;
    g_audio_dma_complete = false;
    g_audio_dma_rx_complete = false;
    g_audio_dma_error = false;
    ret = HAL_AUDCODEC_Receive_DMA(&codec, (uint8_t *)g_audio_dma_buffer,
                                   sizeof(g_audio_dma_buffer),
                                   HAL_AUDCODEC_ADC_CH0);
    if (ret != HAL_OK)
      {
        syslog(LOG_ERR, "[AudioTest] ADC DMA start failed: %d\n", ret);
        goto cleanup;
      }

    syslog(LOG_INFO, "[AudioTest] MICROPHONE: DMA sampling for %d seconds\n",
           AUDIO_TEST_SECONDS);

    while (!g_audio_dma_rx_complete && !g_audio_dma_error &&
           waited_ms < (AUDIO_TEST_SECONDS * 1000 + 500))
      {
        usleep(1000);
        waited_ms++;
      }

    if (g_audio_dma_error)
      {
        ret = HAL_ERROR;
        syslog(LOG_ERR, "[AudioTest] MICROPHONE: DMA error\n");
      }
    else if (!g_audio_dma_rx_complete)
      {
        ret = HAL_TIMEOUT;
        syslog(LOG_ERR, "[AudioTest] MICROPHONE: DMA timeout after %lu ms\n",
               (unsigned long)waited_ms);
      }
    else
      {
        samples = AUDIO_DMA_BUFFER_WORDS;
        for (uint32_t n = 0; n < samples; n++)
          {
            int32_t sample = (int16_t)(g_audio_dma_buffer[n] & 0xffff);
            if (sample < min_sample)
              {
                min_sample = sample;
              }
            if (sample > max_sample)
              {
                max_sample = sample;
              }
            {
              uint32_t abs_sample = (uint32_t)(sample < 0 ? -sample : sample);
              sum_abs += abs_sample;
              if (abs_sample > peak_abs)
                {
                  peak_abs = abs_sample;
                }
              if (abs_sample >= 1000)
                {
                  count_abs_1000++;
                }
              if (abs_sample >= 5000)
                {
                  count_abs_5000++;
                }
            }
          }
        syslog(LOG_INFO,
               "[AudioTest] MICROPHONE: DMA samples=%lu min=%ld max=%ld avg_abs=%lu\n",
               (unsigned long)samples, (long)min_sample, (long)max_sample,
               (unsigned long)(sum_abs / samples));
        syslog(LOG_INFO,
               "[AudioTest] MICROPHONE: DMA peak_abs=%lu above_1000=%lu above_5000=%lu\n",
               (unsigned long)peak_abs, (unsigned long)count_abs_1000,
               (unsigned long)count_abs_5000);
      }
  }

  audio_adc_stop(&codec);
  g_audio_dma_codec = NULL;

cleanup:
  audio_pa_set(false);
  HAL_AUDCODEC_DeInit(&codec);
  HAL_TURN_OFF_PLL();
  HAL_PMU_EnableAudio(0);
  syslog(LOG_INFO, "[AudioTest] complete\n");
  return ret == HAL_OK ? 0 : -1;
}

int velawear_audio_voice_start(uint16_t stream_id,
                               uint32_t sample_count,
                               uint32_t total_bytes,
                               uint32_t crc32)
{
  int ret = 0;

  pthread_mutex_lock(&g_audio_hw_lock);
  if (g_audio_voice_receiving || g_audio_voice_ready ||
      g_audio_voice_playing)
    {
      ret = -1;
    }
  else if (sample_count == 0 || sample_count > AUDIO_DMA_BUFFER_WORDS ||
           total_bytes != sample_count * 2U ||
           total_bytes > VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES)
    {
      ret = -1;
    }
  else
    {
      g_audio_voice_receiving = true;
      g_audio_voice_ready = false;
      g_audio_voice_stream_id = stream_id;
      g_audio_voice_expected_bytes = total_bytes;
      g_audio_voice_received_bytes = 0;
      g_audio_voice_sample_count = sample_count;
      g_audio_voice_expected_crc32 = crc32;
      g_audio_voice_crc32 = 0xffffffffU;
    }
  pthread_mutex_unlock(&g_audio_hw_lock);

  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[AudioVoice] reject start stream=%u samples=%lu bytes=%lu\n",
             (unsigned int)stream_id, (unsigned long)sample_count,
             (unsigned long)total_bytes);
    }
  return ret;
}

int velawear_audio_voice_write(uint16_t stream_id,
                               uint32_t offset,
                               const uint8_t *data,
                               uint16_t length)
{
  uint16_t i;
  int ret = 0;

  pthread_mutex_lock(&g_audio_hw_lock);
  if (!g_audio_voice_receiving || stream_id != g_audio_voice_stream_id ||
      data == NULL || length == 0 || (length & 1U) != 0U ||
      offset != g_audio_voice_received_bytes ||
      offset > g_audio_voice_expected_bytes ||
      length > g_audio_voice_expected_bytes - offset)
    {
      ret = -1;
    }
  else
    {
      for (i = 0; i < length; i += 2)
        {
          uint16_t sample = (uint16_t)data[i] |
                            ((uint16_t)data[i + 1] << 8);
          g_audio_dma_buffer[(offset + i) / 2U] = (uint32_t)sample;
        }
      g_audio_voice_crc32 = audio_crc32_update(g_audio_voice_crc32,
                                                data, length);
      g_audio_voice_received_bytes += length;
    }
  pthread_mutex_unlock(&g_audio_hw_lock);

  return ret;
}

int velawear_audio_voice_end(uint16_t stream_id,
                             uint32_t total_bytes,
                             uint32_t crc32,
                             uint32_t *sample_count)
{
  uint32_t actual_crc32;
  int ret = 0;

  pthread_mutex_lock(&g_audio_hw_lock);
  if (!g_audio_voice_receiving || stream_id != g_audio_voice_stream_id ||
      total_bytes != g_audio_voice_expected_bytes ||
      g_audio_voice_received_bytes != g_audio_voice_expected_bytes)
    {
      ret = -1;
    }
  else
    {
      actual_crc32 = g_audio_voice_crc32 ^ 0xffffffffU;
      if (actual_crc32 != g_audio_voice_expected_crc32 ||
          actual_crc32 != crc32)
        {
          ret = -1;
        }
      else
        {
          g_audio_voice_receiving = false;
          g_audio_voice_ready = true;
          if (sample_count != NULL)
            {
              *sample_count = g_audio_voice_sample_count;
            }
          syslog(LOG_INFO,
                 "[AudioVoice] stream complete id=%u samples=%lu bytes=%lu\n",
                 (unsigned int)stream_id,
                 (unsigned long)g_audio_voice_sample_count,
                 (unsigned long)g_audio_voice_expected_bytes);
        }
    }

  if (ret < 0)
    {
      g_audio_voice_receiving = false;
      g_audio_voice_ready = false;
      syslog(LOG_WARNING,
             "[AudioVoice] stream rejected id=%u received=%lu expected=%lu\n",
             (unsigned int)stream_id,
             (unsigned long)g_audio_voice_received_bytes,
             (unsigned long)g_audio_voice_expected_bytes);
    }
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}

int velawear_audio_voice_abort(uint16_t stream_id)
{
  int ret = 0;

  pthread_mutex_lock(&g_audio_hw_lock);
  if (g_audio_voice_playing ||
      (g_audio_voice_receiving == false && g_audio_voice_ready == false) ||
      stream_id != g_audio_voice_stream_id)
    {
      ret = -1;
    }
  else
    {
      g_audio_voice_receiving = false;
      g_audio_voice_ready = false;
      g_audio_voice_received_bytes = 0;
      g_audio_voice_expected_bytes = 0;
      g_audio_voice_sample_count = 0;
      g_audio_voice_expected_crc32 = 0;
      g_audio_voice_crc32 = 0xffffffffU;
    }
  pthread_mutex_unlock(&g_audio_hw_lock);

  if (ret == 0)
    {
      syslog(LOG_WARNING, "[AudioVoice] stream aborted id=%u\n",
             (unsigned int)stream_id);
    }
  return ret;
}

int velawear_audio_voice_play(uint16_t stream_id, uint32_t sample_count)
{
  bool resume_stream = false;
  int ret;

  pthread_mutex_lock(&g_audio_hw_lock);
  if (!g_audio_voice_ready || g_audio_voice_playing ||
      stream_id != g_audio_voice_stream_id ||
      sample_count != g_audio_voice_sample_count)
    {
      pthread_mutex_unlock(&g_audio_hw_lock);
      return -1;
    }

  g_audio_voice_ready = false;
  g_audio_voice_playing = true;
  resume_stream = audio_mic_stream_suspend_locked();
  ret = audio_voice_play_locked(sample_count);
  audio_mic_stream_resume_locked(resume_stream);
  g_audio_voice_playing = false;
  if (ret == 0)
    {
      g_audio_voice_received_bytes = 0;
      g_audio_voice_expected_bytes = 0;
      g_audio_voice_sample_count = 0;
    }
  pthread_mutex_unlock(&g_audio_hw_lock);

  return ret;
}

int velawear_speaker_alert(int duration_ms, int pattern)
{
  int ret;

  pthread_mutex_lock(&g_audio_hw_lock);
  bool resume_stream = audio_mic_stream_suspend_locked();
  ret = audio_speaker_alert(duration_ms, pattern);
  audio_mic_stream_resume_locked(resume_stream);
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}

int velawear_music_hw_test(void)
{
  int ret;

  pthread_mutex_lock(&g_audio_hw_lock);
  bool resume_stream = audio_mic_stream_suspend_locked();
  ret = audio_music_hw_test();
  audio_mic_stream_resume_locked(resume_stream);
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}

int velawear_audio_hw_test(void)
{
  int ret;

  pthread_mutex_lock(&g_audio_hw_lock);
  bool resume_stream = audio_mic_stream_suspend_locked();
  ret = audio_hw_test();
  audio_mic_stream_resume_locked(resume_stream);
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}
int velawear_mic_stream_read_pcm(int16_t *samples, size_t sample_count)
{
  uint8_t ready;
  uint32_t offset;
  irqstate_t flags;

  if (samples == NULL || sample_count != AUDIO_MIC_STREAM_HALF_WORDS)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_audio_hw_lock);
  if (!g_mic_stream_active)
    {
      pthread_mutex_unlock(&g_audio_hw_lock);
      return -ENODEV;
    }

  flags = up_irq_save();
  if (g_mic_stream_error)
    {
      up_irq_restore(flags);
      pthread_mutex_unlock(&g_audio_hw_lock);
      return -EIO;
    }

  ready = g_mic_stream_ready;
  if (ready & 1U)
    {
      g_mic_stream_ready &= (uint8_t)~1U;
      offset = 0;
    }
  else if (ready & 2U)
    {
      g_mic_stream_ready &= (uint8_t)~2U;
      offset = AUDIO_MIC_STREAM_HALF_WORDS;
    }
  else
    {
      up_irq_restore(flags);
      pthread_mutex_unlock(&g_audio_hw_lock);
      return 0;
    }
  up_irq_restore(flags);

  for (size_t i = 0; i < sample_count; i++)
    {
      samples[i] = (int16_t)(g_mic_stream_buffer[offset + i] & 0xffffU);
    }
  pthread_mutex_unlock(&g_audio_hw_lock);
  return (int)sample_count;
}

int velawear_audio_pcm_play(const int16_t *samples, size_t sample_count)
{
  bool resume_stream;
  int ret;

  if (samples == NULL || sample_count == 0 ||
      sample_count > AUDIO_DMA_BUFFER_WORDS)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_audio_hw_lock);
  for (size_t i = 0; i < sample_count; i++)
    {
      g_audio_dma_buffer[i] = (uint32_t)(uint16_t)samples[i];
    }

  resume_stream = audio_mic_stream_suspend_locked();
  ret = audio_voice_play_locked((uint32_t)sample_count);
  audio_mic_stream_resume_locked(resume_stream);
  pthread_mutex_unlock(&g_audio_hw_lock);
  return ret;
}
