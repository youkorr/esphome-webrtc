// Media bring-up for ESP-WebRTC on the ESP32-P4.
//
// Adapted from Espressif's reference implementation:
//   esp-webrtc-solution/solutions/videocall_demo/main/media_sys.c
//   (LicenseRef-Espressif-Modified-MIT — Espressif products only)
//
// This file wires Espressif's media components together; it is C (not C++)
// because the upstream APIs are C and several use designated initializers.

#ifdef USE_ESP32

#include "media_sys.h"

#include <string.h>
#include "esp_log.h"

#include "esp_capture.h"
#include "esp_capture_defaults.h"
#include "av_render.h"
#include "av_render_default.h"
#include "esp_video_init.h"
#include "esp_video_enc_default.h"
#include "esp_video_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"

// Board helpers provided by the `codec_board` component.
#include "codec_board.h"
#include "codec_init.h"

static const char *TAG = "webrtc.media";

typedef struct {
  esp_capture_video_src_if_t *vid_src;
  esp_capture_audio_src_if_t *aud_src;
  esp_capture_handle_t capture_handle;
} capture_system_t;

typedef struct {
  av_render_audio_render_handle_t audio_render;
  av_render_video_render_handle_t video_render;
  av_render_handle_t player;
} player_system_t;

static capture_system_t s_capture;
static player_system_t s_player;
static bool s_built;

static int build_capture_system(void) {
  // Camera: V4L2 over esp_video (ESP32-P4 CSI/MIPI path).
  esp_video_init_config_t cam_config = {0};
  get_video_init_cfg(&cam_config);
  if (esp_video_init(&cam_config) != 0) {
    ESP_LOGE(TAG, "esp_video_init failed");
    return -1;
  }

  esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
      .dev_name = "/dev/video0",
      .buf_count = 2,
  };
  s_capture.vid_src = esp_capture_new_video_v4l2_src(&v4l2_cfg);

  esp_capture_audio_dev_src_cfg_t aud_cfg = {
      .record_handle = get_record_handle(),
  };
  s_capture.aud_src = esp_capture_new_audio_dev_src(&aud_cfg);

  esp_capture_cfg_t cfg = {
      .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
      .audio_src = s_capture.aud_src,
      .video_src = s_capture.vid_src,
  };
  if (esp_capture_open(&cfg, &s_capture.capture_handle) != 0) {
    ESP_LOGE(TAG, "esp_capture_open failed");
    return -1;
  }
  return 0;
}

static int build_player_system(void) {
  i2s_render_cfg_t i2s_cfg = {
      .fixed_clock = true,
      .play_handle = get_playback_handle(),
  };
  s_player.audio_render = av_render_alloc_i2s_render(&i2s_cfg);

  lcd_render_cfg_t lcd_cfg = {
      .lcd_handle = board_get_lcd_handle(),
  };
  s_player.video_render = av_render_alloc_lcd_render(&lcd_cfg);

  av_render_cfg_t render_cfg = {
      .audio_render = s_player.audio_render,
      .video_render = s_player.video_render,
      .audio_raw_fifo_size = 4096,
      .audio_render_fifo_size = 6 * 1024,
      .video_raw_fifo_size = 500 * 1024,
      .allow_drop_data = false,
  };
  s_player.player = av_render_open(&render_cfg);
  if (s_player.player == NULL) {
    ESP_LOGE(TAG, "av_render_open failed");
    return -1;
  }
  return 0;
}

int webrtc_media_sys_buildup(const char *board_name) {
  if (s_built) {
    return 0;
  }
  // Select the board pinmap / codec config and bring up I2C + codecs.
  set_codec_board_type(board_name);
  codec_init_cfg_t codec_cfg = {0};
  init_codec(&codec_cfg);

  // Register the default audio/video encoders + decoders (H.264, MJPEG,
  // OPUS, G.711, ...). Needed before capture/render can negotiate codecs.
  esp_video_enc_register_default();
  esp_audio_enc_register_default();
  esp_video_dec_register_default();
  esp_audio_dec_register_default();

  if (build_capture_system() != 0) {
    return -1;
  }
  if (build_player_system() != 0) {
    return -1;
  }
  s_built = true;
  return 0;
}

int webrtc_media_sys_get_provider(esp_webrtc_media_provider_t *provider) {
  if (!s_built || provider == NULL) {
    return -1;
  }
  provider->capture = s_capture.capture_handle;
  provider->player = s_player.player;
  return 0;
}

void webrtc_media_sys_teardown(void) {
  if (!s_built) {
    return;
  }
  if (s_player.player) {
    av_render_close(s_player.player);
    s_player.player = NULL;
  }
  if (s_capture.capture_handle) {
    esp_capture_close(s_capture.capture_handle);
    s_capture.capture_handle = NULL;
  }
  s_built = false;
}

#endif  // USE_ESP32
