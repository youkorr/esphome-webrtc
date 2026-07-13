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

// Mirrors the Python config enums (see __init__.py).
enum Signaling {
  SIGNALING_APPRTC = 0,
  SIGNALING_WHIP,
  SIGNALING_JANUS,
};

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

// Bitmask, matches esp_peer_media_dir_t (SEND_ONLY=1<<0, RECV_ONLY=1<<1).
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
  // Network and camera/codec must be up first.
  float get_setup_priority() const override { return setup_priority::LATE; }

  // --- config setters (called from generated code) ---
  void set_signaling(Signaling s) { this->signaling_ = s; }
  void set_room_id(const std::string &room) { this->room_id_ = room; }
  void set_signal_url(const std::string &url) { this->signal_url_ = url; }
  void set_board(const std::string &board) { this->board_ = board; }
  void set_video_codec(VideoCodec c) { this->video_codec_ = c; }
  void set_audio_codec(AudioCodec c) { this->audio_codec_ = c; }
  void set_video_direction(MediaDir d) { this->video_dir_ = d; }
  void set_audio_direction(MediaDir d) { this->audio_dir_ = d; }
  void set_video_bitrate(uint32_t b) { this->video_bitrate_ = b; }
  void set_audio_bitrate(uint32_t b) { this->audio_bitrate_ = b; }
  void set_video_width(uint16_t w) { this->video_width_ = w; }
  void set_video_height(uint16_t h) { this->video_height_ = h; }
  void set_video_fps(uint8_t f) { this->video_fps_ = f; }
  void set_enable_data_channel(bool e) { this->enable_data_channel_ = e; }
  void set_auto_start(bool a) { this->auto_start_ = a; }
  void add_ice_server(const std::string &url, const std::string &user, const std::string &pass) {
    this->ice_servers_.push_back(IceServer{url, user, pass});
  }

  // --- runtime control ---
  // Open (if needed) and begin connecting / join the room.
  void start();
  // Stop the current call.
  void stop();
  // Send a message over the data channel.
  void send_data(const std::string &data);
  bool is_connected() const { return this->connected_; }

  // --- automation triggers ---
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

  // Called from the (C) esp_webrtc event handler trampoline.
  void on_event_(int event_type);

 protected:
  bool open_();
  std::string build_signal_url_() const;

  Signaling signaling_{SIGNALING_APPRTC};
  std::string room_id_;
  std::string signal_url_;
  std::string board_{"ESP32_P4_DEV_V14"};
  VideoCodec video_codec_{VIDEO_CODEC_H264};
  AudioCodec audio_codec_{AUDIO_CODEC_OPUS};
  MediaDir video_dir_{MEDIA_DIR_SEND_RECV};
  MediaDir audio_dir_{MEDIA_DIR_SEND_RECV};
  uint32_t video_bitrate_{0};
  uint32_t audio_bitrate_{0};
  uint16_t video_width_{1024};
  uint16_t video_height_{600};
  uint8_t video_fps_{10};
  bool enable_data_channel_{true};
  bool auto_start_{false};
  std::vector<IceServer> ice_servers_;

  // Opaque esp_webrtc handle (void * upstream). Kept as void* to avoid
  // leaking the C headers into this header.
  void *rtc_{nullptr};
  // Heap array of esp_peer_ice_server_cfg_t handed to esp_webrtc_open(). It
  // (and the IceServer strings it points into) must outlive open_(), so it is
  // owned by the component, not built on the stack. Opaque here for the same
  // reason as rtc_.
  void *ice_cfgs_{nullptr};
  // Built signaling URL. Must outlive open_() because esp_webrtc keeps the
  // pointer we pass in cfg.signaling_cfg.signal_url.
  std::string signal_url_full_;
  bool started_{false};
  bool connected_{false};
  // auto_start is deferred out of setup() until the network is actually up;
  // this latches the one-shot attempt so loop() doesn't retry every tick.
  bool auto_start_attempted_{false};
  uint32_t last_query_{0};
  // Bitmask of pending esp_webrtc events (1 << event_type). Written from the
  // esp_webrtc task, drained in loop() so triggers run on the ESPHome
  // main-loop thread. A bitmask (not a single slot) so back-to-back events
  // like PAIRED then CONNECTED cannot overwrite each other.
  std::atomic<uint32_t> pending_events_{0};

  CallbackManager<void()> on_connected_;
  CallbackManager<void()> on_disconnected_;
  CallbackManager<void()> on_paired_;
  CallbackManager<void()> on_connect_failed_;
};

}  // namespace webrtc
}  // namespace esphome

// Triggers/actions referenced by the generated code live here. Included at the
// bottom (after WebRTCComponent is fully defined) to avoid a circular include.
#include "automation.h"

#endif  // USE_ESP32
