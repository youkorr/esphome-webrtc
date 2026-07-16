#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include "signaling.h"

#include <atomic>
#include <string>
#include <vector>

#ifdef USE_ESP32

namespace esphome {

// Forward-declared so this header stays free of the mic/speaker headers (they
// are only needed in webrtc.cpp for the fdaudio audio bridge).
namespace microphone {
class Microphone;
}
namespace speaker {
class Speaker;
}

namespace webrtc {

enum VideoCodec {
  VIDEO_CODEC_NONE = 0,
  VIDEO_CODEC_H264,
  VIDEO_CODEC_MJPEG,
};

enum AudioCodec {
  AUDIO_CODEC_NONE = 0,
  AUDIO_CODEC_G711A,
  AUDIO_CODEC_G711U,
  AUDIO_CODEC_OPUS,
};

enum MediaDir {
  MEDIA_DIR_NONE = 0,
  MEDIA_DIR_SEND_ONLY = 1 << 0,
  MEDIA_DIR_RECV_ONLY = 1 << 1,
  MEDIA_DIR_SEND_RECV = (1 << 0) | (1 << 1),
};

struct IceServer {
  std::string url;
  std::string username;
  std::string password;
};

class WebRTCComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_room_id(const std::string &r) { this->room_id_ = r; }
  void set_video_codec(VideoCodec c) { this->video_codec_ = c; }
  void set_audio_codec(AudioCodec c) { this->audio_codec_ = c; }
  void set_video_direction(MediaDir d) { this->video_dir_ = d; }
  void set_audio_direction(MediaDir d) { this->audio_dir_ = d; }
  void set_video_resolution(uint16_t w, uint16_t h, uint8_t fps) {
    this->video_w_ = w;
    this->video_h_ = h;
    this->video_fps_ = fps;
  }
  void set_enable_data_channel(bool e) { this->enable_data_channel_ = e; }
  void set_auto_start(bool a) { this->auto_start_ = a; }
  void add_ice_server(const std::string &url, const std::string &user, const std::string &pass) {
    this->ice_servers_.push_back(IceServer{url, user, pass});
  }

  // --- fdaudio audio bridge (share fdaudio's mic/speaker via the ESPHome
  // microphone + speaker platforms, so webrtc lives alongside voice_assistant).
  // When both are set the component runs "bridged": the near-end mic PCM is
  // G.711-encoded and sent to the peer, and the peer's incoming G.711 is decoded
  // to the speaker. No codec ownership -> no conflict with fdaudio/lvgl.
  void set_microphone(microphone::Microphone *m) { this->mic_ = m; }
  void set_speaker(speaker::Speaker *s) { this->spk_ = s; }
  void set_audio_sample_rate(uint32_t r) { this->audio_rate_ = r; }

  void start();
  void stop();
  void send_data(const std::string &data);
  bool is_connected() const { return this->connected_.load(); }

  void add_on_connected_callback(std::function<void()> &&cb) {
    this->on_connected_.add(std::move(cb));
  }
  void add_on_disconnected_callback(std::function<void()> &&cb) {
    this->on_disconnected_.add(std::move(cb));
  }
  void add_on_paired_callback(std::function<void()> &&cb) { this->on_paired_.add(std::move(cb)); }
  void add_on_connect_failed_callback(std::function<void()> &&cb) {
    this->on_connect_failed_.add(std::move(cb));
  }

  // Called from the esp_peer state callback (peer task).
  void on_peer_state_(int state);
  // Local SDP/ICE from esp_peer -> forward to the signaling server.
  void send_local_signal_(int msg_type, const uint8_t *data, int size);
  // Incoming ENCODED audio frame from esp_peer (peer task) -> decode -> speaker.
  void on_audio_frame_(const uint8_t *data, int size);

 protected:
  bool open_peer_();
  static void task_fn_(void *arg);       // runs esp_peer main_loop
  static void audio_tx_fn_(void *arg);   // mic ring -> G.711 -> esp_peer send_audio
  // Remote SDP/ICE from signaling -> feed into esp_peer.
  void feed_remote_sdp_(const std::string &sdp);
  void feed_remote_candidate_(const std::string &candidate);

  // --- audio bridge helpers ---
  bool audio_bridge_enabled_() const { return this->mic_ != nullptr && this->spk_ != nullptr; }
  void start_audio_bridge_();  // (re)start mic+speaker for a call (on connect)
  void stop_audio_bridge_();   // stop mic+speaker (on disconnect)
  void on_mic_data_(const std::vector<uint8_t> &data);  // mic cb -> ring buffer

  std::string room_id_{"esphome_room"};
  VideoCodec video_codec_{VIDEO_CODEC_H264};
  AudioCodec audio_codec_{AUDIO_CODEC_OPUS};
  MediaDir video_dir_{MEDIA_DIR_SEND_RECV};
  MediaDir audio_dir_{MEDIA_DIR_SEND_RECV};
  uint16_t video_w_{640};
  uint16_t video_h_{480};
  uint8_t video_fps_{15};
  bool enable_data_channel_{true};
  bool auto_start_{false};
  bool auto_start_attempted_{false};
  // true when this side answers (peer already in room); false when it offers.
  bool controlled_{false};
  std::vector<IceServer> ice_servers_;

  ApprtcSignaling signaling_;

  // --- fdaudio audio bridge ---
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spk_{nullptr};
  uint32_t audio_rate_{16000};      // PCM rate of the ESPHome mic/speaker (mono 16-bit)
  void *mic_rb_{nullptr};           // RingbufHandle_t: mic PCM, cb -> audio_tx task
  bool mic_subscribed_{false};
  bool mic_started_{false};         // did we start the mic (so we stop it on disconnect)
  void *audio_tx_task_{nullptr};    // TaskHandle_t
  volatile bool audio_run_{false};
  uint32_t audio_tx_count_{0};      // frames sent to peer (throttled telemetry)
  uint32_t audio_rx_count_{0};      // frames received from peer (throttled telemetry)
  // Latched so start_audio_bridge_()/stop_audio_bridge_() run once per edge.
  bool bridge_active_{false};

  void *peer_{nullptr};       // esp_peer_handle_t
  const void *ops_{nullptr};  // const esp_peer_ops_t *
  void *ice_cfgs_{nullptr};   // heap esp_peer_ice_server_cfg_t[]; outlives open()
  void *task_{nullptr};       // TaskHandle_t
  volatile bool run_{false};
  bool started_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> pending_states_{0};

  CallbackManager<void()> on_connected_;
  CallbackManager<void()> on_disconnected_;
  CallbackManager<void()> on_paired_;
  CallbackManager<void()> on_connect_failed_;
};

}  // namespace webrtc
}  // namespace esphome

#include "automation.h"

#endif  // USE_ESP32
