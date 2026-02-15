#include <stdint.h>
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "mdns.h"
#include "net_functions.h"
#include "network_interface.h"
#include "nvs_flash.h"

#include <sys/time.h>

#include "driver/i2s_std.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#include "dsp_processor_settings.h"
#endif

#include "snapclient.h"
#include "ota_server.h"
#include "player.h"
#include "settings_manager.h"
#include "ui_http_server.h"
#if CONFIG_DAC_TAS5805M
#include "tas5805m_settings.h"
#endif

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY
// 1  // tskNO_AFFINITY

TaskHandle_t t_main_task = NULL;
TaskHandle_t t_ota_task = NULL;

/* Logging tag */
static const char *TAG = "SC";

SemaphoreHandle_t playerStateChangedMutex = NULL;

typedef struct audioDACdata_s {
  bool playerMute;
  bool stateMute;
  int volume;
} audioDACdata_t;

static audioDACdata_t audioDAC_data;
static QueueHandle_t audioDACQHdl = NULL;
static SemaphoreHandle_t audioDACSemaphore = NULL;

/**
 *
 */
static void dac_control(audio_board_handle_t board_handle,
                             audioDACdata_t dac_data) {
  static audioDACdata_t dac_data_old = {
      .playerMute = true,
      .stateMute = true,
      .volume = -1,
  };
  static bool muted = true;
  // TODO: can and should we pass audio_hal_handle_t instead of
  // audio_board_handle_t?
  if (dac_data.playerMute != dac_data_old.playerMute ||
      dac_data.stateMute != dac_data_old.stateMute) {
    // if either player or state mute is active, we need to mute the output
    bool mute = dac_data.playerMute || dac_data.stateMute;
    if (mute != muted) {
      muted = mute;
      audio_hal_set_mute(board_handle->audio_hal, muted);
    }
  }
  if (dac_data.volume != dac_data_old.volume) {
    audio_hal_set_volume(board_handle->audio_hal, dac_data.volume);
  }
    dac_data_old = dac_data;
}

/**
 * Set mute state. If set_state is true, it reflects the snapclient state. Otherwise it
 * is coming from player for temporary muting e.g. during startup.
 */
void audio_set_mute(bool mute, bool set_state) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (set_state && (mute != audioDAC_data.stateMute)) {
    audioDAC_data.stateMute = mute;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  else if (!set_state && mute != audioDAC_data.playerMute) {
    audioDAC_data.playerMute = mute;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

void player_set_mute(bool mute) {
  audio_set_mute(mute, false);
}

void set_mute_state(bool mute) {
  audio_set_mute(mute, true);
}

/**
 *
 */
void audio_set_volume(int volume) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (volume != audioDAC_data.volume) {
    audioDAC_data.volume = volume;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

void player_state_changed() {
  if (playerStateChangedMutex != NULL) {
    xSemaphoreGive(playerStateChangedMutex);
  }
  ESP_LOGI(TAG, "main task cb");
}

/**
 *
 */
void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
//  ESP_ERROR_CHECK(nvs_flash_erase());
  ESP_ERROR_CHECK(ret);

  esp_log_level_set("*", ESP_LOG_INFO);

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_log_level_set("uart", ESP_LOG_WARN);
  // esp_log_level_set("i2s_std", ESP_LOG_DEBUG);
  // esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  esp_log_level_set("httpd_uri", ESP_LOG_WARN);
  esp_log_level_set("settings", ESP_LOG_DEBUG);
  esp_log_level_set("dsp_settings", ESP_LOG_DEBUG);
  esp_log_level_set("UI_HTTP", ESP_LOG_WARN);
  esp_log_level_set("dspProc", ESP_LOG_DEBUG);

  t_main_task = xTaskGetCurrentTaskHandle();

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  // clang-format off
  // nINT/REFCLKO Function Select Configuration Strap
  //  • When nINTSEL is floated or pulled to
  //    VDD2A, nINT is selected for operation on the
  //    nINT/REFCLKO pin (default).
  //  • When nINTSEL is pulled low to VSS, REF-
  //    CLKO is selected for operation on the nINT/
  //    REFCLKO pin.
  //
  // LAN8720 doesn't stop REFCLK while in reset, so we leave the
  // strap floated. It is connected to IO0 on ESP32 so we get nINT
  // function with a HIGH pin value, which is also perfect during boot.
  // Before initializing LAN8720 (which resets the PHY) we pull the
  // strap low and this results in REFCLK enabled which is needed
  // for MAC unit.
  //
  // clang-format on
  gpio_config_t cfg = {.pin_bit_mask = BIT64(GPIO_NUM_5),
                       .mode = GPIO_MODE_DEF_INPUT,
                       .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&cfg);
#endif

  board_i2s_pin_t pin_config0;
  get_i2s_pins(I2S_NUM_0, &pin_config0);

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  // some codecs need i2s mclk for initialization

  i2s_chan_handle_t tx_chan;

  i2s_chan_config_t tx_chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = 128,
      .auto_clear = true,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  i2s_std_clk_config_t i2s_clkcfg = {
      .sample_rate_hz = 44100,
      .clk_src = I2S_CLK_SRC_APLL,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = pin_config0
                          .mck_io_num,  // some codecs may require mclk signal,
                                        // this example doesn't need it
              .bclk = pin_config0.bck_io_num,
              .ws = pin_config0.ws_io_num,
              .dout = pin_config0.data_out_num,
              .din = pin_config0.data_in_num,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
  i2s_channel_enable(tx_chan);
#endif

  ESP_LOGI(TAG, "Start codec chip");
  audio_board_handle_t board_handle = audio_board_init();
  if (board_handle) {
    ESP_LOGI(TAG, "Audio board_init done");
  } else {
    ESP_LOGE(TAG,
             "Audio board couldn't be initialized. Check menuconfig if project "
             "is configured right or check your wiring!");

    vTaskDelay(portMAX_DELAY);
  }

  audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                       AUDIO_HAL_CTRL_STOP);
  audio_hal_set_mute(board_handle->audio_hal,
                     true);  // ensure no noise is sent after firmware crash

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
#endif

  //  ESP_LOGI(TAG, "init player");
  i2s_std_gpio_config_t i2s_pin_config0 = {
      .mclk = pin_config0.mck_io_num,
      .bclk = pin_config0.bck_io_num,
      .ws = pin_config0.ws_io_num,
      .dout = pin_config0.data_out_num,
      .din = pin_config0.data_in_num,
      .invert_flags =
          {
#if CONFIG_INVERT_MCLK_LEVEL
              .mclk_inv = true,

#else
              .mclk_inv = false,
#endif

#if CONFIG_INVERT_BCLK_LEVEL
              .bclk_inv = true,
#else
              .bclk_inv = false,
#endif

#if CONFIG_INVERT_WORD_SELECT_LEVEL
              .ws_inv = true,
#else
              .ws_inv = false,
#endif
          },
  };

  audioDACQHdl = xQueueCreate(1, sizeof(audioDACdata_t));
  audioDACSemaphore = xSemaphoreCreateMutex();
  audioDAC_data.stateMute = true;
  audioDAC_data.playerMute = true;
  audioDAC_data.volume = -1;

  init_player(i2s_pin_config0, I2S_NUM_0, player_set_mute);
  add_player_state_cb(player_state_changed);

  // Create binary semaphore for player state change notification
  playerStateChangedMutex = xSemaphoreCreateBinary();
  if (playerStateChangedMutex == NULL) {
    ESP_LOGE(TAG, "Failed to create playerStateChangedMutex");
    return;
  }

  #if CONFIG_DAC_TAS5805M
  // Apply persisted TAS5805M settings now that the codec has been initialized
  if (tas5805m_settings_init() != ESP_OK) {
    ESP_LOGW(TAG, "Failed to init persisted TAS5805M settings");
  }
  #endif

  network_if_init();

  // Initialize settings manager (hostname + snapserver settings)
  settings_manager_init();

  // Get hostname for mDNS
  char mdns_hostname[64] = {0};
  if (settings_get_hostname(mdns_hostname, sizeof(mdns_hostname)) != ESP_OK) {
    strncpy(mdns_hostname, "snapclient", sizeof(mdns_hostname) - 1);
  }
  ESP_LOGI(TAG, "Device hostname: %s", mdns_hostname);

  init_http_server_task();

  // Enable websocket server
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

  net_mdns_register(mdns_hostname);
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp();
#endif

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();  // Must init processor first (creates mutexes/semaphores)
  dsp_settings_init();   // Then settings can restore params into the processor
#endif

  xTaskCreatePinnedToCore(&ota_server_task, "ota", 14 * 256, NULL,
                          OTA_TASK_PRIORITY, &t_ota_task, OTA_TASK_CORE_ID);
  init_snapcast(audio_set_volume, set_mute_state);

  //  while (1) {
  //    // audio_event_iface_msg_t msg;
  //    vTaskDelay(portMAX_DELAY);  //(pdMS_TO_TICKS(5000));
  //
  //    // ma120_read_error(0x20);
  //
  //    esp_err_t ret = 0;  // audio_event_iface_listen(evt, &msg,
  //    portMAX_DELAY); if (ret != ESP_OK) {
  //      ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
  //      continue;
  //    }
  //  }

#if CONFIG_PM_ENABLE
  // Configure dynamic frequency scaling:
  // automatic light sleep is enabled if tickless idle support is enabled.
  esp_pm_config_t pmConfig = {
      .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,  // Maximum CPU frequency
      .min_freq_mhz = 40,                               // Minimum CPU frequency
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
      .light_sleep_enable = true
#endif
  };
  esp_pm_configure(&pmConfig);
#endif
  audioDACdata_t dac_data;
  player_state_e state = IDLE;
  int counter = 0;
  while (1) {
    if (xQueueReceive(audioDACQHdl, &dac_data, pdMS_TO_TICKS(100)) == pdTRUE) {
      dac_control(board_handle, dac_data);
    }
    if (xSemaphoreTake(playerStateChangedMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      player_state_e state_new = get_player_state();
      ESP_LOGI(TAG, "main got cb: %d", state_new);
      if (state_new != state) {
        ESP_LOGI(TAG, "Player state changed: %d -> %d", state, state_new);
        if (state_new == PLAYING) {
          audio_hal_ctrl_codec(board_handle->audio_hal,
                                AUDIO_HAL_CODEC_MODE_DECODE,
                                AUDIO_HAL_CTRL_START);
        } else if (state == PLAYING) {
          audio_hal_ctrl_codec(board_handle->audio_hal,
                                AUDIO_HAL_CODEC_MODE_DECODE,
                                AUDIO_HAL_CTRL_STOP);
        }
        state = state_new;
      }
    }
    // test pause/play toggle every 200 loops
    // counter++;
    // if (counter % 200 == 0) {
    //   ESP_LOGI(TAG, "toggle pause: %d", state == PLAYING);
    //   pause_player(state == PLAYING);
    // }
  }
}
