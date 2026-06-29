#pragma once

#include "esphome/core/automation.h"
#include "webrtc.h"

#ifdef USE_ESP32

namespace esphome {
namespace webrtc {

// --- Triggers ---
class ConnectedTrigger : public Trigger<> {
 public:
  explicit ConnectedTrigger(WebRTCComponent *parent) {
    parent->add_on_connected_callback([this]() { this->trigger(); });
  }
};

class DisconnectedTrigger : public Trigger<> {
 public:
  explicit DisconnectedTrigger(WebRTCComponent *parent) {
    parent->add_on_disconnected_callback([this]() { this->trigger(); });
  }
};

class PairedTrigger : public Trigger<> {
 public:
  explicit PairedTrigger(WebRTCComponent *parent) {
    parent->add_on_paired_callback([this]() { this->trigger(); });
  }
};

class ConnectFailedTrigger : public Trigger<> {
 public:
  explicit ConnectFailedTrigger(WebRTCComponent *parent) {
    parent->add_on_connect_failed_callback([this]() { this->trigger(); });
  }
};

// --- Actions ---
template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<WebRTCComponent> {
 public:
  void play(Ts... x) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<WebRTCComponent> {
 public:
  void play(Ts... x) override { this->parent_->stop(); }
};

template<typename... Ts> class SendDataAction : public Action<Ts...>, public Parented<WebRTCComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, data)
  void play(Ts... x) override { this->parent_->send_data(this->data_.value(x...)); }
};

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
