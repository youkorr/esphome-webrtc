#pragma once

#include "esphome/core/helpers.h"

#include <functional>
#include <string>

#ifdef USE_ESP32

namespace esphome {
namespace webrtc {

// Minimal AppRTC signaling client. Protocol reimplemented from
// esp-webrtc-solution/components/esp_webrtc/impl/apprtc_signal/signal_default.c,
// but driving esp_peer directly instead of the esp_webrtc wrapper.
//
// Flow: HTTP POST join -> params (client_id, room_id, wss_url, is_initiator);
// WebSocket to wss_url + register; receive offer/answer/candidate/bye; send
// local SDP/candidate via HTTP POST to base_url/message/<room>/<client>.
class ApprtcSignaling {
 public:
  // Callbacks fire on the websocket task.
  std::function<void(const std::string &sdp)> on_remote_sdp;
  std::function<void(const std::string &candidate)> on_remote_candidate;
  std::function<void()> on_bye;

  // Join the room. signal_url e.g. https://webrtc.espressif.com/join/<room>.
  bool start(const std::string &signal_url);
  void stop();

  // Send the local SDP / ICE candidate to the peer (HTTP POST /message/...).
  void send_sdp(const std::string &sdp);
  void send_candidate(const std::string &candidate);

  bool is_initiator() const { return this->is_initiator_; }
  bool joined() const { return !this->client_id_.empty(); }

  // Invoked from the (static) websocket event handler in signaling.cpp.
  void on_ws_connected_();
  void handle_ws_text_(const char *text, int len);

 protected:
  std::string base_url_;
  std::string room_id_;
  std::string client_id_;
  std::string wss_url_;
  std::string message_url_;
  bool is_initiator_{false};
  void *ws_{nullptr};  // esp_websocket_client_handle_t
};

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
