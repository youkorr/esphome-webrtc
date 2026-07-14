#include "webrtc.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "esp_peer.h"
#include "esp_peer_default.h"
}

namespace esphome {
namespace webrtc {

static const char *const TAG = "webrtc";

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

// ---- esp_peer callbacks (run on the peer task) ----

static int peer_on_state(esp_peer_state_t state, void *ctx) {
  static_cast<WebRTCComponent *>(ctx)->on_peer_state_(static_cast<int>(state));
  return 0;
}

// Outgoing signaling message (local SDP / ICE candidate). Phase 2 will forward
// this to the AppRTC signaling server; for now we just log that it fired.
static int peer_on_msg(esp_peer_msg_t *msg, void *ctx) {
  if (msg != nullptr) {
    ESP_LOGD(TAG, "[signaling TODO] local msg type=%d size=%d", static_cast<int>(msg->type),
             msg->size);
  }
  return 0;
}

static int peer_on_video_info(esp_peer_video_stream_info_t *info, void *ctx) { return 0; }
static int peer_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx) { return 0; }

// Incoming ENCODED frames from the peer. Phase 3: video -> edge264 decode ->
// LVGL canvas; audio -> decode -> fdaudio speaker.
static int peer_on_video_data(esp_peer_video_frame_t *frame, void *ctx) { return 0; }
static int peer_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx) { return 0; }

void WebRTCComponent::on_peer_state_(int state) {
  if (state >= 0 && state < 32) {
    this->pending_states_.fetch_or(1u << state, std::memory_order_relaxed);
  }
}

void WebRTCComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up WebRTC (esp_peer, room=%s)...", this->room_id_.c_str());
}

bool WebRTCComponent::open_peer_() {
  if (this->peer_ != nullptr) {
    return true;
  }
  this->ops_ = reinterpret_cast<const void *>(esp_peer_get_default_impl());
  if (this->ops_ == nullptr) {
    ESP_LOGE(TAG, "no default esp_peer impl (out of memory?)");
    return false;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);

  // ICE server array must outlive open(): own it on the heap, pointing into
  // the persistent ice_servers_ strings.
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

  esp_peer_default_cfg_t default_cfg = {};  // all-zero => library defaults

  esp_peer_cfg_t cfg = {};
  cfg.server_lists = static_cast<esp_peer_ice_server_cfg_t *>(this->ice_cfgs_);
  cfg.server_num = static_cast<uint8_t>(this->ice_servers_.size());
  cfg.role = ESP_PEER_ROLE_CONTROLLING;
  cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
  cfg.audio_dir = static_cast<esp_peer_media_dir_t>(this->audio_dir_);
  cfg.video_dir = static_cast<esp_peer_media_dir_t>(this->video_dir_);
  cfg.audio_info.codec = to_peer_audio(this->audio_codec_);
  cfg.audio_info.sample_rate = (this->audio_codec_ == AUDIO_CODEC_OPUS) ? 16000 : 8000;
  cfg.audio_info.channel = 1;
  cfg.video_info.codec = to_peer_video(this->video_codec_);
  cfg.video_info.width = this->video_w_;
  cfg.video_info.height = this->video_h_;
  cfg.video_info.fps = this->video_fps_;
  cfg.enable_data_channel = this->enable_data_channel_;
  cfg.no_auto_reconnect = true;
  cfg.extra_cfg = &default_cfg;
  cfg.extra_size = sizeof(default_cfg);
  cfg.ctx = this;
  cfg.on_state = peer_on_state;
  cfg.on_msg = peer_on_msg;
  cfg.on_video_info = peer_on_video_info;
  cfg.on_audio_info = peer_on_audio_info;
  cfg.on_video_data = peer_on_video_data;
  cfg.on_audio_data = peer_on_audio_data;

  esp_peer_handle_t handle = nullptr;
  if (ops->open(&cfg, &handle) != ESP_PEER_ERR_NONE || handle == nullptr) {
    ESP_LOGE(TAG, "esp_peer open failed");
    return false;
  }
  this->peer_ = handle;
  return true;
}

void WebRTCComponent::task_fn_(void *arg) {
  auto *self = static_cast<WebRTCComponent *>(arg);
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(self->ops_);
  auto handle = static_cast<esp_peer_handle_t>(self->peer_);
  while (self->run_) {
    if (ops->main_loop != nullptr) {
      ops->main_loop(handle);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  vTaskDelete(nullptr);
}

void WebRTCComponent::start() {
  if (!this->open_peer_()) {
    return;
  }
  if (this->started_) {
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  auto handle = static_cast<esp_peer_handle_t>(this->peer_);

  this->run_ = true;
  xTaskCreatePinnedToCore(task_fn_, "webrtc_peer", 8192, this, 5,
                          reinterpret_cast<TaskHandle_t *>(&this->task_), 0);

  // Start ICE gathering / connection. Without signaling (Phase 2) the local
  // SDP produced here has nowhere to go, so it will not connect yet -- this is
  // the Phase 1 build/boot proof.
  ESP_LOGI(TAG, "Starting WebRTC (signaling not implemented yet -- Phase 2)");
  ops->new_connection(handle);
  this->started_ = true;
}

void WebRTCComponent::stop() {
  if (!this->started_) {
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  auto handle = static_cast<esp_peer_handle_t>(this->peer_);
  ESP_LOGI(TAG, "Stopping WebRTC");
  if (ops->disconnect != nullptr) {
    ops->disconnect(handle);
  }
  this->run_ = false;
  this->started_ = false;
  this->connected_ = false;
}

void WebRTCComponent::send_data(const std::string &data) {
  if (this->peer_ == nullptr || !this->enable_data_channel_) {
    ESP_LOGW(TAG, "send_data ignored (no session / data channel disabled)");
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  esp_peer_data_frame_t frame = {};
  frame.type = ESP_PEER_DATA_CHANNEL_STRING;
  frame.data = reinterpret_cast<uint8_t *>(const_cast<char *>(data.data()));
  frame.size = static_cast<int>(data.size());
  ops->send_data(static_cast<esp_peer_handle_t>(this->peer_), &frame);
}

void WebRTCComponent::loop() {
  if (this->auto_start_ && !this->auto_start_attempted_ && network::is_connected()) {
    this->auto_start_attempted_ = true;
    ESP_LOGI(TAG, "Network up; auto-starting WebRTC");
    this->start();
  }

  uint32_t evs = this->pending_states_.exchange(0u, std::memory_order_acquire);
  if (evs == 0) {
    return;
  }
  if (evs & (1u << ESP_PEER_STATE_PAIRED)) {
    ESP_LOGD(TAG, "paired");
    this->on_paired_.call();
  }
  if (evs & (1u << ESP_PEER_STATE_CONNECTED)) {
    ESP_LOGI(TAG, "connected");
    this->connected_ = true;
    this->on_connected_.call();
  }
  if (evs & (1u << ESP_PEER_STATE_CONNECT_FAILED)) {
    ESP_LOGW(TAG, "connect failed");
    this->connected_ = false;
    this->on_connect_failed_.call();
  }
  if (evs & (1u << ESP_PEER_STATE_DISCONNECTED)) {
    ESP_LOGI(TAG, "disconnected");
    this->connected_ = false;
    this->on_disconnected_.call();
  }
}

void WebRTCComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "WebRTC (esp_peer):");
  ESP_LOGCONFIG(TAG, "  Room: %s", this->room_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Video: %ux%u @%ufps", this->video_w_, this->video_h_, this->video_fps_);
  ESP_LOGCONFIG(TAG, "  Data channel: %s", YESNO(this->enable_data_channel_));
  ESP_LOGCONFIG(TAG, "  Auto start: %s", YESNO(this->auto_start_));
  ESP_LOGCONFIG(TAG, "  ICE servers: %u", (unsigned) this->ice_servers_.size());
  ESP_LOGCONFIG(TAG, "  Signaling: NOT IMPLEMENTED (Phase 2)");
}

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
