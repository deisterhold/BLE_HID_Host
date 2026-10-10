#include "classic_bt_hid.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace esphome::classic_bt_hid {

static const char *const TAG = "classic_bt_hid";
static constexpr uint8_t INQUIRY_LENGTH = 8;

ClassicBTHID *ClassicBTHID::instance_ = nullptr;

void ClassicBTHID::setup() {
  if (instance_ != nullptr && instance_ != this) {
    ESP_LOGE(TAG, "Only one Classic HID host instance is supported");
    this->mark_failed();
    return;
  }
  instance_ = this;

  this->event_queue_ = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(QueuedEvent));
  if (this->event_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create HID event queue");
    this->mark_failed();
    return;
  }

  if (!this->parse_address_filter_() || !this->initialize_bluetooth_()) {
    this->mark_failed();
  }
}

void ClassicBTHID::loop() {
  QueuedEvent event{};
  while (this->event_queue_ != nullptr && xQueueReceive(this->event_queue_, &event, 0) == pdTRUE) {
    this->process_event_(event);
  }

  if (!this->host_ready_ || !this->scan_requested_ || this->scan_running_ ||
      this->connection_pending_ || this->device_connected_ ||
      static_cast<int32_t>(millis() - this->next_scan_attempt_ms_) < 0) {
    return;
  }
  this->start_scan_();
}

void ClassicBTHID::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth Classic HID host:");
  ESP_LOGCONFIG(TAG, "  Address filter: %s", this->mac_address_.empty() ? "none" : this->mac_address_.c_str());
  ESP_LOGCONFIG(TAG, "  Raw HID input events: enabled");
}

bool ClassicBTHID::parse_address_filter_() {
  if (this->mac_address_.empty()) {
    this->has_address_filter_ = false;
    return true;
  }

  unsigned int octets[ESP_BD_ADDR_LEN]{};
  int consumed = 0;
  const int parsed = std::sscanf(this->mac_address_.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%n",
                                 &octets[0], &octets[1], &octets[2], &octets[3], &octets[4],
                                 &octets[5], &consumed);
  if (parsed != ESP_BD_ADDR_LEN || consumed != 17 || this->mac_address_.size() != 17) {
    ESP_LOGE(TAG, "mac_address must have the form 12:34:56:78:9A:BC");
    return false;
  }
  for (size_t index = 0; index < ESP_BD_ADDR_LEN; ++index) {
    if (octets[index] > 0xFF) {
      ESP_LOGE(TAG, "mac_address contains an invalid octet");
      return false;
    }
    this->filter_address_[index] = static_cast<uint8_t>(octets[index]);
  }
  this->has_address_filter_ = true;
  return true;
}

bool ClassicBTHID::initialize_bluetooth_() {
  esp_err_t result = nvs_flash_init();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(result));
    return false;
  }

  esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  controller_config.mode = ESP_BT_MODE_CLASSIC_BT;
  result = esp_bt_controller_init(&controller_config);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_bluedroid_init();
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_bluedroid_enable();
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_bt_gap_register_callback(gap_callback_);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "GAP callback registration failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_bt_hid_host_register_callback(hid_callback_);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "HID callback registration failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_bt_hid_host_init();
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "HID host init failed: %s", esp_err_to_name(result));
    return false;
  }
  return true;
}

bool ClassicBTHID::enqueue_event_(const QueuedEvent &event) {
  if (this->event_queue_ == nullptr || xQueueSend(this->event_queue_, &event, 0) != pdTRUE) {
    ESP_LOGW(TAG, "HID event queue full; dropping event %u", static_cast<unsigned>(event.type));
    return false;
  }
  return true;
}

bool ClassicBTHID::matches_address_filter_(const uint8_t *address) const {
  return !this->has_address_filter_ ||
         std::memcmp(address, this->filter_address_, ESP_BD_ADDR_LEN) == 0;
}

bool ClassicBTHID::is_peripheral_(const esp_bt_gap_cb_param_t *param) {
  uint32_t cod = 0;
  for (int index = 0; index < param->disc_res.num_prop; ++index) {
    const esp_bt_gap_dev_prop_t *property = &param->disc_res.prop[index];
    if (property->type == ESP_BT_GAP_DEV_PROP_COD && property->val != nullptr &&
        property->len >= static_cast<int>(sizeof(cod))) {
      std::memcpy(&cod, property->val, sizeof(cod));
      return esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    }
  }
  return false;
}

std::string ClassicBTHID::address_to_string_(const uint8_t *address) {
  char text[18];
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                address[0], address[1], address[2], address[3], address[4], address[5]);
  return text;
}

void ClassicBTHID::start_scan_() {
  this->candidate_claimed_.store(false);
  const esp_err_t result = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LENGTH, 0);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "Could not start inquiry: %s", esp_err_to_name(result));
    this->next_scan_attempt_ms_ = millis() + 1000;
    this->scan_requested_ = true;
    return;
  }
  ESP_LOGI(TAG, "Starting Bluetooth Classic inquiry");
  this->scan_running_ = true;
  this->scan_requested_ = false;
}

void ClassicBTHID::report_connection_error_(const uint8_t *address, uint8_t status) {
  const std::string text = address_to_string_(address);
  this->on_connection_error_callbacks_.call(text, status);
}

void ClassicBTHID::process_event_(const QueuedEvent &event) {
  switch (event.type) {
    case EventType::HOST_READY:
      this->host_ready_ = true;
      this->scan_requested_ = true;
      break;
    case EventType::HOST_ERROR:
      ESP_LOGE(TAG, "HID host initialization failed: status=%d", event.hid_status);
      this->mark_failed();
      break;
    case EventType::DISCOVERY_RESULT: {
      if (!this->scan_running_ || this->connection_pending_ || this->device_connected_) {
        break;
      }
      std::memcpy(this->active_address_, event.address, ESP_BD_ADDR_LEN);
      this->connection_pending_ = true;
      this->connect_after_discovery_ = true;
      this->scan_requested_ = false;
      ESP_LOGI(TAG, "Peripheral found; stopping inquiry before HID connection");
      const esp_err_t result = esp_bt_gap_cancel_discovery();
      if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not stop inquiry: %s", esp_err_to_name(result));
        this->connect_after_discovery_ = false;
        this->connection_pending_ = false;
        this->scan_requested_ = true;
      }
      break;
    }
    case EventType::DISCOVERY_STOPPED:
      this->scan_running_ = false;
      this->candidate_claimed_.store(false);
      if (this->connect_after_discovery_) {
        this->connect_after_discovery_ = false;
        ESP_LOGI(TAG, "Inquiry stopped; connecting to HID device");
        const esp_err_t result = esp_bt_hid_host_connect(this->active_address_);
        if (result != ESP_OK) {
          ESP_LOGW(TAG, "Could not start HID connection: %s", esp_err_to_name(result));
          this->connection_pending_ = false;
          this->report_connection_error_(this->active_address_, static_cast<uint8_t>(result));
          this->scan_requested_ = true;
        }
      } else if (!this->connection_pending_ && !this->device_connected_) {
        this->scan_requested_ = true;
      }
      break;
    case EventType::OPEN: {
      this->connection_pending_ = false;
      const std::string address = address_to_string_(event.address);
      if (event.hid_status == ESP_HIDH_OK &&
          event.connection_state == ESP_HIDH_CONN_STATE_CONNECTED) {
        std::memcpy(this->active_address_, event.address, ESP_BD_ADDR_LEN);
        this->device_connected_ = true;
        ESP_LOGI(TAG, "Connected to HID device %s", address.c_str());
        this->on_connect_callbacks_.call(address);
      } else {
        ESP_LOGW(TAG, "HID connection failed for %s: status=%d", address.c_str(), event.hid_status);
        this->report_connection_error_(event.address, static_cast<uint8_t>(event.hid_status));
        this->scan_requested_ = true;
      }
      break;
    }
    case EventType::INPUT: {
      const std::string address = address_to_string_(event.address);
      const std::vector<uint8_t> report(event.report.begin(), event.report.begin() + event.report_length);
      this->on_input_callbacks_.call(address, event.protocol_mode, report);
      break;
    }
    case EventType::CLOSE: {
      this->device_connected_ = false;
      this->connection_pending_ = false;
      const std::string address = address_to_string_(event.address);
      ESP_LOGI(TAG, "Disconnected from HID device %s", address.c_str());
      this->on_disconnect_callbacks_.call(address);
      this->scan_requested_ = true;
      break;
    }
  }
}

void ClassicBTHID::gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  ClassicBTHID *self = instance_;
  if (self == nullptr) {
    return;
  }

  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      if (!is_peripheral_(param) || !self->matches_address_filter_(param->disc_res.bda)) {
        break;
      }
      bool expected = false;
      if (!self->candidate_claimed_.compare_exchange_strong(expected, true)) {
        break;
      }
      QueuedEvent queued{};
      queued.type = EventType::DISCOVERY_RESULT;
      std::memcpy(queued.address, param->disc_res.bda, ESP_BD_ADDR_LEN);
      if (!self->enqueue_event_(queued)) {
        self->candidate_claimed_.store(false);
      }
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        QueuedEvent queued{};
        queued.type = EventType::DISCOVERY_STOPPED;
        self->enqueue_event_(queued);
      }
      break;
    case ESP_BT_GAP_CFM_REQ_EVT:
      ESP_LOGW(TAG, "Pairing confirmation requested (numeric code %06" PRIu32 "); accepting",
               param->cfm_req.num_val);
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
      ESP_LOGW(TAG, "Legacy pairing PIN requested; replying with 0000");
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
      break;
    }
    default:
      break;
  }
}

void ClassicBTHID::hid_callback_(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {
  ClassicBTHID *self = instance_;
  if (self == nullptr) {
    return;
  }

  QueuedEvent queued{};
  switch (event) {
    case ESP_HIDH_INIT_EVT:
      queued.type = param->init.status == ESP_HIDH_OK ? EventType::HOST_READY : EventType::HOST_ERROR;
      queued.hid_status = param->init.status;
      self->enqueue_event_(queued);
      break;
    case ESP_HIDH_OPEN_EVT:
      if (param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTING) {
        break;
      }
      queued.type = EventType::OPEN;
      queued.hid_status = param->open.status;
      queued.connection_state = param->open.conn_status;
      std::memcpy(queued.address, param->open.bd_addr, ESP_BD_ADDR_LEN);
      self->enqueue_event_(queued);
      break;
    case ESP_HIDH_DATA_IND_EVT:
      if (param->data_ind.data == nullptr || param->data_ind.len == 0) {
        break;
      }
      if (param->data_ind.len > MAX_INPUT_REPORT_SIZE) {
        ESP_LOGW(TAG, "Dropping oversized HID input report: %u bytes", param->data_ind.len);
        break;
      }
      queued.type = EventType::INPUT;
      std::memcpy(queued.address, self->active_address_, ESP_BD_ADDR_LEN);
      queued.protocol_mode = static_cast<uint8_t>(param->data_ind.proto_mode);
      queued.report_length = param->data_ind.len;
      std::memcpy(queued.report.data(), param->data_ind.data, queued.report_length);
      self->enqueue_event_(queued);
      break;
    case ESP_HIDH_CLOSE_EVT:
      if (param->close.conn_status == ESP_HIDH_CONN_STATE_DISCONNECTING) {
        break;
      }
      queued.type = EventType::CLOSE;
      std::memcpy(queued.address, self->active_address_, ESP_BD_ADDR_LEN);
      self->enqueue_event_(queued);
      break;
    default:
      break;
  }
}

}  // namespace esphome::classic_bt_hid