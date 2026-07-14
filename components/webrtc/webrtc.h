#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include <atomic>
#include <string>
#include <vector>

#ifdef USE_ESP32

namespace esphome {
namespace webrtc {

// Mirror the Python enums (see __init__.py). Values match esp_peer's codecs.
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

// Matches esp_peer_media_dir_t (SEND_ONLY=1<<0, RECV_ONLY=1<<1).
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

  // Runtime control (automation actions).
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

 protected:
  bool open_peer_();
  static void task_fn_(void *arg);  // runs esp_peer main_loop

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
  std::vector<IceServer> ice_servers_;

  // Opaque esp_peer handles/pointers (kept void* to avoid leaking C headers).
  void *peer_{nullptr};       // esp_peer_handle_t
  const void *ops_{nullptr};  // const esp_peer_ops_t *
  void *ice_cfgs_{nullptr};   // heap esp_peer_ice_server_cfg_t[]; outlives open()
  void *task_{nullptr};       // TaskHandle_t
  volatile bool run_{false};
  bool started_{false};
  std::atomic<bool> connected_{false};
  // Bitmask of esp_peer states seen on the peer task; drained in loop().
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
