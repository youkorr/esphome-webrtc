#include "webrtc.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

extern "C" {
#include "esp_webrtc.h"
#include "esp_webrtc_defaults.h"
#include "esp_peer.h"
#include "media_sys.h"
}

namespace esphome {
namespace webrtc {

static const char *const TAG = "webrtc";

// esp_webrtc event types (from esp_webrtc.h: esp_webrtc_event_type_t).
static constexpr int EV_CONNECTING = 1;
static constexpr int EV_PAIRED = 2;
static constexpr int EV_CONNECTED = 3;
static constexpr int EV_CONNECT_FAILED = 4;
static constexpr int EV_DISCONNECTED = 5;

static esp_peer_video_codec_t to_peer_video(VideoCodec c) {
  switch (c) {
    case VIDEO_CODEC_H264:
      return ESP_PEER_VIDEO_CODEC_H264;
    case VIDEO_CODEC_MJPEG:
      return ESP_PEER_VIDEO_CODEC_MJPEG;
    default:
      return ESP_PEER_VIDEO_CODEC_NONE;
  }
}

static esp_peer_audio_codec_t to_peer_audio(AudioCodec c) {
  switch (c) {
    case AUDIO_CODEC_G711A:
      return ESP_PEER_AUDIO_CODEC_G711A;
    case AUDIO_CODEC_G711U:
      return ESP_PEER_AUDIO_CODEC_G711U;
    case AUDIO_CODEC_OPUS:
      return ESP_PEER_AUDIO_CODEC_OPUS;
    default:
      return ESP_PEER_AUDIO_CODEC_NONE;
  }
}

// Trampoline from the esp_webrtc task. Keep it tiny: just stash the event and
// let loop() fire the user triggers on the main thread.
static int webrtc_event_cb(esp_webrtc_event_t *event, void *ctx) {
  auto *self = static_cast<WebRTCComponent *>(ctx);
  if (self != nullptr && event != nullptr) {
    self->on_event_(static_cast<int>(event->type));
  }
  return 0;
}

void WebRTCComponent::on_event_(int event_type) {
  // Set the bit for this event. fetch_or is atomic, so several events raised
  // between two loop() drains all survive (unlike a single overwritten slot).
  if (event_type > 0 && event_type < 32) {
    this->pending_events_.fetch_or(1u << event_type, std::memory_order_relaxed);
  }
}

std::string WebRTCComponent::build_signal_url_() const {
  switch (this->signaling_) {
    case SIGNALING_APPRTC:
      // Both peers join the same room on Espressif's public AppRTC bridge.
      return "https://webrtc.espressif.com/join/" + this->room_id_;
    case SIGNALING_WHIP:
    case SIGNALING_JANUS:
    default:
      return this->signal_url_;
  }
}

void WebRTCComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up WebRTC (board=%s)...", this->board_.c_str());
  if (webrtc_media_sys_buildup(this->board_.c_str()) != 0) {
    ESP_LOGE(TAG, "media system build-up failed");
    this->mark_failed();
    return;
  }
  // NOTE: auto_start is intentionally NOT handled here. At LATE setup the
  // network (ESP-Hosted Wi-Fi / Ethernet) is not associated yet, so joining
  // the signaling room would fail. loop() starts the call once the network is
  // actually connected.
}

bool WebRTCComponent::open_() {
  if (this->rtc_ != nullptr) {
    return true;
  }

  // Persist the built URL: esp_webrtc keeps the pointer we hand it, so it must
  // outlive this function (was a stack std::string before -> dangling).
  this->signal_url_full_ = this->build_signal_url_();

  esp_webrtc_cfg_t cfg = {};
  // Only the AppRTC signaling impl is linked in (see __init__.py: we pull just
  // components/esp_webrtc/impl/apprtc_signal). whip/janus are not selectable
  // from YAML, so esp_signaling_get_whip_impl()/get_janus_impl() are not
  // referenced (they would fail to link). To restore them, add their impl
  // components and re-add the switch cases.
  cfg.signaling_impl = esp_signaling_get_apprtc_impl();
  cfg.signaling_cfg.signal_url = const_cast<char *>(this->signal_url_full_.c_str());
  cfg.peer_impl = esp_peer_get_default_impl();

  // ICE servers (optional; AppRTC also provides its own). The cfg array and
  // the strings it references must outlive open_(), so the array is owned by
  // the component (this->ice_cfgs_) and the strings live in ice_servers_.
  if (!this->ice_servers_.empty() && this->ice_cfgs_ == nullptr) {
    auto *arr = new esp_peer_ice_server_cfg_t[this->ice_servers_.size()]{};
    for (size_t i = 0; i < this->ice_servers_.size(); i++) {
      auto &s = this->ice_servers_[i];
      arr[i].stun_url = const_cast<char *>(s.url.c_str());
      if (!s.username.empty())
        arr[i].user = const_cast<char *>(s.username.c_str());
      if (!s.password.empty())
        arr[i].psw = const_cast<char *>(s.password.c_str());
    }
    this->ice_cfgs_ = arr;
  }
  if (this->ice_cfgs_ != nullptr) {
    cfg.peer_cfg.server_lists = static_cast<esp_peer_ice_server_cfg_t *>(this->ice_cfgs_);
    cfg.peer_cfg.server_num = static_cast<uint8_t>(this->ice_servers_.size());
  }

  cfg.peer_cfg.audio_dir = static_cast<esp_peer_media_dir_t>(this->audio_dir_);
  cfg.peer_cfg.video_dir = static_cast<esp_peer_media_dir_t>(this->video_dir_);
  cfg.peer_cfg.enable_data_channel = this->enable_data_channel_;

  cfg.peer_cfg.audio_info.codec = to_peer_audio(this->audio_codec_);
  if (this->audio_codec_ == AUDIO_CODEC_OPUS) {
    cfg.peer_cfg.audio_info.sample_rate = 16000;
    cfg.peer_cfg.audio_info.channel = 1;
  } else {  // G.711
    cfg.peer_cfg.audio_info.sample_rate = 8000;
    cfg.peer_cfg.audio_info.channel = 1;
  }

  cfg.peer_cfg.video_info.codec = to_peer_video(this->video_codec_);
  cfg.peer_cfg.video_info.width = this->video_width_;
  cfg.peer_cfg.video_info.height = this->video_height_;
  cfg.peer_cfg.video_info.fps = this->video_fps_;

  esp_webrtc_handle_t handle = nullptr;
  if (esp_webrtc_open(&cfg, &handle) != ESP_PEER_ERR_NONE || handle == nullptr) {
    ESP_LOGE(TAG, "esp_webrtc_open failed");
    return false;
  }
  this->rtc_ = handle;

  esp_webrtc_media_provider_t provider = {};
  if (webrtc_media_sys_get_provider(&provider) == 0) {
    esp_webrtc_set_media_provider(handle, &provider);
  }
  esp_webrtc_set_event_handler(handle, webrtc_event_cb, this);

  if (this->video_bitrate_ > 0)
    esp_webrtc_set_video_bitrate(handle, this->video_bitrate_);
  if (this->audio_bitrate_ > 0)
    esp_webrtc_set_audio_bitrate(handle, this->audio_bitrate_);

  // Don't auto-create the peer connection until start() is called.
  esp_webrtc_enable_peer_connection(handle, false);
  return true;
}

void WebRTCComponent::start() {
  if (!this->open_()) {
    return;
  }
  if (this->started_) {
    return;
  }
  ESP_LOGI(TAG, "Starting WebRTC session...");
  if (esp_webrtc_start(static_cast<esp_webrtc_handle_t>(this->rtc_)) != ESP_PEER_ERR_NONE) {
    ESP_LOGE(TAG, "esp_webrtc_start failed");
    return;
  }
  this->started_ = true;
}

void WebRTCComponent::stop() {
  if (this->rtc_ == nullptr || !this->started_) {
    return;
  }
  ESP_LOGI(TAG, "Stopping WebRTC session...");
  esp_webrtc_stop(static_cast<esp_webrtc_handle_t>(this->rtc_));
  this->started_ = false;
  this->connected_ = false;
}

void WebRTCComponent::send_data(const std::string &data) {
  if (this->rtc_ == nullptr || !this->enable_data_channel_) {
    ESP_LOGW(TAG, "send_data ignored (no session / data channel disabled)");
    return;
  }
  esp_webrtc_send_custom_data(static_cast<esp_webrtc_handle_t>(this->rtc_),
                              ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
                              reinterpret_cast<uint8_t *>(const_cast<char *>(data.data())),
                              static_cast<int>(data.size()));
}

void WebRTCComponent::loop() {
  // Deferred auto-start: wait until the network is actually connected before
  // joining the signaling room (setup() runs before Wi-Fi association).
  if (this->auto_start_ && !this->auto_start_attempted_ && network::is_connected()) {
    this->auto_start_attempted_ = true;
    ESP_LOGI(TAG, "Network up; auto-starting WebRTC session");
    this->start();
  }

  // Drive the esp_webrtc state machine ~1 Hz.
  const uint32_t now = millis();
  if (this->started_ && this->rtc_ != nullptr && now - this->last_query_ > 1000) {
    this->last_query_ = now;
    esp_webrtc_query(static_cast<esp_webrtc_handle_t>(this->rtc_));
  }

  // Drain every event captured on the esp_webrtc task since the last loop().
  // Bits are processed in event-type (chronological) order so PAIRED fires
  // before CONNECTED even when both arrived in the same window.
  uint32_t evs = this->pending_events_.exchange(0u, std::memory_order_acquire);
  if (evs == 0) {
    return;
  }
  if (evs & (1u << EV_CONNECTING)) {
    ESP_LOGD(TAG, "connecting");
  }
  if (evs & (1u << EV_PAIRED)) {
    ESP_LOGD(TAG, "paired");
    this->on_paired_.call();
  }
  if (evs & (1u << EV_CONNECTED)) {
    ESP_LOGI(TAG, "connected");
    this->connected_ = true;
    this->on_connected_.call();
  }
  if (evs & (1u << EV_CONNECT_FAILED)) {
    ESP_LOGW(TAG, "connect failed");
    this->connected_ = false;
    this->on_connect_failed_.call();
  }
  if (evs & (1u << EV_DISCONNECTED)) {
    ESP_LOGI(TAG, "disconnected");
    this->connected_ = false;
    this->on_disconnected_.call();
  }
}

void WebRTCComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "WebRTC:");
  ESP_LOGCONFIG(TAG, "  Signaling: apprtc");
  ESP_LOGCONFIG(TAG, "  Signal URL: %s", this->build_signal_url_().c_str());
  ESP_LOGCONFIG(TAG, "  Board: %s", this->board_.c_str());
  ESP_LOGCONFIG(TAG, "  Video: %ux%u @%ufps", this->video_width_, this->video_height_,
                this->video_fps_);
  ESP_LOGCONFIG(TAG, "  Data channel: %s", YESNO(this->enable_data_channel_));
  ESP_LOGCONFIG(TAG, "  Auto start: %s", YESNO(this->auto_start_));
  ESP_LOGCONFIG(TAG, "  ICE servers: %u", (unsigned) this->ice_servers_.size());
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed (media system).");
  }
}

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
