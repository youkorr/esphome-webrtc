#include "signaling.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cstring>
#include <cstdlib>

extern "C" {
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
}

namespace esphome {
namespace webrtc {

static const char *const TAG = "webrtc.sig";

// --- HTTP POST helper: returns the response body as a string. ---
static esp_err_t http_evt_(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr && evt->data_len > 0) {
    static_cast<std::string *>(evt->user_data)->append(static_cast<const char *>(evt->data),
                                                       evt->data_len);
  }
  return ESP_OK;
}

static std::string http_post_(const std::string &url, const std::string &body) {
  std::string out;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 10000;
  cfg.event_handler = http_evt_;
  cfg.user_data = &out;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) {
    return out;
  }
  esp_http_client_set_header(c, "Content-Type", "application/json");
  if (!body.empty()) {
    esp_http_client_set_post_field(c, body.c_str(), body.size());
  }
  esp_err_t err = esp_http_client_perform(c);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "POST %s failed: %s", url.c_str(), esp_err_to_name(err));
  }
  esp_http_client_cleanup(c);
  return out;
}

// --- WebSocket event handler ---
static void ws_event_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
  auto *self = static_cast<ApprtcSignaling *>(arg);
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "websocket connected");
      self->on_ws_connected_();
      break;
    case WEBSOCKET_EVENT_DATA:
      // op_code 0x01 = text frame (the only kind AppRTC uses).
      if (data->op_code == 0x01 && data->data_len > 0 && data->data_ptr != nullptr) {
        self->handle_ws_text_(data->data_ptr, data->data_len);
      }
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "websocket disconnected");
      break;
    default:
      break;
  }
}

bool ApprtcSignaling::join(const std::string &signal_url) {
  std::string resp = http_post_(signal_url, "");
  if (resp.empty()) {
    ESP_LOGE(TAG, "empty join response from %s", signal_url.c_str());
    return false;
  }
  cJSON *root = cJSON_Parse(resp.c_str());
  if (root == nullptr) {
    ESP_LOGE(TAG, "join: cannot parse response");
    return false;
  }
  cJSON *params = cJSON_GetObjectItem(root, "params");
  bool ok = false;
  this->pending_msgs_.clear();
  do {
    if (params == nullptr) {
      break;
    }
    cJSON *it = cJSON_GetObjectItem(params, "client_id");
    if (it != nullptr && it->valuestring != nullptr) {
      this->client_id_ = it->valuestring;
    }
    it = cJSON_GetObjectItem(params, "room_id");
    if (it != nullptr && it->valuestring != nullptr) {
      this->room_id_ = it->valuestring;
    }
    it = cJSON_GetObjectItem(params, "wss_url");
    if (it != nullptr && it->valuestring != nullptr) {
      this->wss_url_ = it->valuestring;
    }
    it = cJSON_GetObjectItem(params, "is_initiator");
    if (it != nullptr) {
      if (cJSON_IsString(it) && it->valuestring != nullptr) {
        this->is_initiator_ = (strcmp(it->valuestring, "true") == 0);
      } else {
        this->is_initiator_ = cJSON_IsTrue(it);
      }
    }
    this->base_url_ = signal_url;
    size_t p = this->base_url_.find("/join");
    if (p != std::string::npos) {
      this->base_url_ = this->base_url_.substr(0, p);
    }
    this->message_url_ = this->base_url_ + "/message/" + this->room_id_ + "/" + this->client_id_;
    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    if (messages != nullptr && cJSON_IsArray(messages)) {
      int n = cJSON_GetArraySize(messages);
      for (int i = 0; i < n; i++) {
        cJSON *m = cJSON_GetArrayItem(messages, i);
        if (m != nullptr && m->valuestring != nullptr) {
          this->pending_msgs_.emplace_back(m->valuestring);
        }
      }
    }
    if (this->client_id_.empty() || this->wss_url_.empty()) {
      break;
    }
    ok = true;
  } while (false);
  cJSON_Delete(root);
  if (!ok) {
    ESP_LOGE(TAG, "join: missing client_id/wss_url");
  } else {
    ESP_LOGI(TAG, "signaling joined: room=%s client=%s initiator=%s", this->room_id_.c_str(),
             this->client_id_.c_str(), this->is_initiator_ ? "yes" : "no");
  }
  return ok;
}

bool ApprtcSignaling::connect() {
  // The AppRTC server checks the Origin header (403 without it); store it in a
  // member so it outlives esp_websocket_client_start.
  this->origin_ = "Origin: " + this->base_url_ + "\r\n";
  esp_websocket_client_config_t wcfg = {};
  wcfg.uri = this->wss_url_.c_str();
  wcfg.headers = this->origin_.c_str();
  wcfg.crt_bundle_attach = esp_crt_bundle_attach;
  wcfg.buffer_size = 20 * 1024;
  wcfg.network_timeout_ms = 10000;
  wcfg.reconnect_timeout_ms = 60 * 1000;
  esp_websocket_client_handle_t ws = esp_websocket_client_init(&wcfg);
  if (ws == nullptr) {
    ESP_LOGE(TAG, "websocket init failed");
    return false;
  }
  esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_, this);
  this->ws_ = ws;
  if (esp_websocket_client_start(ws) != ESP_OK) {
    ESP_LOGE(TAG, "websocket start failed");
    return false;
  }
  // Replay any messages already queued in the room (e.g. the peer's offer when
  // it joined first). Done now that the caller has opened the peer.
  for (auto &m : this->pending_msgs_) {
    this->handle_ws_text_(m.c_str(), static_cast<int>(m.size()));
  }
  this->pending_msgs_.clear();
  return true;
}

void ApprtcSignaling::on_ws_connected_() {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "cmd", "register");
  cJSON_AddStringToObject(j, "roomid", this->room_id_.c_str());
  cJSON_AddStringToObject(j, "clientid", this->client_id_.c_str());
  char *payload = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (payload != nullptr && this->ws_ != nullptr) {
    esp_websocket_client_send_text(static_cast<esp_websocket_client_handle_t>(this->ws_), payload,
                                   strlen(payload), portMAX_DELAY);
    free(payload);
  }
}

void ApprtcSignaling::handle_ws_text_(const char *text, int len) {
  if (text == nullptr || len <= 0) {
    return;
  }
  cJSON *outer = cJSON_Parse(text);
  if (outer == nullptr) {
    return;
  }
  // Messages arrive either directly, or wrapped as {"msg": "<json string>"}.
  cJSON *msg = cJSON_GetObjectItem(outer, "msg");
  cJSON *inner = outer;
  bool inner_owned = false;
  if (msg != nullptr && cJSON_IsString(msg) && msg->valuestring != nullptr) {
    inner = cJSON_Parse(msg->valuestring);
    inner_owned = true;
  }
  if (inner != nullptr) {
    cJSON *type = cJSON_GetObjectItem(inner, "type");
    if (type != nullptr && type->valuestring != nullptr) {
      const char *t = type->valuestring;
      if (strcmp(t, "offer") == 0 || strcmp(t, "answer") == 0) {
        cJSON *sdp = cJSON_GetObjectItem(inner, "sdp");
        if (sdp != nullptr && sdp->valuestring != nullptr && this->on_remote_sdp) {
          this->on_remote_sdp(sdp->valuestring);
        }
      } else if (strcmp(t, "candidate") == 0) {
        cJSON *cand = cJSON_GetObjectItem(inner, "candidate");
        if (cand != nullptr && cand->valuestring != nullptr && this->on_remote_candidate) {
          this->on_remote_candidate(cand->valuestring);
        }
      } else if (strcmp(t, "bye") == 0) {
        if (this->on_bye) {
          this->on_bye();
        }
      }
    }
    if (inner_owned) {
      cJSON_Delete(inner);
    }
  }
  cJSON_Delete(outer);
}

void ApprtcSignaling::send_sdp(const std::string &sdp) {
  if (this->message_url_.empty()) {
    return;
  }
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "type", this->is_initiator_ ? "offer" : "answer");
  cJSON_AddStringToObject(j, "sdp", sdp.c_str());
  char *payload = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (payload != nullptr) {
    ESP_LOGD(TAG, "send local %s (%u bytes)", this->is_initiator_ ? "offer" : "answer",
             (unsigned) sdp.size());
    http_post_(this->message_url_, payload);
    free(payload);
  }
}

void ApprtcSignaling::send_candidate(const std::string &candidate) {
  if (this->message_url_.empty()) {
    return;
  }
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "type", "candidate");
  cJSON_AddStringToObject(j, "candidate", candidate.c_str());
  char *payload = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (payload != nullptr) {
    http_post_(this->message_url_, payload);
    free(payload);
  }
}

void ApprtcSignaling::stop() {
  // Tell the room we are leaving so the server frees our slot; otherwise a
  // re-join to the SAME room sees it as full (2 clients) and mis-assigns the
  // offerer/answerer role. AppRTC: POST <base>/leave/<room>/<client>.
  if (!this->base_url_.empty() && !this->room_id_.empty() && !this->client_id_.empty()) {
    http_post_(this->base_url_ + "/leave/" + this->room_id_ + "/" + this->client_id_, "");
  }
  if (this->ws_ != nullptr) {
    esp_websocket_client_stop(static_cast<esp_websocket_client_handle_t>(this->ws_));
    esp_websocket_client_destroy(static_cast<esp_websocket_client_handle_t>(this->ws_));
    this->ws_ = nullptr;
  }
  this->client_id_.clear();
  this->pending_msgs_.clear();
}

}  // namespace webrtc
}  // namespace esphome

#endif  // USE_ESP32
