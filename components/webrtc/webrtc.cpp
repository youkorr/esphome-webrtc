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
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include <cstring>
#include <cstdlib>

extern "C" {
#include "esp_peer.h"
#include "esp_peer_default.h"
}

#ifdef USE_ESP_WEBRTC_OPUS
extern "C" {
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"
}
#endif

#ifdef USE_ESP_WEBRTC_VIDEO
#include "driver/ppa.h"
#include "esp_cache.h"
extern "C" {
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
}
// P4 hardware Motion-JPEG codec (encode + decode). RGB565 in / RGB565 out, so
// the MJPEG path needs no YUV conversion (unlike H.264).
#include "driver/jpeg_encode.h"
#include "driver/jpeg_decode.h"
#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif
// edge264 High-profile software H.264 decoder (from the h264_hp component in the
// ip-camera-viewer repo). Only present when that lib is in the build; the flag
// -DUSE_H264_HP_EDGE264 is a global build flag set by h264_hp.
#ifdef USE_H264_HP_EDGE264
#include "esphome/components/h264_hp/h264_hp_decoder.h"
#endif
#endif

namespace esphome {
namespace webrtc {

static const char *const TAG = "webrtc";

// G.711 telephony is always 8 kHz mono; a 20 ms frame is 160 samples.
static constexpr uint32_t G711_RATE = 8000;
static constexpr int G711_FRAME_SAMPLES = 160;  // 20 ms @ 8 kHz
// Opus here runs 16 kHz mono (wideband voice), 20 ms frames.
static constexpr uint32_t OPUS_RATE = 16000;

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

// Incoming ENCODED frames. Audio -> G.711 decode -> fdaudio speaker. Video
// (MJPEG) -> stash the JPEG here; the HW decode + LVGL draw happen in loop().
static int peer_on_video_data(esp_peer_video_frame_t *frame, void *ctx) {
#ifdef USE_ESP_WEBRTC_VIDEO
  if (frame != nullptr && frame->data != nullptr && frame->size > 0) {
    static_cast<WebRTCComponent *>(ctx)->on_video_frame_(frame->data, frame->size);
  }
#endif
  return 0;
}
static int peer_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx) {
  if (frame != nullptr && frame->data != nullptr && frame->size > 0) {
    static_cast<WebRTCComponent *>(ctx)->on_audio_frame_(frame->data, frame->size);
  }
  return 0;
}

// Video-over-data-channel framing magic (see dc_tx_ in webrtc.h for why).
static const uint8_t VIDEO_DC_MAGIC[4] = {'V', 'I', 'D', '0'};

// Maximum encoded JPEG frame size we will push into the SCTP data channel.
// The video goes over a RELIABLE ordered SCTP channel on the throughput-limited
// ESP-Hosted (C6) link. Once frames get big the link can't keep up: SCTP starts
// retransmitting (CRC errors), the receive buffer backs up ("No buffer for TSN")
// and the stack faults. Measured ceiling with audio also flowing is ~11-13 KB
// per frame; 13 KB matches the proven-stable operating point. Frames above this
// cap are dropped (MJPEG is self-contained -> the next frame repairs the image)
// and a throttled warning tells the user to lower jpeg_quality/fps/resolution.
static const uint32_t MJPEG_DC_MAX_FRAME = 13000;

// Incoming data-channel messages: video frames (magic-prefixed) go to the video
// path; anything else is application data.
static int peer_on_data(esp_peer_data_frame_t *frame, void *ctx) {
  if (frame == nullptr || frame->data == nullptr || frame->size <= 0) {
    return 0;
  }
#ifdef USE_ESP_WEBRTC_VIDEO
  if (frame->size > 4 && memcmp(frame->data, VIDEO_DC_MAGIC, 4) == 0) {
    static_cast<WebRTCComponent *>(ctx)->on_video_frame_(frame->data + 4, frame->size - 4);
    return 0;
  }
#endif
  ESP_LOGD(TAG, "data channel RX: %d bytes", frame->size);
  return 0;
}

void WebRTCComponent::on_peer_state_(int state) {
  if (state >= 0 && state < 32) {
    this->pending_states_.fetch_or(1u << state, std::memory_order_relaxed);
  }
}

// Log an SDP line-by-line (a 2 KB blob would be truncated by the logger) so we
// can inspect the H.264 m-line/rtpmap/fmtp negotiation.
static void log_sdp_(const char *label, const std::string &sdp) {
  ESP_LOGI(TAG, "===== %s SDP (%u bytes) =====", label, (unsigned) sdp.size());
  size_t start = 0;
  while (start < sdp.size()) {
    size_t nl = sdp.find('\n', start);
    size_t end = (nl == std::string::npos) ? sdp.size() : nl;
    std::string line = sdp.substr(start, end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty())
      ESP_LOGI(TAG, "  %s", line.c_str());
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
}

void WebRTCComponent::send_local_signal_(int msg_type, const uint8_t *data, int size) {
  std::string s(reinterpret_cast<const char *>(data), size);
  if (msg_type == ESP_PEER_MSG_TYPE_SDP) {
    // esp_peer advertises H.264 Main profile (profile-level-id=4d001f). Browsers'
    // WebRTC H.264 is usually Constrained Baseline only, so for a sendrecv video
    // m-line they can't offer Main back and DECLINE the whole track (m=video 0 in
    // the answer -> no remote video). The P4 HW encoder's stream is baseline-
    // compatible, so rewrite the advertised profile to Constrained Baseline
    // (42e01f); the browser then accepts and decodes it. MJPEG carries no
    // profile-level-id, so only munge the H.264 offer.
    if (this->video_codec_ == VIDEO_CODEC_H264) {
      for (size_t pos = 0; (pos = s.find("4d001f", pos)) != std::string::npos; pos += 6) {
        s.replace(pos, 6, "42e01f");
      }
    }
    log_sdp_("LOCAL (our offer/answer)", s);
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
  // Runs on the webrtc_peer task. Drop frames until the main loop has fully
  // brought up the speaker (bridge_active_). Otherwise the first frame calls
  // spk_->play() here while start_audio_bridge_() is mid spk_->start() on the
  // main loop; fdaudio then inits its full-duplex engine from two tasks at once
  // -> corrupted state -> "Instruction access fault" crash right after connect.
  if (!this->bridge_active_.load(std::memory_order_acquire))
    return;
#ifdef USE_ESP_WEBRTC_OPUS
  if (this->audio_codec_ == AUDIO_CODEC_OPUS && this->opus_dec_ != nullptr &&
      this->opus_pcm_out_ != nullptr) {
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(data);
    raw.len = (uint32_t) size;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = static_cast<uint8_t *>(this->opus_pcm_out_);
    out.len = (uint32_t) this->opus_pcm_out_cap_;
    int dr = esp_audio_dec_process(static_cast<esp_audio_dec_handle_t>(this->opus_dec_), &raw, &out);
    if (dr == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
      this->spk_->play(static_cast<const uint8_t *>(this->opus_pcm_out_), out.decoded_size);
      if ((this->audio_rx_count_++ % 100) == 0)
        ESP_LOGI(TAG, "audio RX: %u frames (Opus %d bytes -> %u PCM)",
                 (unsigned) this->audio_rx_count_, size, (unsigned) out.decoded_size);
    } else if ((this->audio_rx_count_ % 100) == 0) {
      ESP_LOGW(TAG, "Opus decode failed: %d", dr);
    }
    return;
  }
#endif
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

// Peer-task context: copy the newest incoming JPEG frame into a PSRAM stash.
// The heavy HW decode + LVGL canvas update run in loop() (main task) so they
// never touch LVGL from this task. MJPEG frames are independent so newest wins
// (drop under contention); H.264 is inter-frame coded so its access units go
// through an ORDERED bounded queue drained by the edge264 decode task.
#ifdef USE_ESP_WEBRTC_VIDEO
// One encoded H.264 access unit queued from the peer task to the decode task.
struct WebRtcAu {
  uint8_t *data;
  int size;
  bool idr;
};

// True if the Annex-B buffer contains an IDR slice (NAL type 5). Used to re-sync
// after a queue overflow: we drop everything until the next IDR.
static bool annexb_has_idr_(const uint8_t *d, int n) {
  for (int i = 0; i + 3 < n; i++) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      if ((d[i + 3] & 0x1F) == 5)
        return true;
      i += 2;
    }
  }
  return false;
}
#endif

void WebRTCComponent::on_video_frame_(const uint8_t *data, int size) {
#ifdef USE_ESP_WEBRTC_VIDEO
  if (this->remote_canvas_ == nullptr)
    return;

  if (this->video_codec_ == VIDEO_CODEC_MJPEG) {
    // MJPEG: independent frames -> newest-wins stash consumed by loop().
    if (this->jpeg_rx_mtx_ == nullptr)
      return;
    auto mtx = static_cast<SemaphoreHandle_t>(this->jpeg_rx_mtx_);
    if (xSemaphoreTake(mtx, 0) != pdTRUE)
      return;  // loop() holds it; skip this frame
    if ((size_t) size > this->jpeg_rx_cap_) {
      size_t ncap = (size_t) size + 4096;
      void *nb = heap_caps_realloc(this->jpeg_rx_buf_, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (nb == nullptr) {
        xSemaphoreGive(mtx);
        return;
      }
      this->jpeg_rx_buf_ = nb;
      this->jpeg_rx_cap_ = ncap;
    }
    memcpy(this->jpeg_rx_buf_, data, size);
    this->jpeg_rx_size_ = size;
    this->jpeg_rx_ready_ = true;
    xSemaphoreGive(mtx);
    return;
  }

  // H.264: enqueue a heap copy of the access unit for the edge264 decode task.
  auto q = static_cast<QueueHandle_t>(this->video_q_);
  if (q == nullptr)
    return;
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr)
    return;
  memcpy(buf, data, size);
  WebRtcAu au{buf, size, annexb_has_idr_(data, size)};
  if (xQueueSend(q, &au, 0) != pdTRUE) {
    // Queue full: the decoder can't keep up. Drop the OLDEST to bound memory and
    // ask the decoder to re-sync at the next IDR (dropping a P-frame otherwise
    // corrupts everything up to it). Keeps latency bounded to ~1 GOP.
    WebRtcAu old;
    if (xQueueReceive(q, &old, 0) == pdTRUE)
      heap_caps_free(old.data);
    this->need_idr_ = true;
    if (xQueueSend(q, &au, 0) != pdTRUE)
      heap_caps_free(buf);
  }
#endif
}

void WebRTCComponent::feed_remote_sdp_(const std::string &sdp) {
  if (this->peer_ == nullptr) {
    return;
  }
  log_sdp_("REMOTE (peer's offer/answer)", sdp);
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
#ifdef USE_ESP_WEBRTC_VIDEO
  // MJPEG receive: the peer task stashes JPEG frames guarded by this mutex; the
  // decode/canvas draw happen in loop(). Created up front so the first frame
  // that lands before render_remote_frame_() runs is handled safely.
  if (this->video_codec_ == VIDEO_CODEC_MJPEG && this->jpeg_rx_mtx_ == nullptr) {
    this->jpeg_rx_mtx_ = xSemaphoreCreateMutex();
  }
  // Serialises the HW JPEG encoder (webrtc_vtx) and decoder (main loop): they
  // share one peripheral, so concurrent use corrupts it and crashes.
  if (this->video_codec_ == VIDEO_CODEC_MJPEG && this->jpeg_mutex_ == nullptr) {
    this->jpeg_mutex_ = xSemaphoreCreateMutex();
  }
#ifdef USE_H264_HP_EDGE264
  // H.264 receive: ordered access-unit queue from the peer task to the edge264
  // decode task. Created up front so early frames are queued safely.
  if (this->video_codec_ == VIDEO_CODEC_H264 && this->video_q_ == nullptr) {
    this->video_q_ = xQueueCreate(30, sizeof(WebRtcAu));
  }
#endif
#endif
}

bool WebRTCComponent::open_peer_() {
  if (this->peer_ != nullptr) {
    return true;
  }
  // Seed libc rand() with the hardware RNG. esp_peer derives its ICE ufrag/pwd
  // (and session id) from rand(); if it's never seeded, TWO identical firmwares
  // generate the SAME ice-ufrag -> ICE username collision -> P4<->P4 never
  // connects (works with a browser only because the browser has its own creds).
  srand(esp_random());
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
  cfg.on_data = peer_on_data;

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

#ifdef USE_ESP_WEBRTC_OPUS
// Create the Opus encoder + decoder (esp_audio_codec) once. 16 kHz mono, 20 ms,
// VOIP mode. The encoder input frame is a fixed byte count (opus_in_bytes_).
bool WebRTCComponent::open_opus_() {
  if (this->opus_enc_ != nullptr && this->opus_dec_ != nullptr)
    return true;
  // Registration is idempotent across calls (ALREADY_EXIST is fine).
  esp_opus_enc_register();
  esp_opus_dec_register();

  esp_opus_enc_config_t ecfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
  ecfg.sample_rate = OPUS_RATE;
  ecfg.channel = 1;
  ecfg.bits_per_sample = 16;
  ecfg.bitrate = 24000;  // wideband voice
  ecfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
  ecfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
  ecfg.complexity = 5;
  esp_audio_enc_config_t enc_cfg = {};
  enc_cfg.type = ESP_AUDIO_TYPE_OPUS;
  enc_cfg.cfg = &ecfg;
  enc_cfg.cfg_sz = sizeof(ecfg);
  esp_audio_enc_handle_t enc = nullptr;
  if (esp_audio_enc_open(&enc_cfg, &enc) != ESP_AUDIO_ERR_OK || enc == nullptr) {
    ESP_LOGE(TAG, "Opus encoder open failed");
    return false;
  }
  int in_size = 0, out_size = 0;
  esp_audio_enc_get_frame_size(enc, &in_size, &out_size);
  if (in_size <= 0)
    in_size = (int) (OPUS_RATE / 50) * 2;  // 20 ms @ 16 kHz mono, 16-bit
  this->opus_enc_ = enc;
  this->opus_in_bytes_ = in_size;
  this->opus_enc_out_cap_ = (out_size > 0) ? (size_t) out_size : 1500;
  this->opus_enc_out_ = heap_caps_malloc(this->opus_enc_out_cap_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  esp_opus_dec_cfg_t dcfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
  dcfg.sample_rate = OPUS_RATE;
  dcfg.channel = 1;
  dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;  // auto-detect per packet
  dcfg.self_delimited = false;
  esp_audio_dec_cfg_t dec_cfg = {};
  dec_cfg.type = ESP_AUDIO_TYPE_OPUS;
  dec_cfg.cfg = &dcfg;
  dec_cfg.cfg_sz = sizeof(dcfg);
  esp_audio_dec_handle_t dec = nullptr;
  if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
    ESP_LOGE(TAG, "Opus decoder open failed");
    return false;
  }
  this->opus_dec_ = dec;
  this->opus_pcm_out_cap_ = 4096;  // up to a 120 ms frame of 16 kHz mono PCM
  this->opus_pcm_out_ = heap_caps_malloc(this->opus_pcm_out_cap_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->opus_enc_out_ == nullptr || this->opus_pcm_out_ == nullptr) {
    ESP_LOGE(TAG, "Opus buffers alloc failed");
    return false;
  }
  ESP_LOGI(TAG, "Opus ready (16 kHz mono, 20 ms, in=%d out cap=%u)", this->opus_in_bytes_,
           (unsigned) this->opus_enc_out_cap_);
  return true;
}
#endif  // USE_ESP_WEBRTC_OPUS

// ---------------------------------------------------------------------------
// Audio TX task: drain the mic ring, and either G.711-encode (decimate to 8 kHz)
// or Opus-encode (16 kHz) a 20 ms frame, then hand it to esp_peer. Runs whenever
// a call is connected; idle (blocks on the empty ring) otherwise.
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
  std::vector<uint8_t> obuf;  // Opus: raw 16 kHz mono PCM accumulation
  uint32_t pts = 0;
  // Paced sending: the mic delivers ~96 ms bursts, so the ring often holds
  // several 20 ms frames at once. Sending them back-to-back produces bursty RTP
  // that the far end plays with clicks/pops ("le son tape"). Space sends ~20 ms
  // apart so the RTP stream is smooth.
  uint32_t pace = millis();

  while (self->audio_run_) {
    if (!self->connected_.load() || rb == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(20));
      pace = millis();
      continue;
    }
    // Hold this frame until its 20 ms slot; resync if we ever fall far behind.
    int32_t pace_wait = (int32_t) (pace - millis());
    if (pace_wait > 0 && pace_wait < 60)
      vTaskDelay(pdMS_TO_TICKS(pace_wait));
    pace += 20;
    if ((int32_t) (millis() - pace) > 100)
      pace = millis();
#ifdef USE_ESP_WEBRTC_OPUS
    if (self->audio_codec_ == AUDIO_CODEC_OPUS && self->opus_enc_ != nullptr) {
      // Assumes the mic PCM is already 16 kHz mono (fdaudio sample_rate: 16000).
      const size_t need = (size_t) self->opus_in_bytes_;
      obuf.clear();
      while (obuf.size() < need && self->audio_run_) {
        size_t got = 0;
        TickType_t wait = obuf.empty() ? pdMS_TO_TICKS(40) : pdMS_TO_TICKS(5);
        void *item = xRingbufferReceiveUpTo(rb, &got, wait, need - obuf.size());
        if (item == nullptr)
          break;
        const uint8_t *b = static_cast<const uint8_t *>(item);
        obuf.insert(obuf.end(), b, b + got);
        vRingbufferReturnItem(rb, item);
      }
      if (obuf.size() < need)
        continue;  // wait for a full 20 ms frame
      esp_audio_enc_in_frame_t inf = {};
      inf.buffer = obuf.data();
      inf.len = (uint32_t) need;
      esp_audio_enc_out_frame_t of = {};
      of.buffer = static_cast<uint8_t *>(self->opus_enc_out_);
      of.len = (uint32_t) self->opus_enc_out_cap_;
      int er = esp_audio_enc_process(static_cast<esp_audio_enc_handle_t>(self->opus_enc_), &inf, &of);
      if (er == ESP_AUDIO_ERR_OK && of.encoded_bytes > 0) {
        esp_peer_audio_frame_t frame = {};
        frame.pts = pts;
        frame.data = static_cast<uint8_t *>(self->opus_enc_out_);
        frame.size = (int) of.encoded_bytes;
        int tx_ret = 0;
        if (ops->send_audio != nullptr)
          tx_ret = ops->send_audio(handle, &frame);
        pts += 20;
        if ((self->audio_tx_count_++ % 100) == 0) {
          int16_t peak = 0;
          const int16_t *s16 = reinterpret_cast<const int16_t *>(obuf.data());
          for (size_t i = 0; i < need / 2; i++) {
            int16_t a = s16[i] < 0 ? -s16[i] : s16[i];
            if (a > peak)
              peak = a;
          }
          ESP_LOGI(TAG, "audio TX: %u frames (Opus %u bytes, mic peak=%d, ret=%d)",
                   (unsigned) self->audio_tx_count_, (unsigned) of.encoded_bytes, (int) peak,
                   tx_ret);
        }
      } else if ((self->audio_tx_count_ % 100) == 0) {
        ESP_LOGW(TAG, "Opus encode failed: %d", er);
      }
      continue;
    }
#endif
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

    // Decimate to 8 kHz and encode one G.711 byte per output sample. Average the
    // `decim` input samples instead of dropping (a cheap anti-alias box filter):
    // plain sample-dropping folds the 4-8 kHz band back into the audible range
    // and makes speech sound harsh/"saturated". Averaging attenuates it.
    int out = 0;
    for (int i = 0; i + decim <= (int) src.size() && out < G711_FRAME_SAMPLES; i += decim) {
      int32_t acc = 0;
      for (int k = 0; k < decim; k++)
        acc += src[i + k];
      int16_t s = (int16_t) (acc / decim);
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
// scale to the negotiated size, and encode on a P4 HW codec before sending to
// the peer. Two codecs:
//   * H.264  (browser interop): PPA -> YUV420 -> HW H.264 encoder.
//   * MJPEG  (ESP<->ESP): PPA -> RGB565 -> HW JPEG encoder. The P4 has no HW
//     H.264 DECODER, so a P4 that must SHOW the remote peer uses MJPEG both ways.
// Opened lazily once connected (needs the negotiated size).
// ---------------------------------------------------------------------------
bool WebRTCComponent::open_video_encoder_() {
  const bool mjpeg = (this->video_codec_ == VIDEO_CODEC_MJPEG);
  if ((mjpeg && this->jenc_ != nullptr) || (!mjpeg && this->venc_ != nullptr))
    return true;
  this->enc_w_ = this->video_w_;
  this->enc_h_ = this->video_h_;

  ppa_client_config_t pcfg = {};
  pcfg.oper_type = PPA_OPERATION_SRM;
  if (this->ppa_ == nullptr &&
      ppa_register_client(&pcfg, reinterpret_cast<ppa_client_handle_t *>(&this->ppa_)) != ESP_OK) {
    ESP_LOGE(TAG, "PPA client register failed");
    return false;
  }

  if (mjpeg) {
    // MJPEG: PPA emits RGB565 (scaled) into the HW JPEG encoder input, then the
    // encoder writes the JPEG bitstream.
    //  - INPUT (= PPA output): must be 64-byte cache-line aligned for the PPA,
    //    which jpeg_alloc_encoder_mem does NOT guarantee. Use heap_caps_aligned_
    //    alloc(64,...); a DMA-capable 64-aligned PSRAM buffer is a valid JPEG
    //    encoder input too.
    //  - OUTPUT: MUST come from jpeg_alloc_encoder_mem. Its 2D-DMA descriptors
    //    require that exact allocation; a plain heap_caps buffer here crashed
    //    inside jpeg_encoder_process ("Instruction access fault" on frame 1). The
    //    PPA never touches the output, so there is no alignment conflict here.
    this->yuv_buf_size_ = (size_t) this->enc_w_ * this->enc_h_ * 2;  // RGB565
    this->yuv_buf_ =
        heap_caps_aligned_alloc(64, this->yuv_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_encode_memory_alloc_cfg_t imcfg = {};
    imcfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    size_t got_in = 0;
    this->jpeg_in_ = jpeg_alloc_encoder_mem(this->yuv_buf_size_, &imcfg, &got_in);
    this->jpeg_in_size_ = got_in;
    jpeg_encode_memory_alloc_cfg_t omcfg = {};
    omcfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    size_t got_out = 0;
    this->h264_buf_ =
        jpeg_alloc_encoder_mem((size_t) this->enc_w_ * this->enc_h_ * 2, &omcfg, &got_out);
    this->h264_buf_size_ = got_out;
    if (this->yuv_buf_ == nullptr || this->jpeg_in_ == nullptr || this->h264_buf_ == nullptr) {
      ESP_LOGE(TAG, "JPEG enc buffers alloc failed (ppa=%u in=%u out=%u)",
               (unsigned) this->yuv_buf_size_, (unsigned) this->jpeg_in_size_,
               (unsigned) this->h264_buf_size_);
      return false;
    }
    jpeg_encode_engine_cfg_t jcfg = {};
    jcfg.intr_priority = 0;
    jcfg.timeout_ms = 1000 / (this->video_fps_ > 0 ? this->video_fps_ : 15) + 20;
    jpeg_encoder_handle_t je = nullptr;
    if (jpeg_new_encoder_engine(&jcfg, &je) != ESP_OK || je == nullptr) {
      ESP_LOGE(TAG, "jpeg_new_encoder_engine failed");
      return false;
    }
    this->jenc_ = je;
    ESP_LOGI(TAG, "MJPEG encoder ready %ux%u @%ufps", this->enc_w_, this->enc_h_, this->video_fps_);
    return true;
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
  // GOP = 1 s: esp_peer doesn't surface the browser's keyframe requests (PLI/FIR)
  // to us, so a long GOP means any lost packet freezes the far-end video until
  // the next IDR. A 1 s IDR interval lets it self-heal quickly ("video stops"
  // fix). Frames stay modest thanks to send_only + the moderate bitrate.
  uint32_t gop = (uint32_t) this->video_fps_;
  if (gop < 1)
    gop = 1;
  ecfg.gop = (uint8_t) (gop > 255 ? 255 : gop);
  ecfg.fps = this->video_fps_;
  ecfg.res.width = this->enc_w_;
  ecfg.res.height = this->enc_h_;
  // Balanced quality/bandwidth: send_only already removed the downlink
  // contention, so we no longer need to over-compress. qp_max=42 keeps quality
  // decent (qp_max=51 gave ~129-byte garbage frames); a moderate bitrate keeps
  // the ESP-Hosted C6 uplink happy.
  ecfg.rc.bitrate = (uint32_t) this->enc_w_ * this->enc_h_ * this->video_fps_ / 10;
  ecfg.rc.qp_min = 24;
  ecfg.rc.qp_max = 42;
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
  if (this->jenc_ != nullptr) {
    jpeg_del_encoder_engine(static_cast<jpeg_encoder_handle_t>(this->jenc_));
    this->jenc_ = nullptr;
  }
  if (this->ppa_ != nullptr) {
    ppa_unregister_client(static_cast<ppa_client_handle_t>(this->ppa_));
    this->ppa_ = nullptr;
  }
  if (this->yuv_buf_ != nullptr) {
    heap_caps_free(this->yuv_buf_);
    this->yuv_buf_ = nullptr;
  }
  if (this->jpeg_in_ != nullptr) {
    heap_caps_free(this->jpeg_in_);
    this->jpeg_in_ = nullptr;
  }
  if (this->dc_tx_ != nullptr) {
    heap_caps_free(this->dc_tx_);
    this->dc_tx_ = nullptr;
    this->dc_tx_cap_ = 0;
  }
  if (this->h264_buf_ != nullptr) {
    heap_caps_free(this->h264_buf_);
    this->h264_buf_ = nullptr;
  }
}

// Grow a JPEG-encoder DMA buffer to at least `need` bytes (mirror of face2face's
// ensure_enc_buf: the HW JPEG codec requires jpeg_alloc_encoder_mem buffers).
static bool ensure_jpeg_enc_buf_(void **buf, size_t *cap, size_t need, bool input) {
  if (*buf != nullptr && *cap >= need)
    return true;
  if (*buf != nullptr) {
    heap_caps_free(*buf);
    *buf = nullptr;
    *cap = 0;
  }
  jpeg_encode_memory_alloc_cfg_t cfg = {};
  cfg.buffer_direction = input ? JPEG_ENC_ALLOC_INPUT_BUFFER : JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t got = 0;
  *buf = jpeg_alloc_encoder_mem(need, &cfg, &got);
  *cap = (*buf != nullptr) ? (got != 0 ? got : need) : 0;
  return *buf != nullptr;
}

// MJPEG video TX on the MAIN LOOP. Drives the V4L2 capture, PPA-scales the
// camera frame to EXACTLY video_width x video_height (the user's choice, not the
// sensor's native size), HW-JPEG encodes and sends over the data channel. Runs
// on loop() (not a task) so it serialises naturally with the JPEG decode; the
// first-frame crashes were never here — they were ops->send_video on an inactive
// RTP m-line (proven by addr2line).
void WebRTCComponent::pump_mjpeg_tx_() {
  if (!this->connected_.load() || this->camera_ == nullptr || !(this->video_dir_ & MEDIA_DIR_SEND_ONLY))
    return;
  const uint32_t frame_ms = 1000 / (this->video_fps_ > 0 ? this->video_fps_ : 15);
  uint32_t now = millis();
  if (now - this->last_vtx_ms_ < frame_ms)
    return;
  this->last_vtx_ms_ = now;

  auto *cam = this->camera_;
  if (!cam->is_streaming())
    cam->start_streaming();

  // Target = EXACTLY what the user set in YAML, rounded down to the HW JPEG
  // codec's 16px granularity. Independent of the sensor's native resolution.
  const int ew = (int) this->video_w_ & ~15;
  const int eh = (int) this->video_h_ & ~15;
  if (ew < 16 || eh < 16)
    return;

  // Lazy one-time init: PPA client + fixed-size buffers + JPEG engine.
  if (this->jenc_ == nullptr) {
    ppa_client_config_t pcfg = {};
    pcfg.oper_type = PPA_OPERATION_SRM;
    if (this->ppa_ == nullptr &&
        ppa_register_client(&pcfg, reinterpret_cast<ppa_client_handle_t *>(&this->ppa_)) != ESP_OK) {
      ESP_LOGE(TAG, "PPA client register failed");
      return;
    }
    this->enc_w_ = ew;
    this->enc_h_ = eh;
    this->yuv_buf_size_ = (size_t) ew * eh * 2;  // RGB565
    this->yuv_buf_ =
        heap_caps_aligned_alloc(64, this->yuv_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (this->yuv_buf_ == nullptr ||
        !ensure_jpeg_enc_buf_(&this->jpeg_in_, &this->jpeg_in_size_, this->yuv_buf_size_, true) ||
        !ensure_jpeg_enc_buf_(&this->h264_buf_, &this->h264_buf_size_, this->yuv_buf_size_, false)) {
      ESP_LOGE(TAG, "MJPEG buffers alloc failed (%dx%d)", ew, eh);
      return;
    }
    jpeg_encode_engine_cfg_t jcfg = {};
    jcfg.timeout_ms = 70;
    jpeg_encoder_handle_t je = nullptr;
    if (jpeg_new_encoder_engine(&jcfg, &je) != ESP_OK || je == nullptr) {
      ESP_LOGE(TAG, "jpeg_new_encoder_engine failed");
      return;
    }
    this->jenc_ = je;
    if (ew != (int) this->video_w_ || eh != (int) this->video_h_)
      ESP_LOGW(TAG, "MJPEG size rounded to 16px: %ux%u -> %dx%d", this->video_w_, this->video_h_,
               ew, eh);
    ESP_LOGI(TAG, "MJPEG encoder ready (main-loop), sending exactly %dx%d", ew, eh);
  }

  // Sole consumer: dequeue a fresh frame (else current_buffer_index_ stays -1).
  if (this->drive_camera_ && !cam->capture_frame())
    return;
  esp_cam_sensor::SimpleBufferElement *fb = nullptr;
  uint8_t *rgb = nullptr;
  int w = 0, h = 0;
  if (!cam->get_current_rgb_frame(&fb, &rgb, &w, &h) || rgb == nullptr || w <= 0 || h <= 0)
    return;

  // PPA: scale the camera RGB565 (w x h) to EXACTLY ew x eh (the user's size),
  // whatever the sensor delivers.
  ppa_srm_oper_config_t srm = {};
  srm.in.buffer = rgb;
  srm.in.pic_w = w;
  srm.in.pic_h = h;
  srm.in.block_w = w;
  srm.in.block_h = h;
  srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.out.buffer = this->yuv_buf_;
  srm.out.buffer_size = this->yuv_buf_size_;
  srm.out.pic_w = ew;
  srm.out.pic_h = eh;
  srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  srm.scale_x = (float) ew / (float) w;
  srm.scale_y = (float) eh / (float) h;
  srm.mode = PPA_TRANS_MODE_BLOCKING;
  esp_err_t pe = ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(this->ppa_), &srm);
  cam->release_buffer(fb);  // PPA is blocking; camera buffer free to re-queue now
  if (pe != ESP_OK) {
    if (now - this->last_enc_warn_ms_ > 1000) {
      this->last_enc_warn_ms_ = now;
      ESP_LOGW(TAG, "PPA scale failed: %s", esp_err_to_name(pe));
    }
    return;
  }
  // PPA wrote via DMA -> make the CPU see fresh pixels, then copy into the HW
  // JPEG encoder's own input buffer (it requires jpeg_alloc_encoder_mem memory).
  esp_cache_msync(this->yuv_buf_, this->yuv_buf_size_,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  memcpy(this->jpeg_in_, this->yuv_buf_, this->yuv_buf_size_);
  const size_t need = this->yuv_buf_size_;

  jpeg_encode_cfg_t jc = {};
  jc.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  jc.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  jc.image_quality = this->jpeg_quality_;
  jc.width = ew;
  jc.height = eh;
  uint32_t enc_len = 0;
  // Encoder and decoder share one HW peripheral; both run on this task now, but
  // keep the mutex so a future off-loop user stays safe.
  if (this->jpeg_mutex_ != nullptr)
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->jpeg_mutex_), portMAX_DELAY);
  esp_err_t je = jpeg_encoder_process(static_cast<jpeg_encoder_handle_t>(this->jenc_), &jc,
                                      static_cast<uint8_t *>(this->jpeg_in_), need,
                                      static_cast<uint8_t *>(this->h264_buf_),
                                      this->h264_buf_size_, &enc_len);
  if (this->jpeg_mutex_ != nullptr)
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(this->jpeg_mutex_));
  if (je != ESP_OK || enc_len == 0) {
    if (now - this->last_enc_warn_ms_ > 1000) {
      this->last_enc_warn_ms_ = now;
      ESP_LOGW(TAG, "jpeg encode failed: %s", esp_err_to_name(je));
    }
    return;
  }

  // Send over the DATA CHANNEL, NOT ops->send_video. In P4<->P4 the video m-line
  // is negotiated a=inactive (no RTP video stream); send_video then runs esp_peer's
  // RTP encoder on an uninitialized stream and crashes inside write_rtp_packet /
  // rtp_encoder_encode_generic (proven via addr2line: every 'first frame' crash
  // since the beginning was THIS, not the capture/encode pipeline).
  if (!this->enable_data_channel_)
    return;
  // Safety valve: a single frame bigger than the SCTP send window overflows the
  // data channel ("SCTP: No buffer for TSN" flood -> Store access fault crash).
  // Drop it (MJPEG is self-contained, the next frame repairs the picture) and
  // hint the user to lower jpeg_quality/fps/resolution.
  if (enc_len > MJPEG_DC_MAX_FRAME) {
    if (now - this->last_enc_warn_ms_ > 1000) {
      this->last_enc_warn_ms_ = now;
      ESP_LOGW(TAG, "JPEG frame %u B > %u B cap; dropped (lower jpeg_quality/fps/resolution)",
               (unsigned) enc_len, (unsigned) MJPEG_DC_MAX_FRAME);
    }
    return;
  }
  size_t dc_need = (size_t) enc_len + 4;
  if (this->dc_tx_cap_ < dc_need) {
    heap_caps_free(this->dc_tx_);
    this->dc_tx_ = heap_caps_malloc(dc_need + 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    this->dc_tx_cap_ = (this->dc_tx_ != nullptr) ? dc_need + 4096 : 0;
    if (this->dc_tx_ == nullptr)
      return;
  }
  memcpy(this->dc_tx_, VIDEO_DC_MAGIC, 4);
  memcpy(static_cast<uint8_t *>(this->dc_tx_) + 4, this->h264_buf_, enc_len);
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(this->ops_);
  auto handle = static_cast<esp_peer_handle_t>(this->peer_);
  esp_peer_data_frame_t df = {};
  df.type = ESP_PEER_DATA_CHANNEL_DATA;
  df.data = static_cast<uint8_t *>(this->dc_tx_);
  df.size = static_cast<int>(dc_need);
  int vret = 0;
  if (ops != nullptr && ops->send_data != nullptr && handle != nullptr)
    vret = ops->send_data(handle, &df);
  if ((this->video_tx_count_++ % 30) == 0)
    ESP_LOGI(TAG, "video TX: %u frames (MJPEG %dx%d, %u bytes via DC, ret=%d)",
             (unsigned) this->video_tx_count_, ew, eh, (unsigned) enc_len, vret);
}

void WebRTCComponent::video_tx_fn_(void *arg) {
  auto *self = static_cast<WebRTCComponent *>(arg);
  const esp_peer_ops_t *ops = static_cast<const esp_peer_ops_t *>(self->ops_);
  auto handle = static_cast<esp_peer_handle_t>(self->peer_);
  auto *cam = self->camera_;
  const bool mjpeg = (self->video_codec_ == VIDEO_CODEC_MJPEG);
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
    const bool enc_ready = mjpeg ? (self->jenc_ != nullptr) : (self->venc_ != nullptr);
    if (!enc_ready && !self->open_video_encoder_()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    uint32_t t0 = millis();
    // Drive the V4L2 capture ourselves ONLY when we are the sole camera
    // consumer (drive_camera_). If lvgl_camera_display is present (self-view) it
    // already dequeues frames; a second DQBUF/QBUF on the same fd from here
    // corrupts the buffers. In that case we only READ the current buffer below.
    if (self->drive_camera_ && !cam->capture_frame()) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    esp_cam_sensor::SimpleBufferElement *fb = nullptr;
    uint8_t *rgb = nullptr;
    int w = 0, h = 0;
    if (!cam->get_current_rgb_frame(&fb, &rgb, &w, &h) || rgb == nullptr || w <= 0 || h <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const bool dbg1 = (self->video_tx_count_ == 0);  // trace the very first frame
    if (dbg1)
      ESP_LOGI(TAG, "1st frame: got %dx%d rgb=%p, yuv_buf=%p (%u), out=%p (%u), enc %ux%u",
               w, h, rgb, self->yuv_buf_, (unsigned) self->yuv_buf_size_, self->h264_buf_,
               (unsigned) self->h264_buf_size_, self->enc_w_, self->enc_h_);

    // PPA: RGB565 (w x h) -> scale to (enc_w x enc_h). For H.264 also convert to
    // YUV420 (the encoder's native layout); for MJPEG keep RGB565 (the HW JPEG
    // encoder takes RGB565 directly).
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
    srm.out.srm_cm = mjpeg ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_YUV420;
    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm.scale_x = (float) self->enc_w_ / (float) w;
    srm.scale_y = (float) self->enc_h_ / (float) h;
    srm.mode = PPA_TRANS_MODE_BLOCKING;
    if (dbg1)
      ESP_LOGI(TAG, "1st frame: calling PPA...");
    esp_err_t pe = ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(self->ppa_), &srm);
    cam->release_buffer(fb);  // done with the camera frame
    if (pe != ESP_OK) {
      if ((self->video_tx_count_ % 100) == 0)
        ESP_LOGW(TAG, "PPA convert failed: %s", esp_err_to_name(pe));
      vTaskDelay(pdMS_TO_TICKS(frame_ms));
      continue;
    }
    if (dbg1)
      ESP_LOGI(TAG, "1st frame: PPA ok, encoding...");

    uint32_t enc_len = 0;
    int frame_type = 0;
    if (mjpeg) {
      // The PPA wrote RGB565 into yuv_buf_ via DMA; make the CPU see the fresh
      // pixels, then copy them into the encoder's OWN input buffer (the HW JPEG
      // codec requires jpeg_alloc_encoder_mem buffers - a plain buffer crashed
      // inside jpeg_encoder_process).
      esp_cache_msync(self->yuv_buf_, self->yuv_buf_size_,
                      ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
      memcpy(self->jpeg_in_, self->yuv_buf_, self->yuv_buf_size_);
      // Encode RGB565 -> JPEG on the P4 HW JPEG codec.
      jpeg_encode_cfg_t jc = {};
      jc.width = self->enc_w_;
      jc.height = self->enc_h_;
      jc.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
      jc.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
      jc.image_quality = 60;  // balance quality vs the ESP-Hosted C6 uplink
      // Serialise with the main-loop decoder (shared HW JPEG peripheral).
      if (self->jpeg_mutex_ != nullptr)
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(self->jpeg_mutex_), portMAX_DELAY);
      esp_err_t je = jpeg_encoder_process(
          static_cast<jpeg_encoder_handle_t>(self->jenc_), &jc,
          static_cast<uint8_t *>(self->jpeg_in_), self->yuv_buf_size_,
          static_cast<uint8_t *>(self->h264_buf_), self->h264_buf_size_, &enc_len);
      if (self->jpeg_mutex_ != nullptr)
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->jpeg_mutex_));
      if (je != ESP_OK) {
        if ((self->video_tx_count_ % 100) == 0)
          ESP_LOGW(TAG, "jpeg encode failed: %s", esp_err_to_name(je));
        vTaskDelay(pdMS_TO_TICKS(frame_ms));
        continue;
      }
    } else {
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
      enc_len = outf.length;
      frame_type = (int) outf.frame_type;
    }

    esp_peer_video_frame_t vf = {};
    vf.pts = pts;
    vf.data = static_cast<uint8_t *>(self->h264_buf_);
    vf.size = static_cast<int>(enc_len);
    int vret = 0;
    if (ops->send_video != nullptr)
      vret = ops->send_video(handle, &vf);
    // ALSO ship the frame over the data channel ("VID0" framing). In P4<->P4 the
    // video m-line is negotiated a=inactive, so send_video reaches nothing (this
    // is why the far P4 never showed an image in H.264); the DC copy is what the
    // peer's on_data -> on_video_frame_ -> edge264 path actually consumes. A
    // browser peer simply ignores the unknown DC binary.
    if (self->enable_data_channel_ && ops->send_data != nullptr) {
      size_t dc_need = (size_t) enc_len + 4;
      if (self->dc_tx_cap_ < dc_need) {
        heap_caps_free(self->dc_tx_);
        self->dc_tx_ = heap_caps_malloc(dc_need + 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        self->dc_tx_cap_ = (self->dc_tx_ != nullptr) ? dc_need + 4096 : 0;
      }
      if (self->dc_tx_ != nullptr) {
        memcpy(self->dc_tx_, VIDEO_DC_MAGIC, 4);
        memcpy(static_cast<uint8_t *>(self->dc_tx_) + 4, self->h264_buf_, enc_len);
        esp_peer_data_frame_t df = {};
        df.type = ESP_PEER_DATA_CHANNEL_DATA;
        df.data = static_cast<uint8_t *>(self->dc_tx_);
        df.size = static_cast<int>(dc_need);
        ops->send_data(handle, &df);
      }
    }
    pts += frame_ms;
    if ((self->video_tx_count_++ % 30) == 0)
      ESP_LOGI(TAG, "video TX: %u frames (%s %ux%u, %u bytes, type=%d, ret=%d)",
               (unsigned) self->video_tx_count_, mjpeg ? "MJPEG" : "H264", self->enc_w_,
               self->enc_h_, (unsigned) enc_len, frame_type, vret);

    uint32_t dt = millis() - t0;
    if (dt < frame_ms)
      vTaskDelay(pdMS_TO_TICKS(frame_ms - dt));
  }
  self->close_video_encoder_();
  self->video_task_done_ = true;
  vTaskDelete(nullptr);
}

// Create the HW JPEG decoder + its DMA input/output buffers. Buffers are sized
// once for the negotiated resolution (a JPEG is always smaller than the RGB565
// raw of the same size, so the input cap = W*H*2 is safe); frames larger than
// the negotiated size are dropped. Called lazily from render_remote_frame_().
bool WebRTCComponent::open_jpeg_decoder_() {
  if (this->jdec_ != nullptr)
    return true;
  jpeg_decode_engine_cfg_t dcfg = {};
  dcfg.intr_priority = 0;
  dcfg.timeout_ms = 1000 / (this->video_fps_ > 0 ? this->video_fps_ : 15) + 20;
  jpeg_decoder_handle_t jd = nullptr;
  if (jpeg_new_decoder_engine(&dcfg, &jd) != ESP_OK || jd == nullptr) {
    ESP_LOGE(TAG, "jpeg_new_decoder_engine failed");
    return false;
  }
  this->jdec_ = jd;

  // Size the decode buffers to handle ANY peer resolution up to 720p (or our own
  // send resolution if larger), NOT just our own video_width/height. This
  // DECOUPLES send from receive: the two ends can run different resolutions and
  // each simply adapts to whatever the other sends (no dropped frames).
  size_t cap = (size_t) this->video_w_ * this->video_h_ * 2;
  const size_t cap_720p = (size_t) 1280 * 720 * 2;
  if (cap < cap_720p)
    cap = cap_720p;
  jpeg_decode_memory_alloc_cfg_t incfg = {};
  incfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  size_t gi = 0;
  this->jpeg_dec_in_ = jpeg_alloc_decoder_mem(cap, &incfg, &gi);
  this->jpeg_dec_in_cap_ = (gi > 0) ? gi : cap;
  jpeg_decode_memory_alloc_cfg_t outcfg = {};
  outcfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t go = 0;
  this->remote_rgb_ = jpeg_alloc_decoder_mem(cap, &outcfg, &go);
  this->remote_rgb_cap_ = (go > 0) ? go : cap;
  if (this->jpeg_dec_in_ == nullptr || this->remote_rgb_ == nullptr) {
    ESP_LOGE(TAG, "JPEG dec buffers alloc failed (cap=%u)", (unsigned) cap);
    return false;
  }
#ifdef USE_LVGL
  if (this->remote_draw_buf_ == nullptr)
    this->remote_draw_buf_ = new lv_draw_buf_t{};
#endif
  ESP_LOGI(TAG, "MJPEG decoder ready (max %ux%u, in cap %u)", this->video_w_, this->video_h_,
           (unsigned) this->jpeg_dec_in_cap_);
  return true;
}

// Main-loop context (same task as LVGL): take the newest stashed JPEG, HW-decode
// it to RGB565, and hand the buffer to the canvas. Because this runs on the LVGL
// task and LVGL only reads the canvas later in the same loop iteration, the
// decode fully completes before any render -> no tearing, no extra buffering.
void WebRTCComponent::render_remote_frame_() {
#ifdef USE_LVGL
  if (this->remote_canvas_ == nullptr)
    return;

  // H.264 (edge264): the decode task already produced an RGB565 frame; here we
  // only swap it onto the canvas. Double-buffered so the task fills one buffer
  // while LVGL shows the other.
  if (this->video_codec_ == VIDEO_CODEC_H264) {
    if (!this->rgb_ready_.load(std::memory_order_acquire))
      return;
    uint8_t *tmp = this->rgb_display_;
    this->rgb_display_ = this->rgb_decode_;
    this->rgb_decode_ = tmp;
    this->rgb_ready_.store(false, std::memory_order_release);
    auto *canvas = static_cast<lv_obj_t *>(this->remote_canvas_);
    lv_canvas_set_buffer(canvas, this->rgb_display_, this->video_w_, this->video_h_,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
    if ((this->video_rx_count_++ % 30) == 0)
      ESP_LOGI(TAG, "video RX: %u frames (edge264 %ux%u)", (unsigned) this->video_rx_count_,
               this->video_w_, this->video_h_);
    return;
  }

  if (this->video_codec_ != VIDEO_CODEC_MJPEG || !this->jpeg_rx_ready_ ||
      this->jpeg_rx_mtx_ == nullptr)
    return;
  if (this->jdec_ == nullptr && !this->open_jpeg_decoder_())
    return;

  auto mtx = static_cast<SemaphoreHandle_t>(this->jpeg_rx_mtx_);
  if (xSemaphoreTake(mtx, 0) != pdTRUE)
    return;  // peer task is mid-copy; catch it next loop
  int jsize = this->jpeg_rx_size_;
  this->jpeg_rx_ready_ = false;
  bool ok = (jsize > 0 && (size_t) jsize <= this->jpeg_dec_in_cap_);
  if (ok)
    memcpy(this->jpeg_dec_in_, this->jpeg_rx_buf_, jsize);
  xSemaphoreGive(mtx);
  if (!ok) {
    if (jsize > 0)
      ESP_LOGW(TAG, "remote JPEG too big for buffer (%d > %u)", jsize,
               (unsigned) this->jpeg_dec_in_cap_);
    return;
  }

  jpeg_decode_picture_info_t pi = {};
  if (jpeg_decoder_get_info(static_cast<uint8_t *>(this->jpeg_dec_in_), jsize, &pi) != ESP_OK)
    return;
  size_t need = (size_t) pi.width * pi.height * 2;
  if (need == 0 || need > this->remote_rgb_cap_) {
    if ((this->video_rx_count_ % 100) == 0)
      ESP_LOGW(TAG, "remote frame %ux%u exceeds RGB buffer", (unsigned) pi.width,
               (unsigned) pi.height);
    return;
  }

  jpeg_decode_cfg_t dc = {};
  dc.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  dc.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  dc.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t outlen = 0;
  // Serialise with the video_tx encoder (shared HW JPEG peripheral).
  if (this->jpeg_mutex_ != nullptr)
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->jpeg_mutex_), portMAX_DELAY);
  esp_err_t de = jpeg_decoder_process(static_cast<jpeg_decoder_handle_t>(this->jdec_), &dc,
                                      static_cast<uint8_t *>(this->jpeg_dec_in_), jsize,
                                      static_cast<uint8_t *>(this->remote_rgb_),
                                      this->remote_rgb_cap_, &outlen);
  if (this->jpeg_mutex_ != nullptr)
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(this->jpeg_mutex_));
  if (de != ESP_OK) {
    if ((this->video_rx_count_ % 100) == 0)
      ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(de));
    return;
  }
  // The decoder DMAs into PSRAM; invalidate the CPU cache so LVGL reads fresh
  // pixels (same M2C sync the camera->canvas path uses).
  esp_cache_msync(this->remote_rgb_, need,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  // The HW JPEG decoder emits RGB565 with a byte order LVGL reads swapped
  // (psychedelic magenta/cyan colors). Swap the two bytes of each pixel in
  // place, exactly like face2face's proven swap_colors_ path.
  {
    uint16_t *px = static_cast<uint16_t *>(this->remote_rgb_);
    size_t n = need / 2;
    for (size_t i = 0; i < n; i++)
      px[i] = (uint16_t) ((px[i] >> 8) | (px[i] << 8));
  }

  auto *canvas = static_cast<lv_obj_t *>(this->remote_canvas_);
  auto *db = static_cast<lv_draw_buf_t *>(this->remote_draw_buf_);
  const uint32_t stride = (uint32_t) pi.width * 2;
  if (!this->remote_draw_buf_init_ || this->rmt_w_ != pi.width || this->rmt_h_ != pi.height) {
    lv_draw_buf_init(db, pi.width, pi.height, LV_COLOR_FORMAT_RGB565, stride, this->remote_rgb_,
                     need);
    lv_draw_buf_set_flag(db, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_canvas_set_draw_buf(canvas, db);
    this->remote_draw_buf_init_ = true;
    this->rmt_w_ = pi.width;
    this->rmt_h_ = pi.height;
    ESP_LOGI(TAG, "remote canvas draw_buf %ux%u (stride %u)", (unsigned) pi.width,
             (unsigned) pi.height, (unsigned) stride);
  } else {
    db->data = static_cast<uint8_t *>(this->remote_rgb_);
  }
  lv_obj_invalidate(canvas);
  if ((this->video_rx_count_++ % 30) == 0)
    ESP_LOGI(TAG, "video RX: %u frames (%ux%u, %d bytes JPEG)", (unsigned) this->video_rx_count_,
             (unsigned) pi.width, (unsigned) pi.height, jsize);
#endif  // USE_LVGL
}

// I420 (contiguous, width*height + 2*(w/2*h/2)) -> RGB565. Ported verbatim from
// ip_camera_viewer (scalar BT.601, 2x2 block). Runs on the decode task.
void WebRTCComponent::convert_yuv420_to_rgb565_(uint8_t *yuv, uint8_t *rgb565, int width,
                                                int height) {
  const int cw = width >> 1;
  const uint8_t *y_plane = yuv;
  const uint8_t *u_plane = yuv + width * height;
  const uint8_t *v_plane = u_plane + cw * (height >> 1);
  uint16_t *rgb = (uint16_t *) rgb565;
  for (int j = 0; j < height; j += 2) {
    const uint8_t *y0 = y_plane + j * width;
    const uint8_t *y1 = y0 + width;
    const uint8_t *up = u_plane + (j >> 1) * cw;
    const uint8_t *vp = v_plane + (j >> 1) * cw;
    uint16_t *d0 = rgb + j * width;
    uint16_t *d1 = d0 + width;
    for (int i = 0; i < width; i += 2) {
      int u = up[i >> 1] - 128;
      int v = vp[i >> 1] - 128;
      int rc = (v * 359) >> 8;
      int gc = (u * 88 + v * 183) >> 8;
      int bc = (u * 454) >> 8;
      for (int k = 0; k < 2; k++) {
        int y = y0[i + k];
        int r = y + rc, g = y - gc, b = y + bc;
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        d0[i + k] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        y = y1[i + k];
        r = y + rc, g = y - gc, b = y + bc;
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        d1[i + k] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      }
    }
  }
}

#ifdef USE_H264_HP_EDGE264
// Allocate the edge264 decoder + the I420/RGB565 buffers. Called once at the
// start of the decode task. begin(0) = MONO-THREAD (synchronous, no pthreads):
// multi-thread edge264 deadlocks on ESP-IDF FreeRTOS (see h264_hp). The decode
// therefore runs inline on THIS task, which is why it has a large stack.
bool WebRTCComponent::open_h264_decoder_() {
  if (this->hp_dec_ != nullptr)
    return true;
  this->rgb_buf_size_ = (size_t) this->video_w_ * this->video_h_ * 2;
  this->yuv_i420_size_ = (size_t) this->video_w_ * this->video_h_ * 3 / 2;
  this->rgb_a_ = static_cast<uint8_t *>(
      heap_caps_malloc(this->rgb_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->rgb_b_ = static_cast<uint8_t *>(
      heap_caps_malloc(this->rgb_buf_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->yuv_i420_ = static_cast<uint8_t *>(
      heap_caps_malloc(this->yuv_i420_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->rgb_a_ == nullptr || this->rgb_b_ == nullptr || this->yuv_i420_ == nullptr) {
    ESP_LOGE(TAG, "edge264 buffers alloc failed");
    return false;
  }
  memset(this->rgb_a_, 0, this->rgb_buf_size_);
  memset(this->rgb_b_, 0, this->rgb_buf_size_);
  this->rgb_decode_ = this->rgb_a_;
  this->rgb_display_ = this->rgb_b_;

  auto *dec = new h264_hp::H264HpDecoder();
  if (!dec->begin(0)) {
    ESP_LOGE(TAG, "edge264 decoder init failed");
    delete dec;
    return false;
  }
  this->hp_dec_ = dec;
  ESP_LOGI(TAG, "edge264 H.264 decoder ready (%ux%u)", this->video_w_, this->video_h_);
  return true;
}

// Dedicated decode task (core 1): drain the H.264 access-unit queue in order,
// edge264-decode to I420, crop/letterbox to the configured size, convert to
// RGB565 into the back buffer, and flag it ready. The main loop swaps it onto
// the canvas. No LVGL calls here (LVGL only runs on the main task).
void WebRTCComponent::video_rx_fn_(void *arg) {
  auto *self = static_cast<WebRTCComponent *>(arg);
  auto q = static_cast<QueueHandle_t>(self->video_q_);
  if (!self->open_h264_decoder_()) {
    self->video_rx_task_done_ = true;
    vTaskDelete(nullptr);
    return;
  }
  auto *dec = static_cast<h264_hp::H264HpDecoder *>(self->hp_dec_);
  const int W = self->video_w_, H = self->video_h_;
  const int cfg_cw = W / 2, cfg_ch = H / 2;

  while (self->video_rx_run_) {
    if (q == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    WebRtcAu au;
    if (xQueueReceive(q, &au, pdMS_TO_TICKS(50)) != pdTRUE)
      continue;
    // Re-sync: after an overflow (or at start) skip P-frames until an IDR, or
    // edge264 decodes garbage against missing reference frames.
    if (self->need_idr_) {
      if (!au.idr) {
        heap_caps_free(au.data);
        continue;
      }
      self->need_idr_ = false;
    }
    const uint32_t err_before = dec->decode_errors();
    dec->decode_annexb(au.data, au.size);
    heap_caps_free(au.data);
    // WebRTC is lossy (UDP): a P-frame with a missing slice makes edge264 decode
    // against absent references -> garbage, and can even fault. If the decoder
    // flagged an error, drop this output and re-sync at the next IDR instead of
    // propagating (and re-referencing) corruption.
    if (dec->decode_errors() != err_before) {
      self->need_idr_ = true;
      if ((self->video_rx_count_ % 30) == 0)
        ESP_LOGW(TAG, "edge264 decode error -> re-syncing to next IDR");
      continue;
    }

    h264_hp::DecodedFrame f;
    bool got = false;
    while (dec->get_frame(&f)) {
      const int sw = f.width & ~1;
      const int sh = f.height & ~1;
      // The browser may send a resolution larger than we sized buffers for.
      // We crop safely below, but warn once so the mismatch is visible (a much
      // larger stream also strains PSRAM / decode time).
      if (sw > W || sh > H) {
        static bool warned = false;
        if (!warned) {
          ESP_LOGW(TAG, "remote H.264 is %dx%d > configured %dx%d — cropped. Set "
                        "video_width/height to match the sender.",
                   sw, sh, W, H);
          warned = true;
        }
      }
      if (sw > 0 && sh > 0 && f.y && f.cb && f.cr) {
        const int dw = (sw < W ? sw : W) & ~1;
        const int dh = (sh < H ? sh : H) & ~1;
        if (dw < W || dh < H)
          memset(self->yuv_i420_, 0, self->yuv_i420_size_);  // black borders
        uint8_t *Y = self->yuv_i420_;
        uint8_t *U = Y + (size_t) W * H;
        uint8_t *V = U + (size_t) cfg_cw * cfg_ch;
        for (int row = 0; row < dh; row++)
          memcpy(Y + (size_t) row * W, f.y + (size_t) row * f.stride_y, dw);
        for (int row = 0; row < dh / 2; row++)
          memcpy(U + (size_t) row * cfg_cw, f.cb + (size_t) row * f.stride_c, dw / 2);
        for (int row = 0; row < dh / 2; row++)
          memcpy(V + (size_t) row * cfg_cw, f.cr + (size_t) row * f.stride_c, dw / 2);
        self->convert_yuv420_to_rgb565_(self->yuv_i420_, self->rgb_decode_, W, H);
        got = true;
      }
      dec->release_frame();
    }
    if (got) {
      self->rgb_ready_.store(true, std::memory_order_release);
      // Wait for the main loop to consume before decoding the next display
      // frame, so the decode task never overwrites the buffer LVGL is showing.
      for (int i = 0; i < 100 && self->video_rx_run_ &&
                      self->rgb_ready_.load(std::memory_order_acquire);
           i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  self->video_rx_task_done_ = true;
  vTaskDelete(nullptr);
}
#endif  // USE_H264_HP_EDGE264
#endif  // USE_ESP_WEBRTC_VIDEO

void WebRTCComponent::start_audio_bridge_() {
  if (!this->audio_bridge_enabled_() || this->bridge_active_)
    return;
  const bool opus = (this->audio_codec_ == AUDIO_CODEC_OPUS);
  const uint32_t spk_rate = opus ? OPUS_RATE : G711_RATE;
  ESP_LOGI(TAG, "audio bridge: starting mic+speaker (%s @ %u Hz)", opus ? "Opus" : "G.711",
           (unsigned) spk_rate);
  // Speaker plays decoded mono PCM16 at the codec's rate.
  this->spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, spk_rate));
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
  // Retry a few times: a crashed/rebooted peer can leave a ghost that makes the
  // join look "full" until the server expires it. A short retry rides that out.
  const bool simple = !this->signaling_url_.empty();
  if (simple) {
    this->signaling_.set_simple_backend(this->signaling_url_, this->signaling_token_);
    this->signaling_.set_room(this->room_id_);
    ESP_LOGI(TAG, "Joining self-hosted signaling: %s room=%s", this->signaling_url_.c_str(),
             this->room_id_.c_str());
  } else {
    ESP_LOGI(TAG, "Joining signaling room: https://webrtc.espressif.com/join/%s",
             this->room_id_.c_str());
  }
  std::string url = "https://webrtc.espressif.com/join/" + this->room_id_;
  bool joined = false;
  for (int attempt = 1; attempt <= 3 && !joined; attempt++) {
    joined = simple ? this->signaling_.join_simple() : this->signaling_.join(url);
    if (!joined && attempt < 3) {
      ESP_LOGW(TAG, "join attempt %d failed (room busy/ghost or server down?); retrying in 3s",
               attempt);
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
  if (!joined) {
    ESP_LOGE(TAG, "signaling join failed (room full or unreachable)");
    return;
  }
  this->controlled_ = !this->signaling_.is_initiator();
  // Pin the role if configured, so the offerer/answerer election no longer
  // depends on AppRTC join order (which flips when a ghost occupies the room).
  if (this->role_ != ROLE_AUTO) {
    const bool caller = (this->role_ == ROLE_CALLER);
    this->signaling_.force_initiator(caller);
    this->controlled_ = !caller;
    ESP_LOGI(TAG, "role pinned by config: %s", caller ? "caller (offerer)" : "callee (answerer)");
  }
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
  // 32 KB: this task runs esp_peer's main_loop (ICE + the heavy mbedTLS DTLS
  // handshake + SCTP) AND the incoming-audio callback (Opus decode ~6 KB). At
  // connect time those overlap and 16 KB overflowed (crash right after
  // "DTLS handshake success" / audio bridge start).
  xTaskCreatePinnedToCore(task_fn_, "webrtc_peer", 32768, this, 5,
                          reinterpret_cast<TaskHandle_t *>(&this->task_), 0);

  // 4b) Audio bridge: mic ring + callback + G.711/Opus TX task (only if configured).
  if (this->audio_bridge_enabled_()) {
#ifdef USE_ESP_WEBRTC_OPUS
    if (this->audio_codec_ == AUDIO_CODEC_OPUS && !this->open_opus_())
      ESP_LOGE(TAG, "Opus init failed; audio TX/RX will be silent");
#endif
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
      // 20 KB: the Opus encoder (esp_audio_enc_process) is stack-heavy (~12 KB);
      // 4 KB overflowed it (stack protection fault in webrtc_atx). G.711 needs
      // almost none, but sizing for Opus is harmless.
      xTaskCreatePinnedToCore(audio_tx_fn_, "webrtc_atx", 20480, this, 5,
                              reinterpret_cast<TaskHandle_t *>(&this->audio_tx_task_), 0);
    }
    ESP_LOGI(TAG, "audio bridge ready (mic rate %u Hz -> G.711 8 kHz)", (unsigned) this->audio_rate_);
  }

#ifdef USE_ESP_WEBRTC_VIDEO
  // 4c) Video bridge: camera RGB565 -> PPA YUV420 -> H.264 -> send_video.
  if (this->video_bridge_enabled_() && this->video_codec_ == VIDEO_CODEC_MJPEG) {
    // MJPEG TX runs on the MAIN LOOP (pump_mjpeg_tx_, the proven face2face
    // path): no webrtc_vtx task, no PPA. See pump_mjpeg_tx_ for why.
    this->video_tx_count_ = 0;
    this->last_vtx_ms_ = 0;
    ESP_LOGI(TAG, "video bridge ready (main-loop MJPEG, target %ux%u @%ufps)", this->video_w_,
             this->video_h_, this->video_fps_);
  } else if (this->video_bridge_enabled_() && this->video_tx_task_ == nullptr) {
    this->video_run_ = true;
    this->video_task_done_ = false;
    this->video_tx_count_ = 0;
    // Priority 3 (BELOW fdaudio's real-time codec tasks at 5 on this same core):
    // video is best-effort, audio must never be starved. At equal priority the
    // H.264 encode round-robins with fdaudio_spk/mic and underruns the I2S TX
    // ("channel not enabled"); lower priority lets audio always preempt it.
    // 40 KB: we call the camera's capture_frame() on THIS task (sole-consumer
    // mode). capture_frame() is stack-heavy (V4L2 ioctl structs, the first-frame
    // ESP_LOGI block, AND its internal PPA transform, which is heavier when the
    // camera does rotation/mirror) and previously ran on lvgl_camera_display's
    // larger task. On 8 KB it overflowed and crashed with an "Instruction access
    // fault" right at "First frame captured"; 24 KB still overflowed on the
    // rotating (Waveshare) config, so give it a generous 40 KB.
    xTaskCreatePinnedToCore(video_tx_fn_, "webrtc_vtx", 40960, this, 3,
                            reinterpret_cast<TaskHandle_t *>(&this->video_tx_task_), 1);
    const char *vc = (this->video_codec_ == VIDEO_CODEC_MJPEG) ? "MJPEG" : "H.264";
    ESP_LOGI(TAG, "video bridge ready (camera -> %s %ux%u @%ufps)", vc, this->video_w_,
             this->video_h_, this->video_fps_);
  }
#ifdef USE_H264_HP_EDGE264
  // 4d) H.264 receive via edge264: dedicated core-1 decode task (big stack: the
  // scalar decode runs inline on it). Only when a remote canvas is wired.
  if (this->video_codec_ == VIDEO_CODEC_H264 && this->remote_canvas_ != nullptr &&
      this->video_rx_task_ == nullptr) {
    this->need_idr_ = true;
    this->video_rx_run_ = true;
    this->video_rx_task_done_ = false;
    this->video_rx_count_ = 0;
    // Priority 3, below fdaudio's core-1 audio tasks (5): the long inline edge264
    // decode must never hold core 1 against real-time audio.
    xTaskCreatePinnedToCore(video_rx_fn_, "webrtc_vrx", 32768, this, 3,
                            reinterpret_cast<TaskHandle_t *>(&this->video_rx_task_), 1);
    ESP_LOGI(TAG, "H.264 receive ready (edge264 -> canvas %ux%u)", this->video_w_,
             this->video_h_);
  }
#endif
#endif

  // 5) Start the ICE agent for BOTH roles. esp_peer_new_connection() gathers ICE
  // and produces the local SDP; esp_webrtc calls it unconditionally (no role
  // check). We previously called it only for the offerer, so the ANSWERER's ICE
  // agent never started ("AGENT: Start agent" was missing on the answerer) and a
  // P4<->P4 call never connected. Do it BEFORE connect() replays the queued
  // offer, so the answerer's agent is up when that offer arrives.
  ops->new_connection(handle);

  // 6) Open the WebSocket and replay the queued offer (answerer: this feeds the
  // peer's offer, so esp_peer produces the answer via on_msg; offerer already
  // emitted its offer from new_connection above).
  this->signaling_.connect();
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
  this->jpeg_rx_ready_ = false;  // don't render a stale remote frame after teardown
  this->stop_audio_bridge_();

  // 2) Ask both worker tasks to exit and WAIT for them: they dereference the
  // peer handle (main_loop / send_audio), so they MUST be gone before we close
  // it, or we get a use-after-free. Bounded wait (~600 ms) to avoid hanging.
  const bool had_audio = (this->audio_tx_task_ != nullptr);
  const bool had_peer = (this->task_ != nullptr);
  const bool had_video = (this->video_tx_task_ != nullptr);
  const bool had_vrx = (this->video_rx_task_ != nullptr);
  this->audio_run_ = false;
  this->video_run_ = false;
  this->video_rx_run_ = false;
  this->run_ = false;
  for (int i = 0; i < 60; i++) {
    bool audio_ok = !had_audio || this->audio_task_done_;
    bool peer_ok = !had_peer || this->peer_task_done_;
    bool video_ok = !had_video || this->video_task_done_;
    bool vrx_ok = !had_vrx || this->video_rx_task_done_;
    if (audio_ok && peer_ok && video_ok && vrx_ok)
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  this->audio_tx_task_ = nullptr;
  this->video_tx_task_ = nullptr;
  this->video_rx_task_ = nullptr;
  this->task_ = nullptr;
#ifdef USE_ESP_WEBRTC_VIDEO
  // MJPEG main-loop TX: no task owns the encoder, so close it here (we ARE the
  // main loop; pump_mjpeg_tx_ can't be running concurrently).
  if (this->video_codec_ == VIDEO_CODEC_MJPEG)
    this->close_video_encoder_();
  // Drain any queued H.264 access units (the decode task is gone now) and re-arm
  // the IDR re-sync so the next call starts cleanly.
  if (this->video_q_ != nullptr) {
    WebRtcAu au;
    while (xQueueReceive(static_cast<QueueHandle_t>(this->video_q_), &au, 0) == pdTRUE)
      heap_caps_free(au.data);
  }
  this->need_idr_ = true;
  this->rgb_ready_.store(false, std::memory_order_release);
#endif

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

#ifdef USE_ESP_WEBRTC_VIDEO
  // Decode + draw the remote peer's newest MJPEG frame on the LVGL (main) task.
  this->render_remote_frame_();
  // MJPEG send also runs here (capture -> CPU decimate -> HW JPEG -> send), the
  // proven face2face architecture: encode and decode naturally serialized.
  if (this->started_ && this->video_codec_ == VIDEO_CODEC_MJPEG && this->video_bridge_enabled_())
    this->pump_mjpeg_tx_();
#endif

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
  if (this->audio_bridge_enabled_()) {
    const char *ac = (this->audio_codec_ == AUDIO_CODEC_OPUS)    ? "Opus 16 kHz"
                     : (this->audio_codec_ == AUDIO_CODEC_G711U) ? "G.711u"
                                                                 : "G.711a";
    ESP_LOGCONFIG(TAG, "  Audio bridge: fdaudio mic+speaker, %s (mic rate %u Hz)", ac,
                  (unsigned) this->audio_rate_);
  }
  if (this->video_bridge_enabled_()) {
    const char *vc = (this->video_codec_ == VIDEO_CODEC_MJPEG) ? "MJPEG" : "H.264";
    ESP_LOGCONFIG(TAG, "  Video bridge: camera -> %s (%ux%u @%ufps)", vc, this->video_w_,
                  this->video_h_, this->video_fps_);
  }
  if (this->remote_canvas_ != nullptr) {
    const char *dec = (this->video_codec_ == VIDEO_CODEC_MJPEG) ? "MJPEG HW decode"
                                                                : "H.264 edge264 SW decode";
    ESP_LOGCONFIG(TAG, "  Remote video: %s -> LVGL canvas", dec);
  }
  ESP_LOGCONFIG(TAG, "  Signaling: AppRTC (webrtc.espressif.com)");
}

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
