#pragma once

// Thin media bring-up layer for ESP-WebRTC, adapted from the upstream
// `solutions/videocall_demo/main/media_sys.c`.
//
// It builds the capture system (camera + microphone) and the player
// (LCD + speaker) and exposes them as an esp_webrtc media provider.
//
// The concrete board pinmap / codec handles come from the `codec_board`
// component, selected by the board name string (e.g. "ESP32_P4_DEV_V14").

#ifdef USE_ESP32

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_webrtc.h"

// Initialise the board (codec_board), register codecs, and build the
// capture + player systems. Safe to call once. Returns 0 on success.
int webrtc_media_sys_buildup(const char *board_name);

// Fill `provider` with the capture + player handles created above.
int webrtc_media_sys_get_provider(esp_webrtc_media_provider_t *provider);

// Tear everything down (closes capture + player).
void webrtc_media_sys_teardown(void);

#ifdef __cplusplus
}
#endif

#endif  // USE_ESP32
