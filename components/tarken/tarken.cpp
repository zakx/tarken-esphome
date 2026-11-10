#include "tarken.h"

#ifdef USE_ESP32

#include <algorithm>
#include <array>
#include <cmath>

#include "esphome/components/json/json_util.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esphome/components/sha256/sha256.h"

namespace esphome::tarken {

static const char *const TAG = "tarken";

static const uint16_t SERVICE_UUID = 0xABF0;
static const uint16_t CHAR_UUID_FALLBACK = 0xABF1;
// The app replaces unformatted printf floats with this value. We treat it as "no value".
static const float INVALID_VALUE = -69.0f;

static const char *window_state_str(int v) {
  switch (v) {
    case 0:
      return "Closed";
    case 1:
      return "Open (selected mode)";
    case 2:
      return "Released";
    case 3:
    case 4:
    case 5:
    case 6:
      return "Jogging";
    case 7:
      return "Micro tilt mode selected";
    case 8:
      return "Tilt mode selected";
    case 9:
      return "Auto cancelled";
    default:
      return "Unknown";
  }
}

void Tarken::setup() {
  // We connect on demand only.
  this->parent()->set_auto_connect(false);
  this->set_connected_(false);
  this->mac_pref_ = global_preferences->make_preference<uint64_t>(fnv1_hash("tarken_mac"), true);
  if (this->fixed_mac_ != 0) {
    this->mac_ = this->fixed_mac_;
  } else {
    uint64_t saved = 0;
    if (this->mac_pref_.load(&saved) && saved != 0)
      this->mac_ = saved;
  }
  if (this->mac_ != 0)
    this->parent()->set_address(this->mac_);
  this->publish_mac_();
  this->uid_pref_ = global_preferences->make_preference<std::array<uint32_t, 4>>(fnv1_hash("tarken_uid"), true);
  std::array<uint32_t, 4> saved_uid{};
  if (this->uid_pref_.load(&saved_uid) && (saved_uid[0] | saved_uid[1] | saved_uid[2] | saved_uid[3]) != 0) {
    for (int i = 0; i < 4; i++)
      this->uid_[i] = saved_uid[i];
    if (this->uid_mask_ == 0xF)
      ESP_LOGI(TAG, "Using the stored pair key (from Pair); config pair_uid is used only when none is stored");
    this->uid_mask_ = 0xF;
    ESP_LOGI(TAG, "Loaded stored pair key");
  }
  if (this->uid_mask_ != 0xF)
    ESP_LOGW(TAG, "No pair key: power-cycle the Tarken and press 'Pair' within 60 s");
}

void Tarken::publish_mac_() {
#ifdef USE_TEXT_SENSOR
  if (this->mac_sensor_ == nullptr)
    return;
  if (this->mac_ == 0) {
    this->mac_sensor_->publish_state("searching...");
    return;
  }
  char buf[18];
  uint8_t b[6];
  for (int i = 0; i < 6; i++)
    b[i] = (this->mac_ >> (40 - 8 * i)) & 0xFF;
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  this->mac_sensor_->publish_state(buf);
#endif
}

bool Tarken::parse_device(const espbt::ESPBTDevice &device) {
  if (this->mac_ != 0)
    return false;
  std::string name = device.get_name();
  for (auto &ch : name)
    ch = tolower(ch);
  bool hit = name.find("tarken") != std::string::npos;
  for (auto &u : device.get_service_uuids())
    if (u == espbt::ESPBTUUID::from_uint16(SERVICE_UUID))
      hit = true;
  if (!hit)
    return false;
  this->mac_ = device.address_uint64();
  this->mac_pref_.save(&this->mac_);
  global_preferences->sync();
  this->parent()->set_address(this->mac_);
  char addr[espbt::ESPBTDevice::MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  ESP_LOGI(TAG, "Discovered Tarken: %s name='%s' rssi=%d (saved)", device.address_str_to(addr),
           device.get_name().c_str(), device.get_rssi());
  this->publish_mac_();
  // Poll at once, do not wait for the next interval.
  this->defer([this]() { this->request_poll(); });
  return true;
}

void Tarken::forget_device() {
  this->end_session_("forget device");
  this->mac_ = 0;
  this->mac_pref_.save(&this->mac_);
  global_preferences->sync();
  ESP_LOGI(TAG, "Stored Tarken MAC removed, searching again");
  this->publish_mac_();
}

void Tarken::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Tarken WM:\n"
                "  Idle timeout: %" PRIu32 " ms\n"
                "  Session timeout: %" PRIu32 " ms\n"
                "  Command delay: %" PRIu32 " ms\n"
                "  Poll interval: %" PRIu32 " s\n"
                "  Send ping on poll: %s",
                this->idle_timeout_ms_, this->session_timeout_ms_, this->command_delay_ms_,
                this->poll_interval_ms_() / 1000, YESNO(this->send_ping_));
}

uint32_t Tarken::poll_interval_ms_() const {
#ifdef USE_NUMBER
  if (this->poll_interval_number_ != nullptr && this->poll_interval_number_->has_state() &&
      !std::isnan(this->poll_interval_number_->state)) {
    float s = this->poll_interval_number_->state;
    return s <= 0 ? 0 : static_cast<uint32_t>(s) * 1000;
  }
#endif
  return this->default_poll_interval_s_ * 1000;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void Tarken::send_window(const std::vector<int> &values) {
  for (int v : values) {
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"window\":%d}", v);
    this->queue_.push_back({buf, this->command_delay_ms_});
  }
  ESP_LOGI(TAG, "Queued %u window command(s)", (unsigned) values.size());
  this->session_has_commands_ = true;
  this->moving_ = true;
  int last = values.empty() ? -1 : values.back();
  // Show the commanded position at once; the next status confirms it.
  for (int v : values) {
    if (v == 7)
      this->last_open_ = 2;
    else if (v == 8)
      this->last_open_ = 1;
  }
  if (last == 0 || last == 1 || last == 2) {
    this->last_command_ = last;
    this->last_window_ = last == 0 ? 0 : 1;
  }
  // 0 = closed; 1 (open in the selected mode) and 2 (open fully) both report window=1.
  this->expected_window_ = last == 0 ? 0 : (last == 1 || last == 2) ? 1 : -1;
  this->window_before_ = this->last_window_;
  this->start_session_();
}

void Tarken::send_raw(const std::string &json) {
  this->session_has_commands_ = true;
  this->queue_.push_back({json, this->command_delay_ms_});
  this->start_session_();
}

void Tarken::request_pair() {
  ESP_LOGI(TAG, "Pair requested");
  this->new_uid_mask_ = 0;  // collect a new key; the old one stays until it is complete
  this->pairing_ = true;
  TarkenCommand cmd{"{\"pair\":1}", 0};
  cmd.auth = true;
  this->queue_.push_back(cmd);
  this->start_session_();
}

std::string Tarken::position() const {
  if (this->last_window_ == 0)
    return "Closed";
  if (this->last_window_ != 1)
    return "";
  // The status does not tell "open fully" from "full tilt"; use the last command.
  if (this->last_command_ == 2)
    return "Open fully";
  if (this->last_open_ == 2)
    return "Micro tilt";
  if (this->last_open_ == 1)
    return "Full tilt";
  return "";
}

void Tarken::request_poll() {
  ESP_LOGI(TAG, "Poll requested");
  this->first_poll_done_ = true;
  this->last_poll_ = millis();
  // The ping goes ~3 s after subscribe, so the log shows what the Tarken sends by itself first.
  if (this->send_ping_)
    this->queue_.push_back({"{\"ping\":1}", 0, 3000});
  this->start_session_();
}

// ---------------------------------------------------------------------------
// Session handling
// ---------------------------------------------------------------------------

void Tarken::start_session_() {
  this->last_activity_ = millis();
  if (this->session_active_) {
    // Already connecting or connected: queued commands will be sent.
    this->last_command_at_ = millis();
    if (this->ready_ && !this->writing_)
      this->write_next_();
    return;
  }
  if (this->mac_ == 0) {
    ESP_LOGW(TAG, "Tarken not found yet (no advertisement seen), cannot connect");
    this->queue_.clear();
    return;
  }
  this->parent()->set_address(this->mac_);
  ESP_LOGD(TAG, "t=%" PRIu32 " Starting BLE session (last disconnect %" PRIu32 " s ago, client state %s)",
           millis(), (millis() - this->last_disconnect_) / 1000, espbt::client_state_to_string(this->parent()->state()));
  this->session_active_ = true;
  this->session_start_ = millis();
  this->last_command_at_ = millis();
  this->reconnects_ = 0;
  this->write_errors_ = 0;
  // With auto_connect on, the ble_client connects as soon as the tracker sees the device.
  this->parent()->set_auto_connect(true);
}

void Tarken::end_session_(const char *reason, bool disconnect) {
  ESP_LOGI(TAG, "t=%" PRIu32 " Ending BLE session (%s)", millis(), reason);
  // The Tarken sends its full status only after a new login on a new connection, so a movement
  // is tracked with short polls every status_interval until the window reports the target state.
  const uint32_t t = millis();
  if (this->moving_) {
    this->tracking_ = true;
    this->track_until_ = t + this->command_hold_ms_;
    this->next_track_poll_ = t + this->status_interval_ms_;
    ESP_LOGI(TAG, "Tracking the movement: poll every %" PRIu32 " s (expect window=%d)", this->status_interval_ms_ / 1000,
             this->expected_window_);
  } else if (this->tracking_) {
    bool state_ok = this->expected_window_ >= 0 ? this->last_window_ == this->expected_window_
                                                : this->last_window_ != this->window_before_;
    bool done = state_ok;  // note: "window" reports the commanded state, not the end of the movement
    if (done) {
      this->tracking_ = false;
      ESP_LOGI(TAG, "Movement done: window=%d active=%d", this->last_window_, this->last_active_);
    } else if ((int32_t) (t - this->track_until_) >= 0) {
      this->tracking_ = false;
      ESP_LOGW(TAG, "Window did not reach state %d in time (now %d)", this->expected_window_, this->last_window_);
    } else {
      this->next_track_poll_ = t + this->status_interval_ms_;
    }
  }
  this->session_active_ = false;
  this->session_has_commands_ = false;
  this->pairing_ = false;
  this->moving_ = false;
  this->last_window_ack_ = 0;
  this->ready_ = false;
  this->writing_ = false;
  this->nonce_mask_ = 0;
  if (!this->queue_.empty()) {
    ESP_LOGW(TAG, "Dropping %u unsent command(s)", (unsigned) this->queue_.size());
    this->queue_.clear();
  }
  this->parent()->set_auto_connect(false);
  if (!disconnect)
    return;
  auto st = this->parent()->state();
  if (st != espbt::ClientState::IDLE && st != espbt::ClientState::DISCONNECTING)
    this->parent()->disconnect();
}

void Tarken::loop() {
  const uint32_t now = millis();

  // Movement tracking polls
  if (this->tracking_ && !this->session_active_ && (int32_t) (now - this->next_track_poll_) >= 0) {
    this->request_poll();
    return;
  }

  // Periodic poll
  uint32_t interval = this->poll_interval_ms_();
  if (interval > 0 && !this->session_active_) {
    // First poll shortly after boot, then every interval.
    bool due = !this->first_poll_done_ ? (now > 15000) : ((int32_t) (now - this->last_poll_) >= (int32_t) interval);
    if (due) {
      this->first_poll_done_ = true;
      this->last_poll_ = now;
      this->request_poll();
      return;  // 'now' is older than session_start_; check the session next loop
    }
  }

  if (!this->session_active_)
    return;

  if (!this->ready_) {
    static uint32_t last_dbg = 0;
    if (now - last_dbg > 10000) {
      last_dbg = now;
      ESP_LOGD(TAG, "Waiting for connection, client state %s", espbt::client_state_to_string(this->parent()->state()));
    }
    if ((int32_t) (now - this->session_start_) >= (int32_t) this->session_timeout_ms_)
      this->end_session_("timeout: device not found or did not connect");
    return;
  }

  if (this->writing_ && (int32_t) (now - this->write_started_) >= 5000) {
    this->end_session_("write timeout");
    return;
  }
  // Hard limit, whatever the state: never hold the link much longer than allowed.
  uint32_t hold = this->moving_ ? this->command_hold_ms_ : this->max_connected_ms_;
  if ((int32_t) (now - this->ready_since_) >= (int32_t) (hold + 30000)) {
    this->end_session_("hard session limit");
    return;
  }
  if (!this->authenticated_ && this->uid_mask_ == 0xF && !this->pairing_ &&
      (int32_t) (now - this->ready_since_) >= 10000) {
    this->end_session_("auth timeout");
    return;
  }

  if (!this->writing_ && !this->queue_.empty() && (int32_t) (now - this->next_write_at_) >= 0) {
    this->write_next_();
    return;
  }

  if (!this->writing_ && this->queue_.empty() && this->moving_) {
    // Command sent and acknowledged: disconnect, then track with short polls.
    if (this->last_window_ack_ != 0 && (int32_t) (now - this->last_window_ack_) >= 1000)
      this->end_session_("command sent");
    return;
  }
  if (!this->writing_ && this->queue_.empty()) {
    if ((int32_t) (now - this->last_activity_) >= (int32_t) this->idle_timeout_ms_)
      this->end_session_("idle");
    else if ((int32_t) (now - std::max(this->ready_since_, this->last_command_at_)) >= (int32_t) this->max_connected_ms_)
      this->end_session_("max connected time");
  }
}

void Tarken::write_next_() {
  if (this->queue_.empty() || !this->ready_ || this->writing_)
    return;
  if ((int32_t) (millis() - this->next_write_at_) < 0)
    return;  // loop() sends it when the delay has passed
  TarkenCommand &cmd = this->queue_.front();
  if (!this->authenticated_ && !cmd.auth && this->uid_mask_ == 0xF && !this->pairing_)
    return;  // wait for auth_ok
  std::string payload =
      this->base64_ ? base64_encode(reinterpret_cast<const uint8_t *>(cmd.json.data()), cmd.json.size()) : cmd.json;
  ESP_LOGI(TAG, "Write %s", payload.c_str());
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->char_handle_, payload.size(), (uint8_t *) payload.data(),
                                      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char failed: %d", err);
    if (++this->write_errors_ >= 3) {
      this->end_session_("too many write errors");
      return;
    }
    this->next_write_at_ = millis() + 1000;  // try again later
    return;
  }
  this->delay_after_inflight_ = cmd.delay_after_ms;
  this->inflight_is_window_ = cmd.json.rfind("{\"window\"", 0) == 0;
  this->queue_.pop_front();  // in flight now; replies queued with push_front cannot jump ahead of it
  this->writing_ = true;
  this->write_started_ = millis();
  this->last_activity_ = millis();
}

void Tarken::drop_auth_commands_() {
  // Login messages belong to one connection (its nonces); never resend them after a reconnect.
  for (auto it = this->queue_.begin(); it != this->queue_.end();) {
    if (it->auth && it->json.find("\"pair\"") == std::string::npos)
      it = this->queue_.erase(it);
    else
      ++it;
  }
}

void Tarken::set_connected_(bool c) {
#ifdef USE_BINARY_SENSOR
  if (this->connected_ != nullptr)
    this->connected_->publish_state(c);
#endif
}

// ---------------------------------------------------------------------------
// GATT events
// ---------------------------------------------------------------------------

void Tarken::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Connected to %s after %" PRIu32 " ms", this->parent()->address_str(),
                 millis() - this->session_start_);
        this->set_connected_(true);
        // Only connect once per session: do not reconnect when the session ends.
        if (!this->session_active_) {
          this->parent()->set_auto_connect(false);
          this->parent()->disconnect();
        }
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      if (this->ready_ || this->session_active_)
        ESP_LOGI(TAG, "Disconnected");
      if (this->session_active_ && (this->ready_ || this->writing_)) {
        // Link lost during an active session.
        if (this->queue_.empty()) {
          this->end_session_("link lost, nothing pending", false);
        } else if (this->reconnects_ < 1) {
          this->reconnects_++;
          this->drop_auth_commands_();  // the in-flight command (if any) is dropped, not resent
          this->session_start_ = millis();  // fresh timeout for the reconnect
          ESP_LOGW(TAG, "Link lost with %u command(s) pending, reconnecting once", (unsigned) this->queue_.size());
        } else {
          this->end_session_("link lost again", false);
        }
      }
      this->last_disconnect_ = millis();
      this->ready_ = false;
      this->authenticated_ = false;
      this->writing_ = false;
          this->nonce_mask_ = 0;
      this->char_handle_ = 0;
      this->node_state = espbt::ClientState::IDLE;
      this->set_connected_(false);
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->char_handle_ = 0;
      auto *svc = this->parent()->get_service(SERVICE_UUID);
      if (svc == nullptr) {
        ESP_LOGE(TAG, "Service 0x%04X not found", SERVICE_UUID);
        this->end_session_("service missing");
        break;
      }
      if (!svc->parsed)
        svc->parse_characteristics();
      esp32_ble_client::BLECharacteristic *chr = nullptr;
      for (auto *c : svc->characteristics) {
        char ub[esp32_ble::UUID_STR_LEN];
        ESP_LOGD(TAG, "  char %s handle=0x%04X props=0x%02X", c->uuid.to_str(ub), c->handle, c->properties);
        if (chr == nullptr && (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE) &&
            (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY))
          chr = c;
      }
      if (chr == nullptr)
        chr = svc->get_characteristic(CHAR_UUID_FALLBACK);
      if (chr == nullptr) {
        ESP_LOGE(TAG, "No write+notify characteristic in service 0x%04X", SERVICE_UUID);
        this->end_session_("characteristic missing");
        break;
      }
      this->char_handle_ = chr->handle;
      ESP_LOGI(TAG, "Using characteristic handle 0x%04X", this->char_handle_);
      auto err = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
                                                   chr->handle);
      if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed: %d", err);
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle != this->char_handle_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Register for notify failed: %d", param->reg_for_notify.status);
        break;
      }
      ESP_LOGI(TAG, "Notifications enabled");
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->ready_ = true;
      this->authenticated_ = false;
      this->nonce_mask_ = 0;
      if (this->uid_mask_ == 0xF && !this->pairing_) {
        TarkenCommand ch{"{\"challenge\":1}", 0};
        ch.auth = true;
        this->queue_.push_front(ch);
      }
      this->writing_ = false;
      this->last_activity_ = millis();
      this->ready_since_ = millis();
      // Give the CCCD write a moment before the first command.
      this->next_write_at_ = millis() + std::max<uint32_t>(300, this->queue_.empty() ? 0 : this->queue_.front().delay_before_ms);
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.handle != this->char_handle_)
        break;
      if (param->write.status != ESP_GATT_OK) {
        // Do not continue a sequence (e.g. 8 then 1) after a failed step.
        ESP_LOGW(TAG, "Write failed: status=%d, dropping the rest of the queue", param->write.status);
        this->writing_ = false;
        this->end_session_("write failed");
        break;
      }
      ESP_LOGD(TAG, "Write acknowledged");
      if (this->inflight_is_window_)
        this->last_window_ack_ = millis();
      this->next_write_at_ = millis() + this->delay_after_inflight_;
      this->writing_ = false;
      this->last_activity_ = millis();
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle != this->char_handle_)
        break;
      this->last_activity_ = millis();
      this->handle_notify_(param->notify.value, param->notify.value_len);
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Notification parsing
// ---------------------------------------------------------------------------

void Tarken::handle_notify_(const uint8_t *data, uint16_t len) {
  if (len == 0)
    return;
  std::string text(reinterpret_cast<const char *>(data), len);
  // Plain JSON starts with '{'. Otherwise try Base64.
  if (!text.empty() && text[0] != '{') {
    std::vector<uint8_t> raw = base64_decode(text);
    if (!raw.empty() && raw[0] == '{')
      text.assign(raw.begin(), raw.end());
  }
  ESP_LOGD(TAG, "Notify (%u bytes): %s", len, text.c_str());

  // Replace unformatted printf tokens (%f, %.1f, %1.1f, %d, %u) with -69 (treated as "no value").
  std::string clean;
  clean.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '%') {
      size_t j = i + 1;
      while (j < text.size() && (isdigit((unsigned char) text[j]) || text[j] == '.'))
        j++;
      if (j < text.size() && (text[j] == 'f' || text[j] == 'd' || text[j] == 'u')) {
        clean += "-69";
        i = j;
        continue;
      }
    }
    clean += text[i];
  }

#ifdef USE_TEXT_SENSOR
  if (this->last_message_ != nullptr)
    this->last_message_->publish_state(clean);
#endif
  this->handle_json_(clean);
}

void Tarken::handle_auth_field_(const char *key, const char *val) {
  bool is_uid = strncmp(key, "uid", 3) == 0, is_nonce = strncmp(key, "nonce", 5) == 0;
  if (!is_uid && !is_nonce)
    return;
  int idx = key[strlen(key) - 1] - '0';
  if (idx < 0 || idx > 3)
    return;
  uint32_t v = strtoul(val, nullptr, 16);
  if (is_uid) {
    this->new_uid_[idx] = v;
    this->new_uid_mask_ |= 1 << idx;
    if (this->new_uid_mask_ == 0xF) {
      this->new_uid_mask_ = 0;
      for (int i = 0; i < 4; i++)
        this->uid_[i] = this->new_uid_[i];
      this->uid_mask_ = 0xF;
      ESP_LOGI(TAG, "Pair key received and stored: %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32, this->uid_[0], this->uid_[1], this->uid_[2],
               this->uid_[3]);
      std::array<uint32_t, 4> u{this->uid_[0], this->uid_[1], this->uid_[2], this->uid_[3]};
      this->uid_pref_.save(&u);
      global_preferences->sync();
    }
  } else {
    this->nonce_[idx] = v;
    this->nonce_mask_ |= 1 << idx;
    if (this->nonce_mask_ == 0xF && this->uid_mask_ == 0xF)
      this->send_auth_response_();
  }
}

void Tarken::send_auth_response_() {
  // resp = sha256(nonce0..3 || uid0..3) (16 + 16 bytes, big-endian words); the first 16 bytes are
  // sent as lowercase hex in four separate writes {"resp0":"xxxxxxxx"} .. {"resp3":...}.
  this->nonce_mask_ = 0;
  uint8_t msg[32];
  for (int i = 0; i < 4; i++)
    for (int b = 0; b < 4; b++) {
      msg[i * 4 + b] = (this->nonce_[i] >> (24 - 8 * b)) & 0xFF;
      msg[16 + i * 4 + b] = (this->uid_[i] >> (24 - 8 * b)) & 0xFF;
    }
  sha256::SHA256 h;
  h.init();
  h.add(msg, sizeof(msg));
  h.calculate();
  uint8_t d[32];
  h.get_bytes(d);
  char buf[32];
  for (int i = 3; i >= 0; i--) {
    snprintf(buf, sizeof(buf), "{\"resp%d\":\"%02x%02x%02x%02x\"}", i, d[i * 4], d[i * 4 + 1], d[i * 4 + 2],
             d[i * 4 + 3]);
    TarkenCommand rc{buf, 100};
    rc.auth = true;
    this->queue_.push_front(rc);
  }
  ESP_LOGD(TAG, "Login response queued");
}

void Tarken::handle_json_(const std::string &text) {
  bool ok = json::parse_json(text, [this](JsonObject root) -> bool {
    // In a session with window commands, the status sent after the login is from before
    // the command: keep the commanded state. The follow-up polls report the new state.
    if (this->moving_) {
      root.remove("window");
      root.remove("open");
    }
#ifdef USE_SENSOR
    auto pub = [](sensor::Sensor *s, JsonVariant v) {
      if (s == nullptr || v.isNull())
        return;
      float f = v.as<float>();
      if (f == INVALID_VALUE)
        return;
      s->publish_state(f);
    };
    pub(this->temperature_, root["temp"]);
    pub(this->humidity_, root["hum"]);
    pub(this->co2_, root["co2"]);
    pub(this->tvoc_, root["tvoc"]);
    pub(this->battery_, root["bat"]);
    pub(this->uptime_, root["up"]);
    pub(this->window_raw_, root["window"]);
    pub(this->active_, root["active"]);
#endif
#ifdef USE_BINARY_SENSOR
    auto pubb = [](binary_sensor::BinarySensor *s, JsonVariant v) {
      if (s == nullptr || v.isNull())
        return;
      s->publish_state(v.as<int>() != 0);
    };
    pubb(this->open_mode_, root["open"]);
    pubb(this->spindle_, root["spindle"]);
    pubb(this->buttons_, root["buttons"]);
#endif
#ifdef USE_TEXT_SENSOR
    if (this->firmware_ != nullptr && root["fw"].is<const char *>())
      this->firmware_->publish_state(root["fw"].as<const char *>());
    if (this->window_state_ != nullptr && !root["window"].isNull())
      this->window_state_->publish_state(window_state_str(root["window"].as<int>()));
    auto pubarr = [](text_sensor::TextSensor *s, JsonVariant v) {
      if (s == nullptr || v.isNull())
        return;
      std::string out;
      serializeJson(v, out);
      s->publish_state(out);
    };
    pubarr(this->auto_, root["auto"]);
    pubarr(this->timer_, root["timer"]);
#endif
    for (JsonPair kv : root) {
      if (kv.value().is<const char *>())
        this->handle_auth_field_(kv.key().c_str(), kv.value().as<const char *>());
    }
    if (!root["window"].isNull())
      this->last_window_ = root["window"].as<int>();
    if (!root["active"].isNull())
      this->last_active_ = root["active"].as<int>();
    if (!root["open"].isNull())
      this->last_open_ = root["open"].as<int>();
    // A close from the Tarken's own buttons or automation: forget "open fully".
    if (!root["window"].isNull() && root["window"].as<int>() == 0)
      this->last_command_ = 0;
    // "sched_en" is the last message of the status dump after auth_ok.
    if (!root["sched_en"].isNull() && this->authenticated_ && !this->session_has_commands_ &&
        this->queue_.empty() && !this->writing_) {
      this->defer([this]() {
        if (this->session_active_ && !this->session_has_commands_ && this->queue_.empty())
          this->end_session_("status received");
      });
    }
    if (root["ack"].is<const char *>()) {
      const char *ack = root["ack"].as<const char *>();
      ESP_LOGI(TAG, "ack: %s", ack);
      if (strcmp(ack, "auth_ok") == 0) {
        this->authenticated_ = true;
        this->last_command_at_ = millis();
      } else if (strcmp(ack, "auth_fail") == 0) {
        this->defer([this]() { this->end_session_("auth failed (wrong pair key? press Pair after power-cycling)"); });
      } else if (strcmp(ack, "pair_closed") == 0) {
        ESP_LOGW(TAG, "Pairing window closed: power-cycle the Tarken and press Pair within 60 s");
      }
    }
    return true;
  });
  if (!ok)
    ESP_LOGW(TAG, "Could not parse JSON: %s", text.c_str());
}

}  // namespace esphome::tarken

#endif  // USE_ESP32
