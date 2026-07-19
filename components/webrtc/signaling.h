#pragma once

#include "esphome/core/helpers.h"

#include <functional>
#include <string>
#include <vector>

#ifdef USE_ESP32

namespace esphome {
namespace webrtc {

// Minimal AppRTC signaling client. Protocol reimplemented from
// esp-webrtc-solution/components/esp_webrtc/impl/apprtc_signal/signal_default.c,
// but driving esp_peer directly instead of the esp_webrtc wrapper.
//
// Two-step so the caller can learn is_initiator() (from join) and open the
// peer with the right role BEFORE the queued offer is replayed by connect().
class ApprtcSignaling {
 public:
  // Callbacks fire on the websocket task (or the caller's thread for replays).
  std::function<void(const std::string &sdp)> on_remote_sdp;
  std::function<void(const std::string &candidate)> on_remote_candidate;
  std::function<void()> on_bye;

  // Step 1: HTTP POST join <signal_url> (e.g. https://webrtc.espressif.com/
  // join/<room>). Parses client_id/room_id/wss_url/is_initiator and stashes any
  // messages already queued in the room. Does NOT touch the peer.
  bool join(const std::string &signal_url);
  // Step 2: open the WebSocket (register on connect) and replay the queued
  // messages (the peer's offer if it joined first).
  bool connect();
  void stop();

  // Send the local SDP / ICE candidate to the peer (HTTP POST /message/...).
  void send_sdp(const std::string &sdp);
  void send_candidate(const std::string &candidate);

  bool is_initiator() const { return this->is_initiator_; }
  // Override the server-assigned initiator flag (call AFTER join()). Used when the
  // role is pinned by config so the offer/answer election no longer depends on
  // AppRTC join order (which flips when a crashed peer leaves a ghost in the room).
  void force_initiator(bool v) { this->is_initiator_ = v; }

  // Invoked from the (static) websocket event handler in signaling.cpp.
  void on_ws_connected_();
  void handle_ws_text_(const char *text, int len);

 protected:
  std::string base_url_;
  std::string room_id_;
  std::string client_id_;
  std::string wss_url_;
  std::string message_url_;
  std::string origin_;  // "Origin: <base_url>\r\n" header for the WebSocket
  std::vector<std::string> pending_msgs_;  // queued room messages, replayed in connect()
  bool is_initiator_{false};
  void *ws_{nullptr};  // esp_websocket_client_handle_t
};

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
