#include "webrtc.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/esp_cam_sensor/esp_cam_sensor_camera.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"

extern "C" {
#include "esp_peer.h"
#include "esp_peer_default.h"
}

#ifdef USE_ESP_WEBRTC_VIDEO
#include "driver/ppa.h"
extern "C" {
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
}
#endif

namespace esphome {
namespace webrtc {

static const char *const TAG = "webrtc";

// G.711 telephony is always 8 kHz mono; a 20 ms frame is 160 samples.
static constexpr uint32_t G711_RATE = 8000;
static constexpr int G711_FRAME_SAMPLES = 160;  // 20 ms @ 8 kHz

// ---------------------------------------------------------------------------
// G.711 A-law / u-law (ITU-T reference, Sun Microsystems public-domain impl).
// esp_peer transports ENCODED audio, so we (de)compress PCM16 <-> G.711 here.
// ---------------------------------------------------------------------------
#define G711_SIGN_BIT 0x80
#define G711_QUANT_MASK 0x0f
#define G711_SEG_SHIFT 4
#define G711_SEG_MASK 0x70

static const int16_t seg_aend[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
static const int16_t seg_uend[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};

static int g711_search(int val, const int16_t *table, int size) {
  for (int i = 0; i < size; i++) {
    if (val <= table[i])
      return i;
  }
  return size;
}

static uint8_t linear_to_alaw(int16_t pcm_val) {
  int mask, seg;
  uint8_t aval;
  pcm_val = pcm_val >> 3;  // 16-bit -> 13-bit
  if (pcm_val >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    pcm_val = -pcm_val - 1;
    if (pcm_val < 0)
      pcm_val = 0;
  }
  seg = g711_search(pcm_val, seg_aend, 8);
  if (seg >= 8)
    return (uint8_t) (0x7F ^ mask);
  aval = seg << G711_SEG_SHIFT;
  if (seg < 2)
    aval |= (pcm_val >> 1) & G711_QUANT_MASK;
  else
    aval |= (pcm_val >> seg) & G711_QUANT_MASK;
  return aval ^ mask;
}

static int16_t alaw_to_linear(uint8_t a_val) {
  int t, seg;
  a_val ^= 0x55;
  t = (a_val & G711_QUANT_MASK) << 4;
  seg = (a_val & G711_SEG_MASK) >> G711_SEG_SHIFT;
  switch (seg) {
    case 0:
      t += 8;
      break;
    case 1:
      t += 0x108;
      break;
    default:
      t += 0x108;
      t <<= seg - 1;
  }
  return (a_val & G711_SIGN_BIT) ? t : -t;
}

#define G711_BIAS 0x84
#define G711_CLIP 8159

static uint8_t linear_to_ulaw(int16_t pcm_val) {
  int mask, seg;
  uint8_t uval;
  pcm_val = pcm_val >> 2;  // 16-bit -> 14-bit
  if (pcm_val < 0) {
    pcm_val = -pcm_val;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }
  if (pcm_val > G711_CLIP)
    pcm_val = G711_CLIP;
  pcm_val += (G711_BIAS >> 2);
  seg = g711_search(pcm_val, seg_uend, 8);
  if (seg >= 8)
    return (uint8_t) (0x7F ^ mask);
  uval = (seg << 4) | ((pcm_val >> (seg + 1)) & 0xF);
  return uval ^ mask;
}

static int16_t ulaw_to_linear(uint8_t u_val) {
  int t;
  u_val = ~u_val;
  t = ((u_val & G711_QUANT_MASK) << 3) + G711_BIAS;
  t <<= (u_val & G711_SEG_MASK) >> G711_SEG_SHIFT;
  return (u_val & G711_SIGN_BIT) ? (G711_BIAS - t) : (t - G711_BIAS);
}

// ---- esp_peer video/audio codec mapping ----

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

// Local SDP / ICE candidate produced by esp_peer -> send via signaling.
static int peer_on_msg(esp_peer_msg_t *msg, void *ctx) {
  if (msg != nullptr && msg->data != nullptr && msg->size > 0) {
    static_cast<WebRTCComponent *>(ctx)->send_local_signal_(static_cast<int>(msg->type), msg->data,
                                                            msg->size);
  }
  return 0;
}

static int peer_on_video_info(esp_peer_video_stream_info_t *info, void *ctx) { return 0; }
static int peer_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx) { return 0; }

// Incoming ENCODED frames. Slice 1: audio -> G.711 decode -> fdaudio speaker.
// Video (Slice 3) -> edge264 decode -> LVGL canvas, still a stub here.
static int peer_on_video_data(esp_peer_video_frame_t *frame, void *ctx) { return 0; }
static int peer_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx) {
  if (frame != nullptr && frame->data != nullptr && frame->size > 0) {
    static_cast<WebRTCComponent *>(ctx)->on_audio_frame_(frame->data, frame->size);
  }
  return 0;
}

void WebRTCComponent::on_peer_state_(int state) {
  if (state >= 0 && state < 32) {
    this->pending_states_.fetch_or(1u << state, std::memory_order_relaxed);
  }
}

void WebRTCComponent::send_local_signal_(int msg_type, const uint8_t *data, int size) {
  std::string s(reinterpret_cast<const char *>(data), size);
  if (msg_type == ESP_PEER_MSG_TYPE_SDP) {
    this->signaling_.send_sdp(s);
  } else if (msg_type == ESP_PEER_MSG_TYPE_CANDIDATE) {
    this->signaling_.send_candidate(s);
  }
}

// Peer task context: decode incoming G.711 to PCM16 @ 8 kHz and push to the
// ESPHome speaker (speaker->play() is a thread-safe ring enqueue).
void WebRTCComponent::on_audio_frame_(const uint8_t *data, int size) {
  if (this->spk_ == nullptr || size <= 0)
    return;
  std::vector<int16_t> pcm(size);
  if (this->audio_codec_ == AUDIO_CODEC_G711U) {
    for (int i = 0; i < size; i++)
      pcm[i] = ulaw_to_linear(data[i]);
  } else {  // default A-law
    for (int i = 0; i < size; i++)
      pcm[i] = alaw_to_linear(data[i]);
  }
  this->spk_->play(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * sizeof(int16_t));
  // Throttled RX telemetry: is the peer's audio actually arriving?
  if ((this->audio_rx_count_++ % 100) == 0) {
    int32_t peak = 0;
    for (int i = 0; i < size; i++) {
      int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (a > peak)
        peak = a;
    }
    ESP_LOGI(TAG, "audio RX: %u frames (last %d bytes, peak=%d)",
             (unsigned) this->audio_rx_count_, size, (int) peak);
  }
}

void WebRTCComponent::feed_remote_sdp_(const std::string &sdp) {
  if (this->peer_ == nullptr) {
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  esp_peer_msg_t m = {};
  m.type = ESP_PEER_MSG_TYPE_SDP;
  m.data = reinterpret_cast<uint8_t *>(const_cast<char *>(sdp.data()));
  m.size = static_cast<int>(sdp.size());
  ops->send_msg(static_cast<esp_peer_handle_t>(this->peer_), &m);
}

void WebRTCComponent::feed_remote_candidate_(const std::string &candidate) {
  if (this->peer_ == nullptr) {
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  esp_peer_msg_t m = {};
  m.type = ESP_PEER_MSG_TYPE_CANDIDATE;
  m.data = reinterpret_cast<uint8_t *>(const_cast<char *>(candidate.data()));
  m.size = static_cast<int>(candidate.size());
  ops->send_msg(static_cast<esp_peer_handle_t>(this->peer_), &m);
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

  esp_peer_default_cfg_t default_cfg = {};

  esp_peer_cfg_t cfg = {};
  cfg.server_lists = static_cast<esp_peer_ice_server_cfg_t *>(this->ice_cfgs_);
  cfg.server_num = static_cast<uint8_t>(this->ice_servers_.size());
  // Role must match the AppRTC initiator flag: the offerer is CONTROLLING, the
  // answerer is CONTROLLED.
  cfg.role = this->controlled_ ? ESP_PEER_ROLE_CONTROLLED : ESP_PEER_ROLE_CONTROLLING;
  cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
  cfg.audio_dir = static_cast<esp_peer_media_dir_t>(this->audio_dir_);
  cfg.video_dir = static_cast<esp_peer_media_dir_t>(this->video_dir_);
  cfg.audio_info.codec = to_peer_audio(this->audio_codec_);
  // G.711 is fixed 8 kHz mono; Opus is 16 kHz mono.
  cfg.audio_info.sample_rate = (this->audio_codec_ == AUDIO_CODEC_OPUS) ? 16000 : G711_RATE;
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
  self->peer_task_done_ = true;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Audio TX task: drain the mic ring, decimate mic_rate -> 8 kHz, G.711-encode a
// 20 ms frame, and hand it to esp_peer. Runs whenever a call is connected; idle
// (blocks on the empty ring) otherwise.
// ---------------------------------------------------------------------------
void WebRTCComponent::audio_tx_fn_(void *arg) {
  auto *self = static_cast<WebRTCComponent *>(arg);
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(self->ops_);
  auto handle = static_cast<esp_peer_handle_t>(self->peer_);
  auto rb = static_cast<RingbufHandle_t>(self->mic_rb_);

  const int decim = self->audio_rate_ >= G711_RATE ? (int) (self->audio_rate_ / G711_RATE) : 1;
  const int need_src = G711_FRAME_SAMPLES * decim;  // mic samples per 20 ms frame
  std::vector<int16_t> src;
  src.reserve(need_src);
  std::vector<uint8_t> enc(G711_FRAME_SAMPLES);
  uint32_t pts = 0;

  while (self->audio_run_) {
    if (!self->connected_.load() || rb == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    // Collect one 20 ms worth of mic PCM (bytes).
    size_t need_bytes = (size_t) need_src * sizeof(int16_t);
    src.clear();
    size_t have_bytes = 0;
    while (have_bytes < need_bytes && self->audio_run_) {
      size_t got = 0;
      TickType_t wait = (have_bytes == 0) ? pdMS_TO_TICKS(40) : pdMS_TO_TICKS(5);
      void *item = xRingbufferReceiveUpTo(rb, &got, wait, need_bytes - have_bytes);
      if (item == nullptr)
        break;
      const int16_t *s16 = static_cast<const int16_t *>(item);
      size_t n = got / sizeof(int16_t);
      for (size_t i = 0; i < n; i++)
        src.push_back(s16[i]);
      have_bytes += got;
      vRingbufferReturnItem(rb, item);
    }
    if (src.empty())
      continue;  // no mic data yet

    // Decimate to 8 kHz and encode one G.711 byte per output sample.
    int out = 0;
    for (int i = 0; i + decim <= (int) src.size() && out < G711_FRAME_SAMPLES; i += decim) {
      int16_t s = src[i];
      enc[out++] = (self->audio_codec_ == AUDIO_CODEC_G711U) ? linear_to_ulaw(s)
                                                             : linear_to_alaw(s);
    }
    if (out == 0)
      continue;

    esp_peer_audio_frame_t frame = {};
    frame.pts = pts;
    frame.data = enc.data();
    frame.size = out;
    int tx_ret = 0;
    if (ops->send_audio != nullptr)
      tx_ret = ops->send_audio(handle, &frame);
    pts += (uint32_t) (out * 1000 / G711_RATE);  // ~20 ms
    // Throttled TX telemetry: are our mic frames actually going out?
    if ((self->audio_tx_count_++ % 100) == 0) {
      int16_t peak = 0;
      for (int i = 0; i < (int) src.size(); i++) {
        int16_t a = src[i] < 0 ? -src[i] : src[i];
        if (a > peak)
          peak = a;
      }
      ESP_LOGI(TAG, "audio TX: %u frames (%d bytes, mic peak=%d, ret=%d)",
               (unsigned) self->audio_tx_count_, out, (int) peak, tx_ret);
    }
  }
  self->audio_task_done_ = true;
  vTaskDelete(nullptr);
}

void WebRTCComponent::on_mic_data_(const std::vector<uint8_t> &data) {
  if (this->mic_rb_ == nullptr || data.empty())
    return;
  // Non-blocking: if the call pipeline is behind, drop rather than stall the mic
  // (which is shared with voice_assistant via the callback fan-out).
  xRingbufferSend(static_cast<RingbufHandle_t>(this->mic_rb_), data.data(), data.size(), 0);
}

#ifdef USE_ESP_WEBRTC_VIDEO
// ---------------------------------------------------------------------------
// Video: share the camera's RGB565 frames (LVGL keeps its own stream), PPA
// scale + convert to YUV420 at the negotiated size, H.264-encode on the P4 HW
// encoder, and send to the peer. Opened lazily once connected (needs the size).
// ---------------------------------------------------------------------------
bool WebRTCComponent::open_video_encoder_() {
  if (this->venc_ != nullptr)
    return true;
  this->enc_w_ = this->video_w_;
  this->enc_h_ = this->video_h_;

  ppa_client_config_t pcfg = {};
  pcfg.oper_type = PPA_OPERATION_SRM;
  if (ppa_register_client(&pcfg, reinterpret_cast<ppa_client_handle_t *>(&this->ppa_)) != ESP_OK) {
    ESP_LOGE(TAG, "PPA client register failed");
    return false;
  }

  // The P4 HW encoder rejects planar I420 (different layout) and this esp_h264
  // build has no RGB565 input. Its native format is O_UYY_E_VYY, which is
  // exactly what the P4 PPA emits as "YUV420" (PPA + H.264 HW are co-designed).
  // enc input = YUV420 (w*h*3/2); H.264 output fits in < raw (w*h cap).
  this->yuv_buf_size_ = (size_t) this->enc_w_ * this->enc_h_ * 3 / 2;
  this->h264_buf_size_ = (size_t) this->enc_w_ * this->enc_h_;
  this->yuv_buf_ = heap_caps_aligned_alloc(64, this->yuv_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  this->h264_buf_ = heap_caps_aligned_alloc(64, this->h264_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->yuv_buf_ == nullptr || this->h264_buf_ == nullptr) {
    ESP_LOGE(TAG, "video buffers alloc failed (yuv=%u h264=%u)", (unsigned) this->yuv_buf_size_,
             (unsigned) this->h264_buf_size_);
    return false;
  }

  esp_h264_enc_cfg_hw_t ecfg = {};
  // P4 HW encoder native input format (matches the PPA YUV420 output layout).
  ecfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
  ecfg.gop = this->video_fps_;
  ecfg.fps = this->video_fps_;
  ecfg.res.width = this->enc_w_;
  ecfg.res.height = this->enc_h_;
  ecfg.rc.bitrate = (uint32_t) this->enc_w_ * this->enc_h_ * this->video_fps_ / 20;
  ecfg.rc.qp_min = 25;
  ecfg.rc.qp_max = 40;
  esp_h264_enc_handle_t enc = nullptr;
  esp_h264_err_t herr = esp_h264_enc_hw_new(&ecfg, &enc);
  if (herr != ESP_H264_ERR_OK || enc == nullptr) {
    ESP_LOGE(TAG, "esp_h264_enc_hw_new failed: %d", (int) herr);
    return false;
  }
  if (esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "esp_h264_enc_open failed");
    esp_h264_enc_del(enc);
    return false;
  }
  this->venc_ = enc;
  ESP_LOGI(TAG, "H.264 encoder ready %ux%u @%ufps (bitrate %u)", this->enc_w_, this->enc_h_,
           this->video_fps_, (unsigned) ecfg.rc.bitrate);
  return true;
}

void WebRTCComponent::close_video_encoder_() {
  if (this->venc_ != nullptr) {
    esp_h264_enc_close(static_cast<esp_h264_enc_handle_t>(this->venc_));
    esp_h264_enc_del(static_cast<esp_h264_enc_handle_t>(this->venc_));
    this->venc_ = nullptr;
  }
  if (this->ppa_ != nullptr) {
    ppa_unregister_client(static_cast<ppa_client_handle_t>(this->ppa_));
    this->ppa_ = nullptr;
  }
  if (this->yuv_buf_ != nullptr) {
    heap_caps_free(this->yuv_buf_);
    this->yuv_buf_ = nullptr;
  }
  if (this->h264_buf_ != nullptr) {
    heap_caps_free(this->h264_buf_);
    this->h264_buf_ = nullptr;
  }
}

void WebRTCComponent::video_tx_fn_(void *arg) {
  auto *self = static_cast<WebRTCComponent *>(arg);
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(self->ops_);
  auto handle = static_cast<esp_peer_handle_t>(self->peer_);
  auto *cam = self->camera_;
  const uint32_t frame_ms = 1000 / (self->video_fps_ > 0 ? self->video_fps_ : 15);
  uint32_t pts = 0;

  // The camera is shared with LVGL; start_streaming() is idempotent.
  if (cam != nullptr && !cam->is_streaming())
    cam->start_streaming();

  while (self->video_run_) {
    if (!self->connected_.load() || cam == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (self->venc_ == nullptr && !self->open_video_encoder_()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    uint32_t t0 = millis();
    esp_cam_sensor::SimpleBufferElement *fb = nullptr;
    uint8_t *rgb = nullptr;
    int w = 0, h = 0;
    if (!cam->get_current_rgb_frame(&fb, &rgb, &w, &h) || rgb == nullptr || w <= 0 || h <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // PPA: RGB565 (w x h) -> YUV420 (enc_w x enc_h), scale + colour convert.
    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = rgb;
    srm.in.pic_w = w;
    srm.in.pic_h = h;
    srm.in.block_w = w;
    srm.in.block_h = h;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm.out.buffer = self->yuv_buf_;
    srm.out.buffer_size = self->yuv_buf_size_;
    srm.out.pic_w = self->enc_w_;
    srm.out.pic_h = self->enc_h_;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm.scale_x = (float) self->enc_w_ / (float) w;
    srm.scale_y = (float) self->enc_h_ / (float) h;
    srm.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t pe = ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(self->ppa_), &srm);
    cam->release_buffer(fb);  // done with the camera frame
    if (pe != ESP_OK) {
      if ((self->video_tx_count_ % 100) == 0)
        ESP_LOGW(TAG, "PPA convert failed: %s", esp_err_to_name(pe));
      vTaskDelay(pdMS_TO_TICKS(frame_ms));
      continue;
    }

    // Encode YUV420 -> H.264.
    esp_h264_enc_in_frame_t in = {};
    in.raw_data.buffer = static_cast<uint8_t *>(self->yuv_buf_);
    in.raw_data.len = self->yuv_buf_size_;
    in.pts = pts;
    esp_h264_enc_out_frame_t outf = {};
    outf.raw_data.buffer = static_cast<uint8_t *>(self->h264_buf_);
    outf.raw_data.len = self->h264_buf_size_;
    esp_h264_err_t er =
        esp_h264_enc_process(static_cast<esp_h264_enc_handle_t>(self->venc_), &in, &outf);
    if (er != ESP_H264_ERR_OK) {
      if ((self->video_tx_count_ % 100) == 0)
        ESP_LOGW(TAG, "h264 encode failed: %d", (int) er);
      vTaskDelay(pdMS_TO_TICKS(frame_ms));
      continue;
    }

    esp_peer_video_frame_t vf = {};
    vf.pts = pts;
    vf.data = static_cast<uint8_t *>(self->h264_buf_);
    vf.size = static_cast<int>(outf.length);
    int vret = 0;
    if (ops->send_video != nullptr)
      vret = ops->send_video(handle, &vf);
    pts += frame_ms;
    if ((self->video_tx_count_++ % 30) == 0)
      ESP_LOGI(TAG, "video TX: %u frames (%ux%u, %u bytes, type=%d, ret=%d)",
               (unsigned) self->video_tx_count_, self->enc_w_, self->enc_h_, (unsigned) outf.length,
               (int) outf.frame_type, vret);

    uint32_t dt = millis() - t0;
    if (dt < frame_ms)
      vTaskDelay(pdMS_TO_TICKS(frame_ms - dt));
  }
  self->close_video_encoder_();
  self->video_task_done_ = true;
  vTaskDelete(nullptr);
}
#endif  // USE_ESP_WEBRTC_VIDEO

void WebRTCComponent::start_audio_bridge_() {
  if (!this->audio_bridge_enabled_() || this->bridge_active_)
    return;
  ESP_LOGI(TAG, "audio bridge: starting mic+speaker (G.711 @ 8 kHz)");
  // Speaker plays decoded 8 kHz mono PCM16.
  this->spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, G711_RATE));
  this->spk_->start();
  if (!this->mic_->is_running()) {
    this->mic_->start();
    this->mic_started_ = true;
  }
  this->bridge_active_ = true;
}

void WebRTCComponent::stop_audio_bridge_() {
  if (!this->audio_bridge_enabled_() || !this->bridge_active_)
    return;
  ESP_LOGI(TAG, "audio bridge: stopping mic+speaker");
  this->spk_->stop();
  if (this->mic_started_) {
    this->mic_->stop();
    this->mic_started_ = false;
  }
  this->bridge_active_ = false;
}

void WebRTCComponent::start() {
  if (this->started_) {
    return;
  }
  // 1) Join the room first, so we know whether we offer or answer.
  std::string url = "https://webrtc.espressif.com/join/" + this->room_id_;
  ESP_LOGI(TAG, "Joining signaling room: %s", url.c_str());
  if (!this->signaling_.join(url)) {
    ESP_LOGE(TAG, "signaling join failed");
    return;
  }
  this->controlled_ = !this->signaling_.is_initiator();
  ESP_LOGI(TAG, "role: %s", this->controlled_ ? "answerer (controlled)" : "offerer (controlling)");

  // 2) Open the peer with the correct role.
  if (!this->open_peer_()) {
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  auto handle = static_cast<esp_peer_handle_t>(this->peer_);

  // 3) Wire signaling -> peer BEFORE connect() replays any queued offer.
  this->signaling_.on_remote_sdp = [this](const std::string &sdp) { this->feed_remote_sdp_(sdp); };
  this->signaling_.on_remote_candidate = [this](const std::string &c) {
    this->feed_remote_candidate_(c);
  };
  this->signaling_.on_bye = [this]() { ESP_LOGI(TAG, "peer sent bye"); };

  // 4) esp_peer main_loop task.
  this->run_ = true;
  this->peer_task_done_ = false;
  this->audio_tx_count_ = 0;
  this->audio_rx_count_ = 0;
  xTaskCreatePinnedToCore(task_fn_, "webrtc_peer", 8192, this, 5,
                          reinterpret_cast<TaskHandle_t *>(&this->task_), 0);

  // 4b) Audio bridge: mic ring + callback + G.711 TX task (only if configured).
  if (this->audio_bridge_enabled_()) {
    if (this->mic_rb_ == nullptr) {
      this->mic_rb_ = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_BYTEBUF);
      if (this->mic_rb_ == nullptr)
        ESP_LOGE(TAG, "mic ring buffer alloc failed");
    }
    if (!this->mic_subscribed_ && this->mic_rb_ != nullptr) {
      this->mic_->add_data_callback(
          [this](const std::vector<uint8_t> &d) { this->on_mic_data_(d); });
      this->mic_subscribed_ = true;
    }
    if (this->mic_rb_ != nullptr && this->audio_tx_task_ == nullptr) {
      this->audio_run_ = true;
      this->audio_task_done_ = false;
      xTaskCreatePinnedToCore(audio_tx_fn_, "webrtc_atx", 4096, this, 5,
                              reinterpret_cast<TaskHandle_t *>(&this->audio_tx_task_), 0);
    }
    ESP_LOGI(TAG, "audio bridge ready (mic rate %u Hz -> G.711 8 kHz)", (unsigned) this->audio_rate_);
  }

#ifdef USE_ESP_WEBRTC_VIDEO
  // 4c) Video bridge: camera RGB565 -> PPA YUV420 -> H.264 -> send_video.
  if (this->video_bridge_enabled_() && this->video_tx_task_ == nullptr) {
    this->video_run_ = true;
    this->video_task_done_ = false;
    this->video_tx_count_ = 0;
    xTaskCreatePinnedToCore(video_tx_fn_, "webrtc_vtx", 8192, this, 5,
                            reinterpret_cast<TaskHandle_t *>(&this->video_tx_task_), 1);
    ESP_LOGI(TAG, "video bridge ready (camera -> H.264 %ux%u @%ufps)", this->video_w_,
             this->video_h_, this->video_fps_);
  }
#endif

  // 5) Open the WebSocket and replay the queued offer (answerer: this feeds the
  // peer's offer, so esp_peer produces the answer via on_msg).
  this->signaling_.connect();

  // 6) Only the offerer creates the connection (generates the local offer).
  if (!this->controlled_) {
    ops->new_connection(handle);
  }
  this->started_ = true;
}

void WebRTCComponent::stop() {
  if (!this->started_ && this->peer_ == nullptr) {
    return;
  }
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  auto handle = static_cast<esp_peer_handle_t>(this->peer_);
  ESP_LOGI(TAG, "Stopping WebRTC");

  // 1) Stop feeding/rendering audio (also stops the mic pushing into the ring).
  this->connected_ = false;  // the audio TX task stops sending immediately
  this->stop_audio_bridge_();

  // 2) Ask both worker tasks to exit and WAIT for them: they dereference the
  // peer handle (main_loop / send_audio), so they MUST be gone before we close
  // it, or we get a use-after-free. Bounded wait (~600 ms) to avoid hanging.
  const bool had_audio = (this->audio_tx_task_ != nullptr);
  const bool had_peer = (this->task_ != nullptr);
  const bool had_video = (this->video_tx_task_ != nullptr);
  this->audio_run_ = false;
  this->video_run_ = false;
  this->run_ = false;
  for (int i = 0; i < 60; i++) {
    bool audio_ok = !had_audio || this->audio_task_done_;
    bool peer_ok = !had_peer || this->peer_task_done_;
    bool video_ok = !had_video || this->video_task_done_;
    if (audio_ok && peer_ok && video_ok)
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  this->audio_tx_task_ = nullptr;
  this->video_tx_task_ = nullptr;
  this->task_ = nullptr;

  // 3) Tear down signaling (destroys the WebSocket; join() rebuilds it).
  this->signaling_.stop();

  // 4) Now it is safe to disconnect + CLOSE the peer and free it. Nulling peer_
  // forces open_peer_() to build a fresh one on the next start() (otherwise it
  // early-returns the dead handle -> reconnect silently fails).
  if (ops != nullptr && handle != nullptr) {
    if (ops->disconnect != nullptr)
      ops->disconnect(handle);
    if (ops->close != nullptr)
      ops->close(handle);
  }
  this->peer_ = nullptr;

  this->started_ = false;
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
    this->start_audio_bridge_();  // bring up mic+speaker for the call
    this->on_connected_.call();
  }
  if (evs & (1u << ESP_PEER_STATE_CONNECT_FAILED)) {
    ESP_LOGW(TAG, "connect failed");
    this->connected_ = false;
    this->stop_audio_bridge_();
    this->on_connect_failed_.call();
  }
  if (evs & (1u << ESP_PEER_STATE_DISCONNECTED)) {
    ESP_LOGI(TAG, "disconnected");
    this->connected_ = false;
    this->stop_audio_bridge_();
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
  if (this->audio_bridge_enabled_())
    ESP_LOGCONFIG(TAG, "  Audio bridge: fdaudio mic+speaker, G.711 (rate %u Hz)",
                  (unsigned) this->audio_rate_);
  if (this->video_bridge_enabled_())
    ESP_LOGCONFIG(TAG, "  Video bridge: camera -> H.264 (%ux%u @%ufps)", this->video_w_,
                  this->video_h_, this->video_fps_);
  ESP_LOGCONFIG(TAG, "  Signaling: AppRTC (webrtc.espressif.com)");
}

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
