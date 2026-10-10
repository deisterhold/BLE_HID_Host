#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esp_gap_bt_api.h"
#include "esp_hidh_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace esphome::classic_bt_hid {

class ClassicBTHID final : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_mac_address(const std::string &address) { this->mac_address_ = address; }

  template<typename F> void add_on_connect_callback(F &&callback) {
    this->on_connect_callbacks_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_disconnect_callback(F &&callback) {
    this->on_disconnect_callbacks_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_connection_error_callback(F &&callback) {
    this->on_connection_error_callbacks_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_input_callback(F &&callback) {
    this->on_input_callbacks_.add(std::forward<F>(callback));
  }

 protected:
  enum class EventType : uint8_t {
    HOST_READY,
    HOST_ERROR,
    DISCOVERY_RESULT,
    DISCOVERY_STOPPED,
    OPEN,
    INPUT,
    CLOSE,
  };

  static constexpr size_t MAX_INPUT_REPORT_SIZE = 512;
  static constexpr size_t EVENT_QUEUE_LENGTH = 6;

  struct QueuedEvent {
    EventType type{EventType::HOST_READY};
    esp_bd_addr_t address{};
    esp_hidh_status_t hid_status{ESP_HIDH_OK};
    esp_hidh_connection_state_t connection_state{ESP_HIDH_CONN_STATE_UNKNOWN};
    uint8_t protocol_mode{0};
    uint16_t report_length{0};
    std::array<uint8_t, MAX_INPUT_REPORT_SIZE> report{};
  };

  static void gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void hid_callback_(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param);
  static bool is_peripheral_(const esp_bt_gap_cb_param_t *param);
  static std::string address_to_string_(const uint8_t *address);

  bool initialize_bluetooth_();
  bool enqueue_event_(const QueuedEvent &event);
  bool matches_address_filter_(const uint8_t *address) const;
  bool parse_address_filter_();
  void process_event_(const QueuedEvent &event);
  void start_scan_();
  void report_connection_error_(const uint8_t *address, uint8_t status);

  static ClassicBTHID *instance_;

  QueueHandle_t event_queue_{nullptr};
  std::string mac_address_;
  esp_bd_addr_t filter_address_{};
  esp_bd_addr_t active_address_{};
  bool has_address_filter_{false};
  bool host_ready_{false};
  bool scan_requested_{false};
  bool scan_running_{false};
  bool connect_after_discovery_{false};
  bool connection_pending_{false};
  bool device_connected_{false};
  uint32_t next_scan_attempt_ms_{0};
  std::atomic_bool candidate_claimed_{false};

  CallbackManager<void(const std::string &)> on_connect_callbacks_{};
  CallbackManager<void(const std::string &)> on_disconnect_callbacks_{};
  CallbackManager<void(const std::string &, uint8_t)> on_connection_error_callbacks_{};
  CallbackManager<void(const std::string &, uint8_t, const std::vector<uint8_t> &)> on_input_callbacks_{};
};

}  // namespace esphome::classic_bt_hid