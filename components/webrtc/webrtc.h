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
namespace esp_cam_sensor {
class MipiDSICamComponent;
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

// Who offers (creates the SDP offer) in the call. AUTO defers to AppRTC join
// order (fine for P4<->browser); CALLER/CALLEE pin the role so a fixed P4<->P4
// pair always elects the same offerer/answerer regardless of boot order or
// ghost clients left in the room by a crash.
enum PeerRole {
  ROLE_AUTO = 0,
  ROLE_CALLER,   // always the offerer (CONTROLLING)
  ROLE_CALLEE,   // always the answerer (CONTROLLED)
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
  void set_role(PeerRole r) { this->role_ = r; }
  // Self-hosted signaling (our signaling-server/). When set, the P4 uses the
  // simple HTTP+token backend instead of the public AppRTC server.
  void set_signaling_url(const std::string &u) { this->signaling_url_ = u; }
  void set_signaling_token(const std::string &t) { this->signaling_token_ = t; }
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

  // Video bridge: share the esp_cam_sensor camera's RGB565 frames -> PPA YUV420
  // -> H.264 -> send to the peer. The camera keeps streaming for LVGL.
  void set_camera(esp_cam_sensor::MipiDSICamComponent *c) { this->camera_ = c; }
  void set_drive_camera(bool d) { this->drive_camera_ = d; }

  // LVGL canvas that displays the REMOTE peer's video. Only meaningful with the
  // MJPEG codec (the P4 has no HW H.264 decoder, so ESP<->ESP video uses HW
  // Motion-JPEG). Wire it from a YAML lambda:
  //   id(webrtc).set_remote_canvas(id(remote_canvas));
  // Stored as void* so this header stays free of the LVGL headers (lv_obj_t*
  // converts implicitly to void*).
  void set_remote_canvas(void *canvas) { this->remote_canvas_ = canvas; }

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
  // Incoming ENCODED video frame (MJPEG) from esp_peer (peer task) -> stash the
  // JPEG bytes; the HW decode + LVGL canvas draw happen in loop() (main task).
  void on_video_frame_(const uint8_t *data, int size);

 protected:
  bool open_peer_();
  static void task_fn_(void *arg);       // runs esp_peer main_loop
  static void audio_tx_fn_(void *arg);   // mic ring -> G.711/Opus -> esp_peer send_audio
  bool open_opus_();                     // esp_audio_codec Opus encoder + decoder
  static void video_tx_fn_(void *arg);   // camera RGB565 -> PPA YUV -> H.264 -> send_video
  bool open_video_encoder_();            // esp_h264 HW encoder + PPA client
  void close_video_encoder_();
  bool open_jpeg_decoder_();             // HW JPEG decoder + DMA buffers (MJPEG recv)
  void render_remote_frame_();           // loop(): decode/swap remote frame -> canvas
  static void video_rx_fn_(void *arg);   // edge264 H.264 decode task
  bool open_h264_decoder_();             // edge264 decoder + queue + RGB/YUV buffers
  void convert_yuv420_to_rgb565_(uint8_t *yuv, uint8_t *rgb565, int w, int h);
  // Remote SDP/ICE from signaling -> feed into esp_peer.
  void feed_remote_sdp_(const std::string &sdp);
  void feed_remote_candidate_(const std::string &candidate);

  // --- audio bridge helpers ---
  bool audio_bridge_enabled_() const { return this->mic_ != nullptr && this->spk_ != nullptr; }
  bool video_bridge_enabled_() const { return this->camera_ != nullptr; }
  void start_audio_bridge_();  // (re)start mic+speaker for a call (on connect)
  void stop_audio_bridge_();   // stop mic+speaker (on disconnect)
  void on_mic_data_(const std::vector<uint8_t> &data);  // mic cb -> ring buffer

  std::string room_id_{"esphome_room"};
  std::string signaling_url_;    // empty = public AppRTC; set = self-hosted simple backend
  std::string signaling_token_;  // X-Auth-Token for the self-hosted backend
  PeerRole role_{ROLE_AUTO};
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
  // Set by each task just before it self-deletes, so stop() can WAIT for them to
  // exit before closing the peer handle they use (avoids a use-after-free).
  volatile bool peer_task_done_{true};
  volatile bool audio_task_done_{true};
  uint32_t audio_tx_count_{0};      // frames sent to peer (throttled telemetry)
  uint32_t audio_rx_count_{0};      // frames received from peer (throttled telemetry)
  // Latched so start_audio_bridge_()/stop_audio_bridge_() run once per edge.
  // Atomic: written by the main loop (start/stop_audio_bridge_), read by the
  // webrtc_peer task in on_audio_frame_ to know the speaker is fully started
  // before it enqueues PCM. Without this gate the first incoming frame calls
  // spk_->play() (which lazily inits fdaudio) concurrently with the main loop's
  // spk_->start() -> two fdaudio inits race -> instruction access fault crash.
  std::atomic<bool> bridge_active_{false};

  // --- Opus audio codec (esp_audio_codec), optional & opt-in via
  // audio_codec: opus. Higher quality than G.711 (16 kHz wideband vs 8 kHz
  // telephony) and what Espressif uses. esp_peer only transports, so we encode
  // /decode here exactly like G.711. Handles kept void* to keep the header light.
  void *opus_enc_{nullptr};      // esp_audio_enc_handle_t
  void *opus_dec_{nullptr};      // esp_audio_dec_handle_t
  int opus_in_bytes_{640};       // encoder input frame size (16 kHz mono 20 ms)
  void *opus_enc_out_{nullptr};  // encoded-Opus output buffer
  size_t opus_enc_out_cap_{0};
  void *opus_pcm_out_{nullptr};  // decoded-PCM output buffer
  size_t opus_pcm_out_cap_{0};

  // --- video bridge (esp_cam_sensor camera -> H.264 -> peer) ---
  esp_cam_sensor::MipiDSICamComponent *camera_{nullptr};
  // true: video_tx drives the V4L2 capture itself (webrtc is the sole camera
  // consumer). false: something else (e.g. lvgl_camera_display for a self-view)
  // already calls capture_frame(), so we only READ the current buffer — two
  // tasks doing VIDIOC_DQBUF/QBUF on the same fd corrupts the buffers.
  bool drive_camera_{true};
  void *venc_{nullptr};             // esp_h264_enc_handle_t
  void *ppa_{nullptr};              // ppa_client_handle_t (RGB565 -> YUV420)
  void *yuv_buf_{nullptr};          // PPA output buffer (YUV420 for H.264, RGB565 for MJPEG)
  size_t yuv_buf_size_{0};
  void *h264_buf_{nullptr};         // encoder output bitstream buffer
  size_t h264_buf_size_{0};
  void *jpeg_in_{nullptr};          // MJPEG only: jpeg_alloc_encoder_mem input (CPU-decimated RGB565)
  size_t jpeg_in_size_{0};
  // MJPEG TX runs on the MAIN LOOP (like the proven face2face pump_video_tx_):
  // capture -> CPU integer decimation -> HW JPEG encode -> send_video. No PPA and
  // no dedicated task: the PPA+task pipeline crashed deterministically on the
  // first frame, while this exact main-loop path is field-proven on both P4s.
  void pump_mjpeg_tx_();
  uint32_t last_vtx_ms_{0};         // main-loop MJPEG frame pacing
  uint32_t last_enc_warn_ms_{0};    // throttled encode-failure warnings
  // Video rides the SCTP DATA CHANNEL in P4<->P4: the video m-line is negotiated
  // a=inactive (no RTP video stream), and esp_peer's send_video then runs its RTP
  // encoder on an uninitialized stream -> crash inside write_rtp_packet /
  // rtp_encoder_encode_generic (proven by addr2line on the crash dumps). Frames
  // are sent as one DC message prefixed with a 4-byte magic ("VID0").
  void *dc_tx_{nullptr};
  size_t dc_tx_cap_{0};
  // The HW JPEG encoder and decoder share ONE peripheral. The encoder runs on the
  // webrtc_vtx task and the decoder on the main loop (render_remote_frame_); a
  // mutex serialises them (concurrent use corrupts the codec -> crash).
  void *jpeg_mutex_{nullptr};       // SemaphoreHandle_t
  uint16_t enc_w_{0};               // encoder-configured frame size (from first frame)
  uint16_t enc_h_{0};
  void *video_tx_task_{nullptr};    // TaskHandle_t
  volatile bool video_run_{false};
  volatile bool video_task_done_{true};
  uint32_t video_tx_count_{0};

  // --- MJPEG (P4 HW JPEG codec) : ESP<->ESP video, since the P4 has a HW JPEG
  // encoder AND decoder but NO HW H.264 decoder. When video_codec_ == MJPEG the
  // send path is camera RGB565 -> PPA (RGB565 downscale) -> HW JPEG encode ->
  // send_video, and the receive path is on_video_frame_ (JPEG) -> HW JPEG decode
  // -> RGB565 -> LVGL canvas.
  void *jenc_{nullptr};             // jpeg_encoder_handle_t (send)
  void *jdec_{nullptr};             // jpeg_decoder_handle_t (receive)
  // Peer task -> loop() handoff of the newest incoming JPEG frame (guarded by
  // jpeg_rx_mtx_; a whole frame is memcpy'd, so the critical sections are short).
  void *jpeg_rx_buf_{nullptr};      // PSRAM stash of the latest incoming JPEG
  size_t jpeg_rx_cap_{0};
  volatile int jpeg_rx_size_{0};
  volatile bool jpeg_rx_ready_{false};
  void *jpeg_rx_mtx_{nullptr};      // SemaphoreHandle_t
  void *jpeg_dec_in_{nullptr};      // DMA-capable decoder input buffer
  size_t jpeg_dec_in_cap_{0};
  void *remote_rgb_{nullptr};       // DMA-capable RGB565 decoder output = canvas buffer
  size_t remote_rgb_cap_{0};
  uint16_t rmt_w_{0};               // last decoded remote frame size
  uint16_t rmt_h_{0};
  uint32_t video_rx_count_{0};
  void *remote_canvas_{nullptr};    // lv_obj_t* (canvas showing the remote peer)
  void *remote_draw_buf_{nullptr};  // heap lv_draw_buf_t (kept void* to avoid LVGL in the header)
  bool remote_draw_buf_init_{false};

  // --- edge264 H.264 receive (software High-profile decode -> LVGL canvas) ---
  // Used when video_codec_ == H264, a remote canvas is wired, and the h264_hp
  // (edge264) lib is in the build. Scalar decode is heavy, so it runs on its own
  // core-1 task; the main loop only swaps the finished RGB565 buffer onto the
  // canvas. H.264 is inter-frame coded, so access units go through an ORDERED
  // bounded queue (not the newest-wins JPEG stash); on overflow we skip to the
  // next IDR to re-sync. hp_dec_ kept void* so the header stays h264_hp-free.
  void *hp_dec_{nullptr};             // h264_hp::H264HpDecoder*
  void *video_q_{nullptr};            // QueueHandle_t of encoded access units
  void *video_rx_task_{nullptr};      // TaskHandle_t
  volatile bool video_rx_run_{false};
  volatile bool video_rx_task_done_{true};
  volatile bool need_idr_{true};      // skip P-frames until the next IDR (re-sync)
  uint8_t *yuv_i420_{nullptr};        // contiguous I420 repack (decode task)
  size_t yuv_i420_size_{0};
  uint8_t *rgb_a_{nullptr};           // RGB565 double buffer
  uint8_t *rgb_b_{nullptr};
  uint8_t *rgb_decode_{nullptr};      // decode task writes here
  uint8_t *rgb_display_{nullptr};     // main loop shows this
  size_t rgb_buf_size_{0};
  std::atomic<bool> rgb_ready_{false};

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
