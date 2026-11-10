#pragma once

#ifdef USE_ESP32

#include <deque>
#include <string>
#include <vector>

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif

namespace esphome::tarken {

namespace espbt = esphome::esp32_ble_tracker;

// One queued GATT write. The JSON text is Base64-encoded on send.
struct TarkenCommand {
  std::string json;
  uint32_t delay_after_ms;
  uint32_t delay_before_ms{0};  // minimum time after notify subscribe before sending
  bool auth{false};             // part of the login; may be sent before auth_ok
};

class Tarken : public Component, public ble_client::BLEClientNode, public espbt::ESPBTDeviceListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  /// Auto-discovery: find the Tarken by name ("tarken", any case) or service 0xABF0.
  bool parse_device(const espbt::ESPBTDevice &device) override;
  /// Forget the stored MAC and discover again.
  void forget_device();

  // --- Actions (call from lambdas / buttons) ---
  /// Queue one or more {"window":N} commands. They are sent in order, each after the
  /// previous write response, with command_delay between them.
  void send_window(const std::vector<int> &values);
  void send_window(int value) { this->send_window(std::vector<int>{value}); }
  /// Queue a raw JSON command (for example {"ping":1}).
  void send_raw(const std::string &json);
  /// Connect, collect notifications, disconnect.
  void request_poll();
  /// Send {"pair":1}. Works only in the ~60 s window after the Tarken powers on.
  void request_pair();

  // --- Config ---
  void set_idle_timeout(uint32_t ms) { this->idle_timeout_ms_ = ms; }
  void set_session_timeout(uint32_t ms) { this->session_timeout_ms_ = ms; }
  void set_max_connected_time(uint32_t ms) { this->max_connected_ms_ = ms; }
  void set_command_hold_time(uint32_t ms) { this->command_hold_ms_ = ms; }
  void set_status_interval(uint32_t ms) { this->status_interval_ms_ = ms; }
  void set_command_delay(uint32_t ms) { this->command_delay_ms_ = ms; }
  void set_default_poll_interval(uint32_t s) { this->default_poll_interval_s_ = s; }
  void set_send_ping(bool v) { this->send_ping_ = v; }
  void set_base64(bool v) { this->base64_ = v; }
  void set_fixed_address(uint64_t mac) { this->fixed_mac_ = mac; }
#ifdef USE_NUMBER
  void set_poll_interval_number(number::Number *n) { this->poll_interval_number_ = n; }
#endif

#ifdef USE_SENSOR
  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_ = s; }
  void set_humidity_sensor(sensor::Sensor *s) { this->humidity_ = s; }
  void set_co2_sensor(sensor::Sensor *s) { this->co2_ = s; }
  void set_tvoc_sensor(sensor::Sensor *s) { this->tvoc_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { this->battery_ = s; }
  void set_uptime_sensor(sensor::Sensor *s) { this->uptime_ = s; }
  void set_window_raw_sensor(sensor::Sensor *s) { this->window_raw_ = s; }
  void set_active_sensor(sensor::Sensor *s) { this->active_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->connected_ = s; }
  void set_fully_open_mode_binary_sensor(binary_sensor::BinarySensor *s) { this->open_mode_ = s; }
  void set_spindle_binary_sensor(binary_sensor::BinarySensor *s) { this->spindle_ = s; }
  void set_buttons_binary_sensor(binary_sensor::BinarySensor *s) { this->buttons_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_firmware_text_sensor(text_sensor::TextSensor *s) { this->firmware_ = s; }
  void set_window_state_text_sensor(text_sensor::TextSensor *s) { this->window_state_ = s; }
  void set_auto_text_sensor(text_sensor::TextSensor *s) { this->auto_ = s; }
  void set_timer_text_sensor(text_sensor::TextSensor *s) { this->timer_ = s; }
  void set_last_message_text_sensor(text_sensor::TextSensor *s) { this->last_message_ = s; }
  void set_mac_text_sensor(text_sensor::TextSensor *s) { this->mac_sensor_ = s; }
#endif

 
  /// Pairing key (uid0..3), from {"pair":1} in the pairing window or from config.
  void set_pair_uid(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    this->uid_[0] = a; this->uid_[1] = b; this->uid_[2] = c; this->uid_[3] = d;
    this->uid_mask_ = (a | b | c | d) != 0 ? 0xF : 0;  // all zero = no key
  }

 protected:
  void start_session_();
  void handle_auth_field_(const char *key, const char *val);
  void send_auth_response_();
  uint32_t uid_[4]{};
  uint8_t uid_mask_{0};
  uint32_t new_uid_[4]{};
  uint8_t new_uid_mask_{0};
  uint32_t nonce_[4]{};
  uint8_t nonce_mask_{0};
  bool authenticated_{false};
  bool pairing_{false};  // Pair session: no login (pairing works without it)
  bool session_has_commands_{false};  // window/raw commands in this session: keep link for state updates
  ESPPreferenceObject uid_pref_;
  void end_session_(const char *reason, bool disconnect = true);
  void write_next_();
  void handle_notify_(const uint8_t *data, uint16_t len);
  void handle_json_(const std::string &text);
  void set_connected_(bool c);
  uint32_t poll_interval_ms_() const;
  void publish_mac_();

  uint64_t mac_{0};
  uint64_t fixed_mac_{0};
  ESPPreferenceObject mac_pref_;

  // Session state
  bool session_active_{false};
  bool ready_{false};    // notifications registered, writes allowed
  bool writing_{false};  // a write is waiting for its response
  uint32_t session_start_{0};
  uint32_t last_activity_{0};
  uint32_t ready_since_{0};      // when notifications were enabled (set only by GATT events)
  uint32_t last_command_at_{0};  // when a command was queued during the session
  uint32_t write_started_{0};
  uint32_t delay_after_inflight_{0};
  void drop_auth_commands_();
  uint32_t last_disconnect_{0};
  uint32_t next_write_at_{0};
  uint32_t last_poll_{0};
  bool first_poll_done_{false};
  uint8_t reconnects_{0};
  uint8_t write_errors_{0};
  uint16_t char_handle_{0};
  std::deque<TarkenCommand> queue_;

  // Config
  uint32_t idle_timeout_ms_{10000};
  uint32_t session_timeout_ms_{60000};
  uint32_t max_connected_ms_{30000};
  uint32_t command_hold_ms_{120000};  // hold the link after a window command (movements take ~1 min)
  bool moving_{false};               // window command sent in this session
  uint32_t last_window_ack_{0};      // write response of the last window command
  bool inflight_is_window_{false};
  bool tracking_{false};          // polling every status_interval until the target window state
  uint32_t track_until_{0};
  uint32_t next_track_poll_{0};
  uint32_t status_interval_ms_{10000};  // re-login (status refresh) interval while a movement runs
  int expected_window_{-1};       // window state that ends the command session (-1 = any change)
  int window_before_{-1};         // window state before the command
  int last_window_{-1};
  int last_active_{-1};  // meaning unknown (1/2 seen); logged only
  uint32_t command_delay_ms_{500};
  uint32_t default_poll_interval_s_{300};
  bool send_ping_{true};
  bool base64_{false};  // wire format is plain JSON; Base64 only as an option
#ifdef USE_NUMBER
  number::Number *poll_interval_number_{nullptr};
#endif

#ifdef USE_SENSOR
  sensor::Sensor *temperature_{nullptr};
  sensor::Sensor *humidity_{nullptr};
  sensor::Sensor *co2_{nullptr};
  sensor::Sensor *tvoc_{nullptr};
  sensor::Sensor *battery_{nullptr};
  sensor::Sensor *uptime_{nullptr};
  sensor::Sensor *window_raw_{nullptr};
  sensor::Sensor *active_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_{nullptr};
  binary_sensor::BinarySensor *open_mode_{nullptr};
  binary_sensor::BinarySensor *spindle_{nullptr};
  binary_sensor::BinarySensor *buttons_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *firmware_{nullptr};
  text_sensor::TextSensor *window_state_{nullptr};
  text_sensor::TextSensor *auto_{nullptr};
  text_sensor::TextSensor *timer_{nullptr};
  text_sensor::TextSensor *last_message_{nullptr};
  text_sensor::TextSensor *mac_sensor_{nullptr};
#endif
};

}  // namespace esphome::tarken

#endif  // USE_ESP32
