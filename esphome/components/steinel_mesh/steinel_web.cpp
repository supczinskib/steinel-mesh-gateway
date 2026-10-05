#include "steinel_mesh.h"
#include "steinel_page.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <string_view>
#include <utility>

#include "cJSON.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_crt_bundle.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/network/util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

namespace esphome::steinel_mesh {

static const char *const WEB_TAG = "steinel_web";
static const char *const API_BASE = "https://connectapp.steinel.de/api";
static constexpr uint32_t CLOUD_TASK_STACK_BYTES = 8192;
static constexpr uint32_t CLOUD_ERROR_REBOOT_DELAY_MS = 20000;
static constexpr uint32_t CLOUD_DISCOVER_SESSION_TIMEOUT_MS = 300000;
static constexpr uint32_t CLOUD_API_SHUTDOWN_TIMEOUT_MS = 5000;
static constexpr uint32_t AUTO_UPDATE_TASK_STACK_BYTES = 10240;
static const char *const RELEASE_ASSET_REDIRECT_PREFIX =
    "https://release-assets.githubusercontent.com/";
static constexpr size_t AUTO_UPDATE_MAX_REDIRECT_URL_LENGTH = 4096;
static constexpr size_t AUTO_UPDATE_REQUEST_OVERHEAD_BYTES = 256;
static constexpr uint32_t AUTO_UPDATE_STAGE_TIMEOUT_MS = 7500;
static constexpr uint32_t AUTO_UPDATE_ERROR_REBOOT_DELAY_MS = 20000;
static const char *const RELEASE_DOWNLOAD_PREFIX =
    "https://github.com/supczinskib/steinel-mesh-gateway/releases/download/v";
static const char *const RELEASE_ASSET_PREFIX =
    "steinel-mesh-esp32-c3-gateway-v";

#ifdef USE_STEINEL_EXTENDED_DIAGNOSTICS
static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Power-on reset";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Software panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep-sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    case ESP_RST_UNKNOWN:
    default: return "Unknown";
  }
}
#endif

// Stream status through a fixed buffer to bound Mesh-mode heap usage.
class StatusJsonWriter {
 public:
  explicit StatusJsonWriter(httpd_req_t *request) : request_(request) {}

  bool append(std::string_view value) {
    while (this->ok_ && !value.empty()) {
      const size_t available = sizeof(this->buffer_) - this->length_;
      if (available == 0 && !this->flush_())
        break;
      const size_t count = std::min(value.size(), sizeof(this->buffer_) - this->length_);
      std::memcpy(this->buffer_ + this->length_, value.data(), count);
      this->length_ += count;
      value.remove_prefix(count);
    }
    return this->ok_;
  }

  bool escaped(const char *value) {
    if (value == nullptr)
      return this->ok_;
    while (*value != '\0' && this->ok_) {
      const unsigned char byte = static_cast<unsigned char>(*value++);
      if ((byte == '"' || byte == '\\') && !this->append("\\"))
        break;
      if (byte >= 0x20) {
        const char character = static_cast<char>(byte);
        this->append(std::string_view(&character, 1));
      }
    }
    return this->ok_;
  }

  bool number(const char *name, uint32_t value) {
    if (!this->append(name))
      return false;
    char formatted[16];
    const int length = std::snprintf(formatted, sizeof(formatted), "%" PRIu32, value);
    return length > 0 && static_cast<size_t>(length) < sizeof(formatted) &&
           this->append(std::string_view(formatted, static_cast<size_t>(length)));
  }

  bool signed_number(const char *name, int32_t value) {
    if (!this->append(name))
      return false;
    char formatted[16];
    const int length = std::snprintf(formatted, sizeof(formatted), "%" PRId32, value);
    return length > 0 && static_cast<size_t>(length) < sizeof(formatted) &&
           this->append(std::string_view(formatted, static_cast<size_t>(length)));
  }

  bool finish() {
    if (!this->flush_())
      return false;
    this->ok_ = httpd_resp_send_chunk(this->request_, nullptr, 0) == ESP_OK;
    return this->ok_;
  }

 private:
  bool flush_() {
    if (!this->ok_ || this->length_ == 0)
      return this->ok_;
    this->ok_ = httpd_resp_send_chunk(this->request_, this->buffer_, this->length_) == ESP_OK;
    this->length_ = 0;
    return this->ok_;
  }

  httpd_req_t *request_;
  char buffer_[256]{};
  size_t length_{0};
  bool ok_{true};
};

struct SteinelMesh::AutoUpdateContext {
  SteinelMesh *owner;
  esp_ota_handle_t ota_handle{0};
  size_t received{0};
  esp_err_t sink_error{ESP_OK};
  mbedtls_sha256_context sha256{};
  std::string redirect_url;
  bool accept_firmware_data{false};
};

struct SteinelMesh::CloudTaskArgs {
  SteinelMesh *owner;
  CloudJob job;
  std::string email;
  std::string password;
  std::string network_id;
  uint32_t iv_index;
  uint16_t node_address;
};

struct SteinelMesh::CloudBody {
  bool use_flash{false};
  uint8_t *memory{nullptr};
  size_t capacity{0};
  size_t length{0};
  const esp_partition_t *partition{nullptr};
  size_t erased_bytes{0};
  size_t wipe_bytes{0};
  esp_err_t sink_error{ESP_OK};

  ~CloudBody() {
    if (this->memory != nullptr) {
      std::memset(this->memory, 0, this->capacity);
      heap_caps_free(this->memory);
    }
    if (this->use_flash && this->partition != nullptr && this->wipe_bytes != 0)
      esp_partition_erase_range(this->partition, 0, this->wipe_bytes);
  }

  bool prepare(bool flash, std::string &error) {
    this->use_flash = flash;
    if (!flash) return true;
    this->partition = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (this->partition == nullptr || this->partition == running || this->partition->size < 64 * 1024) {
      error = "No safe inactive OTA workspace is available";
      return false;
    }
    return true;
  }

  void reset_for_retry() {
    this->length = 0;
    this->sink_error = ESP_OK;
    if (this->memory != nullptr && this->capacity != 0) std::memset(this->memory, 0, this->capacity);
    // Erase reused sectors on every retry: flash cannot program 0 back to 1.
    this->erased_bytes = 0;
  }

  size_t limit() const {
    return this->use_flash && this->partition != nullptr
               ? this->partition->size
               : SteinelMesh::MAX_DISCOVERY_RESPONSE_BYTES;
  }

  bool append(const uint8_t *data, size_t size) {
    if (data == nullptr || size == 0) return true;
    if (this->sink_error != ESP_OK || size > this->limit() - std::min(this->length, this->limit())) {
      this->sink_error = ESP_ERR_INVALID_SIZE;
      return false;
    }
    const size_t required = this->length + size;
    if (this->use_flash) {
      constexpr size_t SECTOR_SIZE = 4096;
      const size_t erase_target = std::min<size_t>(this->partition->size,
                                           (required + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1));
      if (erase_target > this->erased_bytes) {
        this->sink_error = esp_partition_erase_range(this->partition, this->erased_bytes,
                                                     erase_target - this->erased_bytes);
        if (this->sink_error != ESP_OK) return false;
        this->erased_bytes = erase_target;
        this->wipe_bytes = std::max(this->wipe_bytes, erase_target);
      }
      this->sink_error = esp_partition_write(this->partition, this->length, data, size);
      if (this->sink_error != ESP_OK) return false;
    } else {
      const size_t required_with_terminator = required + 1;
      if (required_with_terminator > this->capacity) {
        const size_t wanted = std::min(this->limit() + 1,
                                       (required_with_terminator + 2047U) & ~static_cast<size_t>(2047U));
        void *resized = heap_caps_realloc(this->memory, wanted, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (resized == nullptr) {
          this->sink_error = ESP_ERR_NO_MEM;
          return false;
        }
        this->memory = static_cast<uint8_t *>(resized);
        this->capacity = wanted;
      }
      std::memcpy(this->memory + this->length, data, size);
      this->memory[required] = 0;
    }
    this->length = required;
    return true;
  }
};

namespace {

const cJSON *object_item(const cJSON *object, const char *name) {
  return object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, name);
}

std::string json_string(const cJSON *object, const char *name) {
  const cJSON *value = object_item(object, name);
  return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

uint32_t json_uint(const cJSON *object, const char *name, uint32_t fallback = 0) {
  const cJSON *value = object_item(object, name);
  return cJSON_IsNumber(value) && value->valuedouble >= 0 ? static_cast<uint32_t>(value->valuedouble) : fallback;
}

bool decode_hex(const char *text, uint8_t *output, size_t output_size) {
  if (text == nullptr) return false;
  size_t digits = 0;
  for (size_t i = 0; text[i] != '\0'; ++i) if (text[i] != '-') ++digits;
  if (digits != output_size * 2) return false;
  auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  size_t input = 0;
  for (size_t index = 0; index < output_size; index++) {
    while (text[input] == '-') input++;
    const int high = nibble(text[input++]);
    while (text[input] == '-') input++;
    const int low = nibble(text[input++]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  while (text[input] == '-') input++;
  return text[input] == '\0';
}

bool parse_hex_value(const char *text, uint16_t &output) {
  if (text == nullptr) return false;
  const size_t length = std::strlen(text);
  if (length == 0 || length > 6) return false;
  const char *start = text;
  if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) start += 2;
  if (*start == '\0' || std::strlen(start) > 4) return false;
  uint16_t parsed = 0;
  for (; *start != '\0'; ++start) {
    const char c = *start;
    const int nibble = c >= '0' && c <= '9' ? c - '0' :
                       c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (nibble < 0) return false;
    parsed = (parsed << 4) | nibble;
  }
  output = parsed;
  return true;
}

bool parse_hex_address(const char *text, uint16_t &output) {
  uint16_t parsed = 0;
  if (!parse_hex_value(text, parsed) || !protocol::unicast(parsed)) return false;
  output = parsed;
  return true;
}

bool parse_hex_address(const std::string &text, uint16_t &output) {
  return parse_hex_address(text.c_str(), output);
}

class FlashJsonReader {
 public:
  FlashJsonReader(const esp_partition_t *partition, size_t length) : partition_(partition), length_(length) {}

  size_t position() const { return this->position_; }
  bool healthy() const { return this->error_ == ESP_OK; }

  bool peek(char &value) {
    if (this->position_ >= this->length_) return false;
    if (this->position_ < this->cache_start_ || this->position_ >= this->cache_start_ + this->cache_size_) {
      this->cache_start_ = this->position_;
      this->cache_size_ = std::min(this->cache_.size(), this->length_ - this->cache_start_);
      this->error_ = esp_partition_read(this->partition_, this->cache_start_, this->cache_.data(), this->cache_size_);
      if (this->error_ != ESP_OK) return false;
      if (this->cache_start_ != 0 && this->cache_start_ % 16384 == 0) esphome::delay(1);
    }
    value = this->cache_[this->position_ - this->cache_start_];
    return true;
  }

  bool get(char &value) {
    if (!this->peek(value)) return false;
    this->position_++;
    return true;
  }

  void skip_whitespace() {
    char value = 0;
    while (this->peek(value) && std::isspace(static_cast<unsigned char>(value))) this->position_++;
  }

  bool consume(char expected) {
    this->skip_whitespace();
    char value = 0;
    return this->get(value) && value == expected;
  }

  bool read_string(char *output, size_t capacity) {
    this->skip_whitespace();
    char value = 0;
    if (!this->get(value) || value != '"') return false;
    size_t written = 0;
    const auto emit = [&](char byte) {
      if (output != nullptr && capacity > 0 && written + 1 < capacity) output[written] = byte;
      written++;
    };
    const auto read_hex4 = [&](uint16_t &codepoint) {
      codepoint = 0;
      for (uint8_t index = 0; index < 4; index++) {
        char hex = 0;
        if (!this->get(hex)) return false;
        const int nibble = hex >= '0' && hex <= '9' ? hex - '0' :
                           hex >= 'a' && hex <= 'f' ? hex - 'a' + 10 :
                           hex >= 'A' && hex <= 'F' ? hex - 'A' + 10 : -1;
        if (nibble < 0) return false;
        codepoint = uint16_t((codepoint << 4) | nibble);
      }
      return true;
    };
    if (output != nullptr && capacity > 0) output[0] = '\0';
    while (this->get(value)) {
      if (value == '"') {
        if (output != nullptr && capacity > 0) {
          size_t end = std::min(written, capacity - 1);
          // Truncate names only at UTF-8 codepoint boundaries.
          if (written >= capacity) {
            size_t start = end;
            while (start > 0 && (static_cast<uint8_t>(output[start - 1]) & 0xC0) == 0x80) --start;
            if (start > 0) {
              --start;
              const uint8_t lead = output[start];
              const size_t bytes = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
              if (end - start < bytes) end = start;
            }
          }
          output[end] = '\0';
        }
        return true;
      }
      if (static_cast<unsigned char>(value) < 0x20) return false;
      if (value == '\\') {
        if (!this->get(value)) return false;
        switch (value) {
          case '"': case '\\': case '/': break;
          case 'b': value = '\b'; break;
          case 'f': value = '\f'; break;
          case 'n': value = '\n'; break;
          case 'r': value = '\r'; break;
          case 't': value = '\t'; break;
          case 'u': {
            uint16_t first = 0;
            if (!read_hex4(first) || first == 0) return false;
            uint32_t codepoint = first;
            if (first >= 0xD800 && first <= 0xDBFF) {
              char slash = 0, u = 0; uint16_t second = 0;
              if (!this->get(slash) || !this->get(u) || slash != '\\' || u != 'u' ||
                  !read_hex4(second) || second < 0xDC00 || second > 0xDFFF) return false;
              codepoint = 0x10000 + ((first - 0xD800) << 10) + second - 0xDC00;
            } else if (first >= 0xDC00 && first <= 0xDFFF) return false;
            if (codepoint < 0x80) emit(char(codepoint));
            else if (codepoint < 0x800) { emit(char(0xC0 | (codepoint >> 6))); emit(char(0x80 | (codepoint & 63))); }
            else if (codepoint < 0x10000) {
              emit(char(0xE0 | (codepoint >> 12))); emit(char(0x80 | ((codepoint >> 6) & 63))); emit(char(0x80 | (codepoint & 63)));
            } else {
              emit(char(0xF0 | (codepoint >> 18))); emit(char(0x80 | ((codepoint >> 12) & 63)));
              emit(char(0x80 | ((codepoint >> 6) & 63))); emit(char(0x80 | (codepoint & 63)));
            }
            continue;
          }
          default: return false;
        }
      }
      emit(value);
    }
    return false;
  }

  bool read_uint(uint32_t &output) {
    this->skip_whitespace();
    char value = 0;
    if (!this->peek(value) || value < '0' || value > '9') return false;
    const bool leading_zero = value == '0';
    uint64_t parsed = 0;
    do {
      this->position_++;
      parsed = parsed * 10U + static_cast<uint8_t>(value - '0');
      if (parsed > UINT32_MAX) return false;
      if (leading_zero && this->peek(value) && value >= '0' && value <= '9') return false;
    } while (this->peek(value) && value >= '0' && value <= '9');
    output = static_cast<uint32_t>(parsed);
    return true;
  }

  bool skip_value(uint8_t depth = 0) {
    if (depth > 24) return false;
    this->skip_whitespace();
    char value = 0;
    if (!this->peek(value)) return false;
    if (value == '"') return this->read_string(nullptr, 0);
    if (value == '{') {
      this->position_++;
      this->skip_whitespace();
      if (this->peek(value) && value == '}') { this->position_++; return true; }
      while (true) {
        if (!this->read_string(nullptr, 0) || !this->consume(':') || !this->skip_value(depth + 1)) return false;
        this->skip_whitespace();
        if (!this->get(value)) return false;
        if (value == '}') return true;
        if (value != ',') return false;
      }
    }
    if (value == '[') {
      this->position_++;
      this->skip_whitespace();
      if (this->peek(value) && value == ']') { this->position_++; return true; }
      while (true) {
        if (!this->skip_value(depth + 1)) return false;
        this->skip_whitespace();
        if (!this->get(value)) return false;
        if (value == ']') return true;
        if (value != ',') return false;
      }
    }
    if (value == 't' || value == 'f' || value == 'n') {
      const char *literal = value == 't' ? "true" : value == 'f' ? "false" : "null";
      for (; *literal; ++literal) if (!this->get(value) || value != *literal) return false;
      return true;
    }
    if (value == '-') { ++this->position_; if (!this->peek(value)) return false; }
    if (value == '0') ++this->position_;
    else if (value >= '1' && value <= '9') {
      do { ++this->position_; } while (this->peek(value) && value >= '0' && value <= '9');
    } else return false;
    if (this->peek(value) && value == '.') {
      ++this->position_;
      if (!this->peek(value) || value < '0' || value > '9') return false;
      do { ++this->position_; } while (this->peek(value) && value >= '0' && value <= '9');
    }
    if (this->peek(value) && (value == 'e' || value == 'E')) {
      ++this->position_;
      if (this->peek(value) && (value == '+' || value == '-')) ++this->position_;
      if (!this->peek(value) || value < '0' || value > '9') return false;
      do { ++this->position_; } while (this->peek(value) && value >= '0' && value <= '9');
    }
    return true;
  }

 private:
  const esp_partition_t *partition_;
  size_t length_;
  size_t position_{0};
  size_t cache_start_{static_cast<size_t>(-1)};
  size_t cache_size_{0};
  std::array<char, 512> cache_{};
  esp_err_t error_{ESP_OK};
};

template<typename Handler> bool read_object(FlashJsonReader &reader, Handler handler) {
  if (!reader.consume('{')) return false;
  reader.skip_whitespace();
  char delimiter = 0;
  if (reader.peek(delimiter) && delimiter == '}') { reader.get(delimiter); return true; }
  while (true) {
    char key[48]{};
    if (!reader.read_string(key, sizeof(key)) || !reader.consume(':') || !handler(key)) return false;
    reader.skip_whitespace();
    if (!reader.get(delimiter)) return false;
    if (delimiter == '}') return true;
    if (delimiter != ',') return false;
  }
}

template<typename Handler> bool read_array(FlashJsonReader &reader, Handler handler) {
  if (!reader.consume('[')) return false;
  reader.skip_whitespace();
  char delimiter = 0;
  if (reader.peek(delimiter) && delimiter == ']') { reader.get(delimiter); return true; }
  size_t index = 0;
  while (true) {
    if (!handler(index++)) return false;
    reader.skip_whitespace();
    if (!reader.get(delimiter)) return false;
    if (delimiter == ']') return true;
    if (delimiter != ',') return false;
  }
}

struct BackupNode {
  protocol::Node details{};
  uint16_t address{0};
  uint16_t element_count{0};
  uint16_t bound_app_key{0};
  bool address_valid{false};
  bool bind_valid{false};
  bool device_key_valid{false};
  bool model_1000{false};
  bool model_1203{false};
  bool model_1206{false};
  bool model_130f{false};
  bool model_1100{false};
  uint16_t lc_element_index{0};
  uint16_t sensor_element_index{0};
  std::array<uint8_t, 16> device_key{};
  char name[48]{};
};

struct BackupAppKey {
  uint16_t index{0};
  uint16_t net_index{0};
  bool index_valid{false};
  bool net_index_valid{false};
  bool key_valid{false};
  std::array<uint8_t, 16> key{};
};

struct BackupNetKey {
  uint16_t index{0};
  bool index_valid{false};
  bool key_valid{false};
  std::array<uint8_t, 16> key{};
};

struct BackupNodeInfo {
  uint16_t address{0};
  uint16_t scene{6};
  bool address_valid{false};
};

struct BackupSummary {
  static constexpr size_t MAX_NODES = 64;
  static constexpr size_t MAX_KEYS = 16;
  std::array<BackupNode, MAX_NODES> nodes{};
  std::array<BackupAppKey, MAX_KEYS> app_keys{};
  std::array<BackupNetKey, MAX_KEYS> net_keys{};
  std::array<BackupNodeInfo, MAX_NODES> node_infos{};
  size_t node_count{0};
  size_t app_key_count{0};
  size_t net_key_count{0};
  size_t node_info_count{0};
  uint16_t low_address{0};
  uint16_t high_address{0};
  bool unicast_range_valid{false};
  bool mesh_uuid_valid{false};
  std::array<uint8_t, 16> mesh_uuid{};
  char mesh_name[48]{};
};

bool read_bindings(FlashJsonReader &reader, uint16_t selected_key, uint16_t &binding, bool &valid) {
  return read_array(reader, [&](size_t) {
    uint32_t parsed = 0;
    if (!reader.read_uint(parsed) || parsed > 0x0FFF) return false;
    if (selected_key == protocol::UNKNOWN_BINDING ? !valid : parsed == selected_key) {
      binding = static_cast<uint16_t>(parsed);
      valid = true;
    }
    return true;
  });
}

bool read_model(FlashJsonReader &reader, size_t element_index, BackupNode &node, uint16_t selected_key) {
  char model_id[16]{};
  uint16_t binding = 0;
  bool binding_valid = false;
  uint16_t publish_address = 0;
  if (!read_object(reader, [&](const char *key) {
        if (std::strcmp(key, "modelId") == 0) return reader.read_string(model_id, sizeof(model_id));
        if (std::strcmp(key, "bind") == 0) return read_bindings(reader, selected_key, binding, binding_valid);
        if (std::strcmp(key, "publish") == 0) {
          char first = 0;
          reader.skip_whitespace();
          if (!reader.peek(first)) return false;
          if (first != '{') return reader.skip_value();
          return read_object(reader, [&](const char *field) {
            if (std::strcmp(field, "address") != 0) return reader.skip_value();
            char address[12]{};
            return reader.read_string(address, sizeof(address)) && parse_hex_value(address, publish_address);
          });
        }
        return reader.skip_value();
      })) return false;
  uint16_t model = 0;
  if (std::strlen(model_id) == 4 && parse_hex_value(model_id, model) &&
      element_index < protocol::MAX_ELEMENTS) {
    const uint16_t cap = protocol::capability(model);
    auto &element = node.details.elements[element_index];
    if (binding_valid) element.capabilities |= cap;
    if (cap != 0 && binding_valid) {
      if (element.app_key == protocol::UNKNOWN_BINDING) element.app_key = binding;
      else if (element.app_key != binding) element.app_key = 0xFFFE;
      if ((cap & (protocol::ONOFF | protocol::LIGHTNESS | protocol::LC | protocol::SENSOR)) &&
          (!node.bind_valid || binding < node.bound_app_key)) {
        node.bound_app_key = binding; node.bind_valid = true;
      }
    }
    if (cap == protocol::SENSOR && binding_valid && protocol::group(publish_address)) element.sensor_group = publish_address;
  }
  if (element_index == 0) {
    node.model_1000 |= std::strcmp(model_id, "1000") == 0;
    node.model_1203 |= std::strcmp(model_id, "1203") == 0;
    node.model_1206 |= std::strcmp(model_id, "1206") == 0;
  }
  if (std::strcmp(model_id, "130F") == 0 || std::strcmp(model_id, "130f") == 0) {
    node.model_130f = true;
    node.lc_element_index = static_cast<uint16_t>(element_index);
  }
  if (std::strcmp(model_id, "1100") == 0) {
    node.model_1100 = true;
    node.sensor_element_index = static_cast<uint16_t>(element_index);
  }
  return true;
}

bool read_element(FlashJsonReader &reader, size_t element_index, BackupNode &node, uint16_t selected_key) {
  return read_object(reader, [&](const char *key) {
    if (std::strcmp(key, "models") != 0) return reader.skip_value();
    return read_array(reader, [&](size_t) { return read_model(reader, element_index, node, selected_key); });
  });
}

bool read_node(FlashJsonReader &reader, BackupNode &node, uint16_t selected_key) {
  char device_key[48]{};
  bool device_key_seen = false;
  const bool ok = read_object(reader, [&](const char *key) {
    if (std::strcmp(key, "UUID") == 0 || std::strcmp(key, "uuid") == 0) {
      char text[48]{};
      return reader.read_string(text, sizeof(text)) && decode_hex(text, node.details.uuid.data(), 16);
    }
    if (std::strcmp(key, "cid") == 0 || std::strcmp(key, "pid") == 0) {
      char text[12]{};
      uint16_t value = 0;
      if (!reader.read_string(text, sizeof(text)) || !parse_hex_value(text, value)) return false;
      if (std::strcmp(key, "cid") == 0) node.details.company_id = value;
      else node.details.product_id = value;
      return true;
    }
    if (std::strcmp(key, "name") == 0) return reader.read_string(node.name, sizeof(node.name));
    if (std::strcmp(key, "deviceKey") == 0) {
      device_key_seen = true;
      return reader.read_string(device_key, sizeof(device_key));
    }
    if (std::strcmp(key, "unicastAddress") == 0) {
      char address[12]{};
      if (!reader.read_string(address, sizeof(address))) return false;
      node.address_valid = parse_hex_address(address, node.address);
      return true;
    }
    if (std::strcmp(key, "elements") == 0) {
      return read_array(reader, [&](size_t index) {
        node.element_count = static_cast<uint16_t>(std::min<size_t>(index + 1, UINT16_MAX));
        return read_element(reader, index, node, selected_key);
      });
    }
    return reader.skip_value();
  });
  node.device_key_valid = ok && device_key_seen &&
                          decode_hex(device_key, node.device_key.data(), node.device_key.size());
  node.details.address = node.address;
  node.details.element_count = static_cast<uint8_t>(std::min<uint16_t>(node.element_count, UINT8_MAX));
  node.details.device_key = node.device_key;
  std::memcpy(node.details.name, node.name, sizeof(node.name));
  node.details.nightmatiq = node.details.product_id == 0x1DCE || std::strstr(node.name, "IS Digi NM") != nullptr;
  return ok;
}

bool read_app_key(FlashJsonReader &reader, BackupAppKey &app) {
  char key_text[48]{};
  bool key_seen = false;
  const bool ok = read_object(reader, [&](const char *key) {
    uint32_t parsed = 0;
    if (std::strcmp(key, "index") == 0) {
      if (!reader.read_uint(parsed) || parsed > 0x0FFF) return false;
      app.index = static_cast<uint16_t>(parsed); app.index_valid = true; return true;
    }
    if (std::strcmp(key, "boundNetKey") == 0) {
      if (!reader.read_uint(parsed) || parsed > 0x0FFF) return false;
      app.net_index = static_cast<uint16_t>(parsed); app.net_index_valid = true; return true;
    }
    if (std::strcmp(key, "key") == 0) { key_seen = true; return reader.read_string(key_text, sizeof(key_text)); }
    return reader.skip_value();
  });
  app.key_valid = ok && key_seen && decode_hex(key_text, app.key.data(), app.key.size());
  return ok;
}

bool read_net_key(FlashJsonReader &reader, BackupNetKey &net) {
  char key_text[48]{};
  bool key_seen = false;
  const bool ok = read_object(reader, [&](const char *key) {
    uint32_t parsed = 0;
    if (std::strcmp(key, "index") == 0) {
      if (!reader.read_uint(parsed) || parsed > 0x0FFF) return false;
      net.index = static_cast<uint16_t>(parsed); net.index_valid = true; return true;
    }
    if (std::strcmp(key, "key") == 0) { key_seen = true; return reader.read_string(key_text, sizeof(key_text)); }
    return reader.skip_value();
  });
  net.key_valid = ok && key_seen && decode_hex(key_text, net.key.data(), net.key.size());
  return ok;
}

bool read_node_info(FlashJsonReader &reader, BackupNodeInfo &info) {
  return read_object(reader, [&](const char *key) {
    uint32_t parsed = 0;
    if (std::strcmp(key, "nodeAddress") == 0) {
      if (!reader.read_uint(parsed) || parsed == 0 || parsed > 0x7FFF) return false;
      info.address = static_cast<uint16_t>(parsed); info.address_valid = true; return true;
    }
    if (std::strcmp(key, "defaultSceneNumber") == 0) {
      if (!reader.read_uint(parsed)) return false;
      if (parsed > 0 && parsed <= UINT16_MAX) info.scene = static_cast<uint16_t>(parsed);
      return true;
    }
    return reader.skip_value();
  });
}

bool read_steinel(FlashJsonReader &reader, BackupSummary &summary) {
  return read_object(reader, [&](const char *key) {
    if (std::strcmp(key, "nodeInfos") != 0) return reader.skip_value();
    return read_array(reader, [&](size_t) {
      if (summary.node_info_count >= summary.node_infos.size()) return false;
      return read_node_info(reader, summary.node_infos[summary.node_info_count++]);
    });
  });
}

bool read_unicast_range(FlashJsonReader &reader, BackupSummary &summary) {
  bool low_seen = false;
  bool high_seen = false;
  const bool ok = read_object(reader, [&](const char *key) {
    if (std::strcmp(key, "lowAddress") != 0 && std::strcmp(key, "highAddress") != 0)
      return reader.skip_value();
    char address[12]{};
    if (!reader.read_string(address, sizeof(address))) return false;
    uint16_t parsed = 0;
    if (!parse_hex_address(address, parsed)) return false;
    if (std::strcmp(key, "lowAddress") == 0) {
      summary.low_address = parsed;
      low_seen = true;
    } else {
      summary.high_address = parsed;
      high_seen = true;
    }
    return true;
  });
  summary.unicast_range_valid = ok && low_seen && high_seen &&
                                summary.low_address > 0 &&
                                summary.low_address <= summary.high_address &&
                                summary.high_address < 0x8000;
  return ok;
}

bool read_provisioner(FlashJsonReader &reader, BackupSummary &summary) {
  return read_object(reader, [&](const char *key) {
    if (std::strcmp(key, "allocatedUnicastRange") != 0) return reader.skip_value();
    return read_array(reader, [&](size_t index) {
      if (index == 0) return read_unicast_range(reader, summary);
      return reader.skip_value();
    });
  });
}

bool read_backup(FlashJsonReader &reader, BackupSummary &summary, uint16_t selected_key = protocol::UNKNOWN_BINDING) {
  return read_object(reader, [&](const char *key) {
    if (std::strcmp(key, "meshUUID") == 0) {
      char uuid[48]{};
      if (!reader.read_string(uuid, sizeof(uuid))) return false;
      summary.mesh_uuid_valid = decode_hex(uuid, summary.mesh_uuid.data(), summary.mesh_uuid.size());
      return true;
    }
    if (std::strcmp(key, "meshName") == 0) return reader.read_string(summary.mesh_name, sizeof(summary.mesh_name));
    if (std::strcmp(key, "nodes") == 0) {
      return read_array(reader, [&](size_t) {
        if (summary.node_count >= summary.nodes.size()) return false;
        return read_node(reader, summary.nodes[summary.node_count++], selected_key);
      });
    }
    if (std::strcmp(key, "appKeys") == 0) {
      return read_array(reader, [&](size_t) {
        if (summary.app_key_count >= summary.app_keys.size()) return false;
        return read_app_key(reader, summary.app_keys[summary.app_key_count++]);
      });
    }
    if (std::strcmp(key, "netKeys") == 0) {
      return read_array(reader, [&](size_t) {
        if (summary.net_key_count >= summary.net_keys.size()) return false;
        return read_net_key(reader, summary.net_keys[summary.net_key_count++]);
      });
    }
    if (std::strcmp(key, "steinel") == 0) return read_steinel(reader, summary);
    if (std::strcmp(key, "provisioners") == 0) {
      return read_array(reader, [&](size_t index) {
        if (index == 0) return read_provisioner(reader, summary);
        return reader.skip_value();
      });
    }
    return reader.skip_value();
  });
}

}  // namespace

esp_err_t SteinelMesh::cloud_http_event_(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0)
    return ESP_OK;
  auto *body = static_cast<CloudBody *>(event->user_data);
  return body != nullptr && body->append(static_cast<const uint8_t *>(event->data), event->data_len)
             ? ESP_OK
             : ESP_FAIL;
}

bool SteinelMesh::load_config_() {
  StoredConfig loaded{};
  if (!this->config_preference_.load(&loaded) || loaded.magic != CONFIG_MAGIC ||
      loaded.version != CONFIG_VERSION || loaded.local_address == 0 ||
      loaded.onoff_address == 0 || loaded.lc_address == 0 || loaded.sensor_address == 0) {
    this->configured_ = false;
    return false;
  }
  // Migrate StoredConfig v2 to NightmatIQ's third-element Sensor Server address.
  if (loaded.sensor_address == loaded.onoff_address &&
      loaded.lc_address == static_cast<uint16_t>(loaded.onoff_address + 1)) {
    loaded.sensor_address = static_cast<uint16_t>(loaded.onoff_address + 2);
    if (!this->config_preference_.save(&loaded))
      ESP_LOGW(WEB_TAG, "Could not persist corrected NightmatIQ sensor element address");
  }
  this->config_ = loaded;
  this->configured_ = true;
  this->mesh_mode_enabled_ = (loaded.flags & (FLAG_ENABLED | FLAG_REMOVE_PENDING)) != 0;
  return true;
}

bool SteinelMesh::load_device_key_() {
  StoredDeviceKey stored{};
  if (!this->device_key_preference_.load(&stored) || stored.magic != DEVICE_KEY_MAGIC ||
      stored.version != DEVICE_KEY_VERSION ||
      std::all_of(stored.key.begin(), stored.key.end(), [](uint8_t value) { return value == 0; })) {
    this->device_key_.fill(0);
    this->device_key_valid_ = false;
    return false;
  }
  this->device_key_ = stored.key;
  this->device_key_valid_ = true;
  return true;
}

void SteinelMesh::load_retired_address_() {
  StoredRetiredAddress stored{};
  if (this->retired_address_preference_.load(&stored) &&
      stored.magic == RETIRED_ADDRESS_MAGIC &&
      stored.version == RETIRED_ADDRESS_VERSION && stored.address > 0 &&
      stored.address < 0x8000) {
    this->retired_local_address_ = stored.address;
  }
}

bool SteinelMesh::load_address_policy_() {
  StoredAddressPolicy stored{};
  if (!this->address_policy_preference_.load(&stored) ||
      stored.magic != ADDRESS_POLICY_MAGIC ||
      stored.version != ADDRESS_POLICY_VERSION || stored.pool_low == 0 ||
      stored.pool_low > stored.pool_high || stored.pool_high >= 0x8000 ||
      stored.initial_address < stored.pool_low ||
      stored.initial_address > stored.pool_high ||
      stored.current_address < stored.pool_low ||
      stored.current_address > stored.pool_high || stored.installation_nonce == 0 ||
      std::all_of(stored.mesh_uuid.begin(), stored.mesh_uuid.end(),
                  [](uint8_t value) { return value == 0; })) {
    this->address_policy_ = StoredAddressPolicy{};
    this->address_policy_valid_ = false;
    return false;
  }
  this->address_policy_ = stored;
  this->address_policy_valid_ = true;
  return true;
}

bool SteinelMesh::save_address_policy_(const StoredAddressPolicy &policy) {
  if (!this->address_policy_preference_.save(&policy)) return false;
  this->address_policy_ = policy;
  this->address_policy_valid_ = true;
  return true;
}

bool SteinelMesh::load_address_confirmation_() {
  StoredAddressConfirmation stored{};
  if (!this->address_confirmation_preference_.load(&stored) ||
      stored.magic != ADDRESS_CONFIRMATION_MAGIC ||
      stored.version != ADDRESS_CONFIRMATION_VERSION || stored.address == 0 ||
      stored.address >= 0x8000 || stored.installation_nonce == 0 ||
      std::all_of(stored.mesh_uuid.begin(), stored.mesh_uuid.end(),
                  [](uint8_t value) { return value == 0; })) {
    this->address_confirmation_ = StoredAddressConfirmation{};
    this->address_confirmation_valid_ = false;
    return false;
  }
  this->address_confirmation_ = stored;
  this->address_confirmation_valid_ = true;
  return true;
}

bool SteinelMesh::current_address_confirmed_() const {
  return this->configured_ && this->address_policy_valid_ &&
         this->address_confirmation_valid_ &&
         this->address_confirmation_.mesh_uuid == this->config_.mesh_uuid &&
         this->address_confirmation_.address == this->config_.local_address &&
         this->address_confirmation_.installation_nonce ==
             this->address_policy_.installation_nonce;
}

void SteinelMesh::persist_address_confirmation_() {
  if (this->address_confirmation_save_attempted_this_boot_ ||
      this->current_address_confirmed_() || this->mesh_rx_messages_.load() == 0 ||
      !this->configured_ || !this->address_policy_valid_ ||
      this->address_policy_.mesh_uuid != this->config_.mesh_uuid ||
      this->address_policy_.current_address != this->config_.local_address)
    return;

  this->address_confirmation_save_attempted_this_boot_ = true;
  StoredAddressConfirmation confirmation{};
  confirmation.address = this->config_.local_address;
  confirmation.installation_nonce = this->address_policy_.installation_nonce;
  confirmation.mesh_uuid = this->config_.mesh_uuid;
  if (!this->address_confirmation_preference_.save(&confirmation)) {
    ESP_LOGW(WEB_TAG, "Could not persist confirmed local Mesh address 0x%04x",
             confirmation.address);
    return;
  }
  this->address_confirmation_ = confirmation;
  this->address_confirmation_valid_ = true;
  ESP_LOGI(WEB_TAG, "Confirmed local Mesh address 0x%04x after an authenticated response",
           confirmation.address);
}

bool SteinelMesh::select_next_local_address_(uint16_t &address) const {
  if (!this->address_policy_valid_) return false;
  const uint16_t first = this->address_policy_.pool_low;
  const uint16_t last = this->address_policy_.pool_high;
  uint16_t candidate = this->address_policy_.current_address;
  const uint16_t start = candidate;
  do {
    candidate = candidate <= first ? last : static_cast<uint16_t>(candidate - 1);
    const uint32_t node_last = static_cast<uint32_t>(this->config_.onoff_address) + 2;
    if (candidate < this->config_.onoff_address || candidate > node_last) {
      address = candidate;
      return true;
    }
  } while (candidate != start);
  return false;
}

bool SteinelMesh::rotate_local_address_(std::string &error) {
  if (!this->configured_ || !this->address_policy_valid_ ||
      this->address_policy_.mesh_uuid != this->config_.mesh_uuid) {
    error = "No saved address pool is available; remove and import the network again";
    return false;
  }
  if (this->address_policy_.automatic_rotations >= AUTO_ADDRESS_ROTATION_LIMIT) {
    error = "Automatic Mesh address recovery reached its safety limit";
    return false;
  }

  uint16_t replacement = 0;
  if (!this->select_next_local_address_(replacement)) {
    error = "No unused local Mesh address remains in the saved pool";
    return false;
  }

  const uint16_t previous = this->config_.local_address;
  const StoredAddressPolicy previous_policy = this->address_policy_;
  StoredAddressPolicy updated_policy = previous_policy;
  updated_policy.current_address = replacement;
  updated_policy.automatic_rotations++;

  if (!this->save_address_policy_(updated_policy)) {
    error = "Could not save the replacement Mesh address policy";
    return false;
  }
  this->retire_local_address_();
  StoredConfig updated_config = this->config_;
  updated_config.local_address = replacement;
  if (!this->save_config_(updated_config)) {
    this->save_address_policy_(previous_policy);
    error = "Could not save the replacement Mesh address";
    return false;
  }

  ESP_LOGW(WEB_TAG,
           "Automatically rotating local Mesh address 0x%04x -> 0x%04x while preserving keys and sequence state",
           previous, replacement);
  this->set_status_("No Mesh response; trying another local address");
  this->reboot_at_ = millis() + 1500;
  this->reboot_pending_.store(true);
  return true;
}

void SteinelMesh::retire_local_address_() {
  if (!this->configured_ || this->config_.local_address == 0 ||
      this->config_.local_address >= 0x8000)
    return;
  StoredRetiredAddress stored{};
  stored.address = this->config_.local_address;
  if (this->retired_address_preference_.save(&stored)) {
    this->retired_local_address_ = stored.address;
  } else {
    ESP_LOGW(WEB_TAG, "Could not persist retired Mesh address 0x%04x", stored.address);
  }
}

bool SteinelMesh::load_cached_iv_index_(const std::array<uint8_t, 16> &mesh_uuid,
                                            uint32_t &iv_index) {
  StoredIvCache stored{};
  if (!this->iv_cache_preference_.load(&stored) || stored.magic != IV_CACHE_MAGIC ||
      stored.version != IV_CACHE_VERSION || stored.iv_index == 0 ||
      stored.mesh_uuid != mesh_uuid)
    return false;
  iv_index = stored.iv_index;
  return true;
}

void SteinelMesh::remember_iv_index_(const StoredConfig &config) {
  if (config.iv_index == 0 ||
      std::all_of(config.mesh_uuid.begin(), config.mesh_uuid.end(), [](uint8_t value) { return value == 0; }))
    return;
  StoredIvCache stored{};
  stored.iv_index = config.iv_index;
  stored.mesh_uuid = config.mesh_uuid;
  if (!this->iv_cache_preference_.save(&stored))
    ESP_LOGW(WEB_TAG, "Could not persist IV Index cache for the Steinel network");
}

bool SteinelMesh::save_config_(const StoredConfig &config) {
  if (!this->config_preference_.save(&config)) return false;
  this->remember_iv_index_(config);
  this->config_ = config;
  this->configured_ = true;
  this->mesh_mode_enabled_ = (config.flags & (FLAG_ENABLED | FLAG_REMOVE_PENDING)) != 0;
  return true;
}

bool SteinelMesh::save_device_key_(const std::array<uint8_t, 16> &device_key) {
  if (std::all_of(device_key.begin(), device_key.end(), [](uint8_t value) { return value == 0; }))
    return false;
  StoredDeviceKey stored{};
  stored.key = device_key;
  if (!this->device_key_preference_.save(&stored))
    return false;
  this->device_key_ = device_key;
  this->device_key_valid_ = true;
  return true;
}

bool SteinelMesh::save_enabled_(bool enabled) {
  if (!this->configured_) return false;
  StoredConfig updated = this->config_;
  if (enabled)
    updated.flags |= FLAG_ENABLED;
  else
    updated.flags &= static_cast<uint16_t>(~FLAG_ENABLED);
  return this->save_config_(updated);
}

void SteinelMesh::clear_config_() {
  // Clearing Mesh NVS resets the sender sequence. Retire its source address
  // outside Mesh NVS to avoid rejection by peers' Replay Protection Lists.
  this->retire_local_address_();
  this->remember_iv_index_(this->config_);
  StoredConfig empty{};
  empty.magic = 0;
  this->config_preference_.save(&empty);
  StoredDeviceKey empty_key{};
  empty_key.magic = 0;
  this->device_key_preference_.save(&empty_key);
  this->device_key_.fill(0);
  this->device_key_valid_ = false;
  this->catalog_ = protocol::Catalog{};
  this->catalog_.magic = 0;
  this->catalog_preference_.save(&this->catalog_);
  const bool interrupted = false;
  this->import_guard_preference_.save(&interrupted);
  this->catalog_valid_ = false;
  this->clear_advertised_identity_();
  this->composition_received_.store(false);
  this->live_company_id_.store(0);
  this->live_product_id_.store(0);
  this->live_version_id_.store(0);
  this->live_firmware_revision_.store(-1);
  this->live_hardware_revision_.store(-1);
  this->force_actual_output_unavailable_();
  this->configured_ = false;
  this->mesh_mode_enabled_ = false;
  this->mesh_started_ = false;
  this->mesh_ready_.store(false);
  this->live_iv_index_.store(0);
  this->live_iv_index_confirmed_.store(false);
}

void SteinelMesh::set_status_(const std::string &status, bool publish) {
  {
    std::lock_guard<std::mutex> lock(this->state_mutex_);
    this->status_ = status;
  }
  // Queue worker/callback updates for publication in ESPHome's main loop.
  if (publish)
    this->status_publish_pending_.store(true);
}

void SteinelMesh::send_json_(AsyncWebServerRequest *request, int code, const std::string &body) {
  auto *response = request->beginResponse(code, "application/json; charset=utf-8", body);
  response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  request->send(response);
}

bool SteinelMesh::authenticate_(AsyncWebServerRequest *request) const {
  if (request->authenticate(this->web_username_.c_str(), this->web_password_.c_str())) return true;
  request->requestAuthentication();
  return false;
}

bool SteinelMesh::parse_version_(const std::string &value,
                                    std::array<uint16_t, 3> &version) {
  if (value.empty() || value.size() > AUTO_UPDATE_VERSION_MAX_LENGTH)
    return false;
  unsigned major = 0;
  unsigned minor = 0;
  unsigned patch = 0;
  char trailing = '\0';
  if (std::sscanf(value.c_str(), "%u.%u.%u%c", &major, &minor, &patch, &trailing) != 3 ||
      major > UINT16_MAX || minor > UINT16_MAX || patch > UINT16_MAX)
    return false;
  version = {static_cast<uint16_t>(major), static_cast<uint16_t>(minor),
             static_cast<uint16_t>(patch)};
  return true;
}

bool SteinelMesh::parse_sha256_(const std::string &value,
                                   std::array<uint8_t, 32> &digest) {
  if (value.size() != digest.size() * 2)
    return false;
  for (size_t index = 0; index < digest.size(); index++) {
    const unsigned char high = static_cast<unsigned char>(value[index * 2]);
    const unsigned char low = static_cast<unsigned char>(value[index * 2 + 1]);
    if (!std::isxdigit(high) || !std::isxdigit(low))
      return false;
    const char pair[3]{value[index * 2], value[index * 2 + 1], '\0'};
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(pair, &end, 16);
    if (end == nullptr || *end != '\0')
      return false;
    digest[index] = static_cast<uint8_t>(parsed);
  }
  return true;
}

bool SteinelMesh::load_auto_update_() {
  StoredAutoUpdate stored{};
  if (!this->auto_update_preference_.load(&stored) || stored.magic != AUTO_UPDATE_MAGIC ||
      stored.version != AUTO_UPDATE_VERSION || stored.image_size == 0 ||
      stored.target_version[AUTO_UPDATE_VERSION_MAX_LENGTH] != '\0' ||
      stored.url[AUTO_UPDATE_URL_MAX_LENGTH] != '\0')
    return false;

  std::array<uint16_t, 3> parsed{};
  const std::string version(stored.target_version);
  const std::string expected_url = std::string(RELEASE_DOWNLOAD_PREFIX) + version + "/" +
                                   RELEASE_ASSET_PREFIX + version + "-ota.bin";
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!parse_version_(version, parsed) || expected_url != stored.url || partition == nullptr ||
      stored.image_size > partition->size ||
      std::all_of(stored.sha256.begin(), stored.sha256.end(),
                  [](uint8_t value) { return value == 0; })) {
    StoredAutoUpdate empty{};
    empty.magic = 0;
    this->auto_update_preference_.save(&empty);
    global_preferences->sync();
    return false;
  }

  this->auto_update_ = stored;
  this->auto_update_mode_ = true;
  this->auto_update_progress_.store(0);
  return true;
}

bool SteinelMesh::save_auto_update_(const StoredAutoUpdate &update) {
  if (!this->auto_update_preference_.save(&update) || !global_preferences->sync())
    return false;
  this->auto_update_ = update;
  return true;
}

bool SteinelMesh::clear_auto_update_() {
  StoredAutoUpdate empty{};
  empty.magic = 0;
  if (!this->auto_update_preference_.save(&empty) || !global_preferences->sync())
    return false;
  this->auto_update_ = StoredAutoUpdate{};
  return true;
}

void SteinelMesh::fail_auto_update_(const std::string &error) {
  if (!this->clear_auto_update_())
    ESP_LOGE(WEB_TAG, "Could not clear failed automatic update request");
  this->auto_update_running_.store(false);
  this->set_status_("Firmware update failed: " + error + "; restarting current firmware", false);
  this->reboot_at_ = millis() + AUTO_UPDATE_ERROR_REBOOT_DELAY_MS;
  this->reboot_pending_.store(true);
  ESP_LOGE(WEB_TAG, "Automatic firmware update failed: %s", error.c_str());
}

void SteinelMesh::advance_auto_update_() {
  if (this->auto_update_running_.load() || this->reboot_pending_.load())
    return;
  if (!network::is_connected()) {
    this->set_status_("Firmware update pending; waiting for network", false);
    return;
  }

  if (!this->auto_update_api_shutdown_started_) {
    this->auto_update_api_shutdown_started_ = true;
    this->auto_update_stage_deadline_ = millis() + AUTO_UPDATE_STAGE_TIMEOUT_MS;
    if (api::global_api_server != nullptr)
      api::global_api_server->on_shutdown();
    this->set_status_("Preparing automatic firmware update", false);
    return;
  }

  const bool api_released =
      api::global_api_server == nullptr || api::global_api_server->teardown();
  if (!api_released) {
    if (static_cast<int32_t>(millis() - this->auto_update_stage_deadline_) < 0)
      return;
    this->fail_auto_update_("could not close Home Assistant connection");
    return;
  }

  if (!this->auto_update_ble_disable_started_) {
    this->auto_update_ble_disable_started_ = true;
    this->auto_update_stage_deadline_ = millis() + AUTO_UPDATE_STAGE_TIMEOUT_MS;
    if (esp32_ble::global_ble != nullptr)
      esp32_ble::global_ble->disable();
    return;
  }

  const bool bluetooth_released =
      esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED;
  if (!bluetooth_released) {
    if (static_cast<int32_t>(millis() - this->auto_update_stage_deadline_) < 0)
      return;
    this->fail_auto_update_("could not release Bluetooth memory");
    return;
  }

  this->auto_update_running_.store(true);
  this->set_status_("Downloading firmware update", false);
  if (xTaskCreate(auto_update_task_, "steinel_ota", AUTO_UPDATE_TASK_STACK_BYTES,
                  this, 2, nullptr) != pdPASS) {
    this->auto_update_running_.store(false);
    this->fail_auto_update_("could not start download task");
  }
}

esp_err_t SteinelMesh::auto_update_http_event_(esp_http_client_event_t *event) {
  auto *context = static_cast<AutoUpdateContext *>(event->user_data);
  if (context == nullptr)
    return ESP_OK;
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
      event->header_value != nullptr && strcasecmp(event->header_key, "Location") == 0) {
    context->redirect_url = event->header_value;
    return ESP_OK;
  }
  if (!context->accept_firmware_data || event->event_id != HTTP_EVENT_ON_DATA ||
      event->data_len <= 0 || context->sink_error != ESP_OK)
    return ESP_OK;

  const size_t length = static_cast<size_t>(event->data_len);
  if (context->received + length > context->owner->auto_update_.image_size) {
    context->sink_error = ESP_ERR_INVALID_SIZE;
    return context->sink_error;
  }
  context->sink_error = esp_ota_write(context->ota_handle, event->data, length);
  if (context->sink_error != ESP_OK)
    return context->sink_error;
  if (mbedtls_sha256_update(&context->sha256,
                            static_cast<const unsigned char *>(event->data), length) != 0) {
    context->sink_error = ESP_FAIL;
    return context->sink_error;
  }
  context->received += length;
  context->owner->auto_update_progress_.store(static_cast<uint8_t>(
      std::min<size_t>(99, context->received * 100 / context->owner->auto_update_.image_size)));
  return ESP_OK;
}

void SteinelMesh::auto_update_task_(void *parameter) {
  auto *self = static_cast<SteinelMesh *>(parameter);
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (partition == nullptr || self->auto_update_.image_size > partition->size) {
    self->fail_auto_update_("no safe OTA partition is available");
    vTaskDelete(nullptr);
    return;
  }

  AutoUpdateContext context{};
  context.owner = self;
  mbedtls_sha256_init(&context.sha256);
  if (mbedtls_sha256_starts(&context.sha256, 0) != 0 ||
      esp_ota_begin(partition, self->auto_update_.image_size, &context.ota_handle) != ESP_OK) {
    mbedtls_sha256_free(&context.sha256);
    self->fail_auto_update_("could not prepare inactive firmware partition");
    vTaskDelete(nullptr);
    return;
  }

  esp_http_client_config_t config{};
  config.url = self->auto_update_.url;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 120000;
  config.buffer_size = 2048;
  config.buffer_size_tx = 512;
  config.event_handler = auto_update_http_event_;
  config.user_data = &context;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  config.disable_auto_redirect = true;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client != nullptr) {
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Connection", "close");
  }
  const esp_err_t redirect_result =
      client == nullptr ? ESP_ERR_NO_MEM : esp_http_client_perform(client);
  const int redirect_status = client == nullptr ? 0 : esp_http_client_get_status_code(client);
  if (client != nullptr)
    esp_http_client_cleanup(client);

  const bool redirect_status_valid = redirect_status == 301 || redirect_status == 302 ||
                                     redirect_status == 303 || redirect_status == 307 ||
                                     redirect_status == 308;
  const bool redirect_url_valid =
      context.redirect_url.size() <= AUTO_UPDATE_MAX_REDIRECT_URL_LENGTH &&
      context.redirect_url.compare(0, std::strlen(RELEASE_ASSET_REDIRECT_PREFIX),
                                   RELEASE_ASSET_REDIRECT_PREFIX) == 0;
  if (redirect_result != ESP_OK || !redirect_status_valid || !redirect_url_valid) {
    esp_ota_abort(context.ota_handle);
    mbedtls_sha256_free(&context.sha256);
    self->fail_auto_update_("invalid GitHub release redirect: result " +
                            std::to_string(static_cast<int32_t>(redirect_result)) +
                            ", HTTP " + std::to_string(redirect_status));
    vTaskDelete(nullptr);
    return;
  }

  context.accept_firmware_data = true;
  config.url = context.redirect_url.c_str();
  config.buffer_size_tx = static_cast<int>(std::max<size_t>(
      512, context.redirect_url.size() + AUTO_UPDATE_REQUEST_OVERHEAD_BYTES));
  client = esp_http_client_init(&config);
  if (client != nullptr) {
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Connection", "close");
  }
  esp_err_t result = client == nullptr ? ESP_ERR_NO_MEM : esp_http_client_perform(client);
  const int http_status = client == nullptr ? 0 : esp_http_client_get_status_code(client);
  int tls_code = 0;
  int tls_flags = 0;
  const int socket_errno = client == nullptr ? 0 : esp_http_client_get_errno(client);
  const esp_err_t tls_error =
      client == nullptr
          ? ESP_ERR_NO_MEM
          : esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
  if (client != nullptr)
    esp_http_client_cleanup(client);

  std::array<uint8_t, 32> digest{};
  const int digest_result = mbedtls_sha256_finish(&context.sha256, digest.data());
  mbedtls_sha256_free(&context.sha256);
  if (result != ESP_OK || http_status != 200 || context.sink_error != ESP_OK ||
      context.received != self->auto_update_.image_size || digest_result != 0 ||
      digest != self->auto_update_.sha256) {
    esp_ota_abort(context.ota_handle);
    ESP_LOGE(WEB_TAG,
             "Firmware download failed: result=%s, HTTP=%d, received=%u/%u, sink=%s, "
             "SHA result=%d, TLS error=%d, TLS code=%d, TLS flags=0x%X, errno=%d",
             esp_err_to_name(result), http_status, static_cast<unsigned>(context.received),
             static_cast<unsigned>(self->auto_update_.image_size),
             esp_err_to_name(context.sink_error), digest_result, static_cast<int>(tls_error),
             tls_code, tls_flags, socket_errno);
    if (context.sink_error != ESP_OK)
      self->fail_auto_update_(std::string("flash write failed: ") +
                              esp_err_to_name(context.sink_error));
    else if (result != ESP_OK)
      self->fail_auto_update_(std::string("HTTPS download failed: ") + esp_err_to_name(result) +
                              " (" + std::to_string(static_cast<int32_t>(result)) +
                              "), HTTP " + std::to_string(http_status) + ", received " +
                              std::to_string(context.received) + " of " +
                              std::to_string(self->auto_update_.image_size) + " bytes, TLS " +
                              std::to_string(static_cast<int32_t>(tls_error)) + "/" +
                              std::to_string(tls_code) + ", flags " +
                              std::to_string(tls_flags) + ", errno " +
                              std::to_string(socket_errno));
    else if (http_status != 200)
      self->fail_auto_update_("firmware server returned HTTP " + std::to_string(http_status));
    else if (context.received != self->auto_update_.image_size)
      self->fail_auto_update_("download was incomplete: " +
                              std::to_string(context.received) + " of " +
                              std::to_string(self->auto_update_.image_size) + " bytes");
    else if (digest_result != 0)
      self->fail_auto_update_("could not calculate firmware SHA-256");
    else if (digest != self->auto_update_.sha256)
      self->fail_auto_update_("SHA-256 verification failed");
    else
      self->fail_auto_update_("firmware download failed");
    vTaskDelete(nullptr);
    return;
  }

  result = esp_ota_end(context.ota_handle);
  if (result != ESP_OK) {
    self->fail_auto_update_(std::string("firmware image validation failed: ") +
                            esp_err_to_name(result));
    vTaskDelete(nullptr);
    return;
  }
  if (!self->clear_auto_update_()) {
    self->fail_auto_update_("could not clear the saved update request");
    vTaskDelete(nullptr);
    return;
  }
  result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) {
    self->fail_auto_update_(std::string("could not activate new firmware: ") +
                            esp_err_to_name(result));
    vTaskDelete(nullptr);
    return;
  }

  self->auto_update_progress_.store(100);
  self->auto_update_running_.store(false);
  self->set_status_("Firmware update verified; restarting gateway", false);
  self->reboot_at_ = millis() + 1500;
  self->reboot_pending_.store(true);
  vTaskDelete(nullptr);
}

bool SteinelMesh::canHandle(AsyncWebServerRequest *request) const {
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(url_buffer);
  if (request->method() == HTTP_GET)
    return url == "/" || url == "/steinel" || url == "/steinel/status" || url == "/api/nodes" || url == "/api/diagnostics" || url == "/api/state-trace";
  return request->method() == HTTP_POST &&
         (url == "/steinel/discover" || url == "/steinel/install" || url == "/steinel/enable" ||
          url == "/steinel/disable" || url == "/steinel/remove" ||
          url == "/steinel/mode" || url == "/steinel/threshold" ||
          url == "/steinel/refresh" || url == "/steinel/password" ||
          url == "/steinel/wifi" || url == "/steinel/update" ||
          url == "/steinel/factory-reset" || url == "/steinel/selection" ||
          url == "/steinel/node" || url == "/steinel/import" || url == "/steinel/diagnostics" || url == "/api/state-trace");
}

void SteinelMesh::handleRequest(AsyncWebServerRequest *request) {
  if (!this->authenticate_(request)) return;
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(url_buffer);
  if (url == "/" || url == "/steinel") return this->handle_index_(request);
  if (url == "/steinel/status") return this->handle_status_(request);
  if (url == "/api/nodes") return this->handle_nodes_(request);
  if (url == "/api/diagnostics") return this->handle_diagnostics_(request);
  if (url == "/api/state-trace") return this->handle_state_trace_(request);
  if (url == "/steinel/diagnostics") return this->handle_diagnostic_session_(request);
  if (url == "/steinel/selection") return this->handle_selection_(request);
  if (url == "/steinel/node") return this->handle_node_control_(request);
  if (url == "/steinel/import") return this->handle_local_import_(request);
  if (url == "/steinel/discover") return this->handle_discover_(request);
  if (url == "/steinel/install") return this->handle_install_(request);
  if (url == "/steinel/enable") return this->handle_enable_(request);
  if (url == "/steinel/disable") return this->handle_disable_(request);
  if (url == "/steinel/remove") return this->handle_remove_(request);
  if (url == "/steinel/mode") return this->handle_mode_(request);
  if (url == "/steinel/threshold") return this->handle_threshold_(request);
  if (url == "/steinel/refresh") return this->handle_refresh_(request);
  if (url == "/steinel/password") return this->handle_password_(request);
  if (url == "/steinel/wifi") return this->handle_wifi_(request);
  if (url == "/steinel/update") return this->handle_auto_update_(request);
  if (url == "/steinel/factory-reset") return this->handle_factory_reset_(request);
  send_json_(request, 404, "{\"message\":\"Not found\"}");
}

void SteinelMesh::handle_index_(AsyncWebServerRequest *request) {
  auto *response = request->beginResponse(200, "text/html; charset=utf-8", STEINEL_PAGE_GZ,
                                          sizeof(STEINEL_PAGE_GZ));
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  request->send(response);
}

void SteinelMesh::handle_nodes_(AsyncWebServerRequest *request) {
  auto *raw = static_cast<httpd_req_t *>(*request);
  httpd_resp_set_type(raw, "application/json; charset=utf-8");
  httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
  StatusJsonWriter json(raw);
  json.append("{\"version\":2,\"nodes\":[");
  for (size_t i = 0; this->catalog_valid_ && i < this->catalog_.count; ++i) {
    const auto &node = this->catalog_.nodes[i];
    NodeState state;
    { std::lock_guard<std::mutex> lock(this->node_mutex_); state = this->node_states_[i]; }
    uint16_t caps = 0;
    for (size_t j = 0; j < node.element_count; ++j) caps |= node.elements[j].capabilities;
    if (i != 0) json.append(",");
    json.number("{\"address\":", node.address);
    json.append(",\"name\":\""); json.escaped(node.name); json.append("\"");
    const auto product = state.composition_matches ? state.product : node.product_id;
    const auto company = state.composition_matches ? state.company : node.company_id;
    json.number(",\"product\":", product);
    json.number(",\"company\":", company);
    json.append(",\"manufacturer\":\"");
    if (company == 0x0563) json.append("Steinel GmbH");
    json.append("\"");
    json.number(",\"composition_vid\":", state.composition_checked ? state.composition_version : 0);
    json.append(",\"model\":\""); json.escaped(protocol::product_name(company, product)); json.append("\"");
    json.append(",\"uuid\":\"");
    const std::array<uint8_t, 16> empty{};
    if (node.uuid != empty) {
      char hex[3];
      for (auto byte : node.uuid) { std::snprintf(hex, sizeof(hex), "%02x", byte); json.append(hex); }
    }
    json.append("\",\"firmware\":\""); json.escaped(state.firmware);
    json.append("\",\"hardware\":\""); json.escaped(state.hardware); json.append("\"");
    json.number(",\"functions\":", state.supported & protocol::possible_functions(node));
    const int mode = state.mode.value;
    json.signed_number(",\"mode\":", mode);
    json.number(",\"capabilities\":", caps);
    json.number(",\"elements\":", node.element_count);
    json.append(",\"selected\":"); json.append(node.selected ? "true" : "false");
    const uint32_t now = millis();
    const bool allowed = protocol::composition_allows(state.composition_checked, state.composition_matches);
    const bool available = allowed && state.last_seen != 0 && now - state.last_seen < state.stale_after;
    json.append(",\"available\":"); json.append(available ? "true" : "false");
    json.append(",\"verified\":"); json.append(state.composition_matches ? "true" : "false");
    json.append(",\"controls_allowed\":"); json.append(allowed ? "true" : "false");
    json.append(",\"composition_checked\":"); json.append(state.composition_checked ? "true" : "false");
    json.number(",\"stale_after_ms\":", state.stale_after);
    json.signed_number(",\"output\":", state.on);
    json.signed_number(",\"automatic\":", state.automatic);
    json.signed_number(",\"motion\":", state.motion);
    json.number(",\"lux\":", state.lux);
    json.number(",\"threshold\":", state.threshold);
    json.number(",\"run_time\":", state.run_time);
    json.signed_number(",\"brightness\":", state.brightness_known ? state.brightness : -1);
    json.number(",\"pending_controls\":", state.controls.pending);
    json.append(",\"pending_values\":[");
    for (size_t field = 0; field < state.controls.values.size(); ++field) {
      if (field) json.append(",");
      json.number("", state.controls.values[field]);
    }
    json.append("]");
    json.append(",\"sensor_overflow\":"); json.append(state.sensors.overflow ? "true" : "false");
    json.append(",\"sensors\":[");
    for (size_t j = 0; j < state.sensors.count; ++j) {
      const auto &value = state.sensors.values[j];
      if (j) json.append(",");
      json.number("{\"element\":", value.element);
      json.number(",\"address\":", node.address + value.element);
      json.number(",\"property\":", value.property);
      json.number(",\"length\":", value.length);
      json.number(",\"age_seconds\":", (now - value.seen) / 1000);
      json.append(",\"available\":");
      json.append(available && value.length && now - value.seen < state.stale_after ? "true" : "false");
      json.append(",\"truncated\":"); json.append(value.length > value.raw.size() ? "true" : "false");
      json.append(",\"raw\":\"");
      for (size_t k = 0; k < std::min<size_t>(value.length, value.raw.size()); ++k) {
        char byte[3]; std::snprintf(byte, sizeof(byte), "%02x", value.raw[k]); json.append(byte);
      }
      json.append("\"}");
    }
    json.append("]");
    json.append("}");
  }
  json.append("]}"); json.finish();
}

void SteinelMesh::handle_diagnostic_session_(AsyncWebServerRequest *request) {
  const std::string action = request->arg("action").c_str();
  const std::string parameter = request->arg(action == "stop" ? "id" : "address").c_str();
  uint32_t value;
  if (!protocol::decimal_u32(parameter.data(), parameter.size(), 1, action == "stop" ? UINT32_MAX : 0x7FFF, value))
    return send_json_(request, 400, "{\"message\":\"Invalid diagnostic request\"}");
  if (action == "stop") {
    bool matches;
    {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      matches = this->device_diagnostics_.id == value;
      if (matches) {
        this->device_diagnostics_.running = false;
        this->clear_diagnostic_groups_();
      }
    }
    if (!matches) return send_json_(request, 409, "{\"message\":\"Diagnostic session changed\"}");
    return send_json_(request, 200, "{\"message\":\"Diagnostic collection stopped\"}");
  }
  if (action != "start") return send_json_(request, 400, "{\"message\":\"Invalid diagnostic action\"}");
  if (!this->catalog_valid_ || !this->mesh_ready_.load() || this->cloud_busy_.load() ||
      this->reboot_pending_.load() || this->auto_update_running_.load())
    return send_json_(request, 409, "{\"message\":\"Diagnostics require active Mesh\"}");
  uint32_t id = 0;
  int status = 404;
  const char *error = "{\"message\":\"Device is not in the imported catalog\"}";
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    if (this->device_diagnostics_.active(millis())) {
      status = 409; error = "{\"message\":\"Another diagnostic session is running\"}";
    } else for (size_t i = 0; i < this->catalog_.count; ++i) {
      if (this->catalog_.nodes[i].address != value) continue;
      status = 503;
      if (!this->restore_diagnostic_node_(i))
        error = "{\"message\":\"Could not prepare device diagnostics\"}";
      else if (!this->prepare_diagnostic_groups_(this->catalog_.nodes[i]))
        error = "{\"message\":\"Could not subscribe diagnostic publication groups\"}";
      else {
        if (!this->diagnostic_session_id_) this->diagnostic_session_id_ = esp_random();
        id = ++this->diagnostic_session_id_; if (!id) id = ++this->diagnostic_session_id_;
        this->device_diagnostics_.start(millis(), id, i, value, this->catalog_.nodes[i].element_count);
      }
      break;
    }
  }
  if (id) return send_json_(request, 200, "{\"id\":" + std::to_string(id) + "}");
  send_json_(request, status, error);
}

void SteinelMesh::handle_diagnostics_(AsyncWebServerRequest *request) {
  const std::string parameter = request->arg("id").c_str(); uint32_t id;
  if (!protocol::decimal_u32(parameter.data(), parameter.size(), 1, UINT32_MAX, id))
    return send_json_(request, 400, "{\"message\":\"Invalid diagnostic session\"}");
  std::unique_ptr<protocol::DeviceDiagnostics> snapshot(new (std::nothrow) protocol::DeviceDiagnostics);
  if (!snapshot) return send_json_(request, 503, "{\"message\":\"Not enough memory\"}");
  auto &session = *snapshot;
  char firmware[17]{}, hardware[17]{};
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_); session = this->device_diagnostics_;
    if (session.node < this->catalog_.count) {
      std::memcpy(firmware, this->node_states_[session.node].firmware, sizeof(firmware));
      std::memcpy(hardware, this->node_states_[session.node].hardware, sizeof(hardware));
      firmware[sizeof(firmware) - 1] = '\0'; hardware[sizeof(hardware) - 1] = '\0';
    }
  }
  if (session.id != id || !this->catalog_valid_ || session.node >= this->catalog_.count)
    return send_json_(request, 409, "{\"message\":\"Diagnostic session changed\"}");
  const auto &node = this->catalog_.nodes[session.node];
  auto *raw = static_cast<httpd_req_t *>(*request);
  httpd_resp_set_type(raw, "application/json; charset=utf-8");
  httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
  StatusJsonWriter json(raw);
  const auto hex = [&](const uint8_t *data, size_t length) {
    char byte[3];
    for (size_t i = 0; i < length; ++i) { std::snprintf(byte, sizeof(byte), "%02x", data[i]); json.append(byte); }
  };
  json.append("{\"schema\":\"steinel-device-diagnostics/1\",\"gateway_version\":\"");
  json.escaped(ESPHOME_PROJECT_VERSION); json.append("\"");
  json.number(",\"id\":", session.id); json.number(",\"elapsed_ms\":", std::min(millis() - session.started, protocol::DeviceDiagnostics::DURATION_MS));
  json.append(",\"running\":"); json.append(session.active(millis()) ? "true" : "false");
  json.append(",\"probe_complete\":"); json.append(session.probe_complete ? "true" : "false");
  json.number(",\"sequence\":", session.sequence);
  json.number(",\"company\":", node.company_id); json.number(",\"product\":", node.product_id);
  json.append(",\"firmware\":\""); json.escaped(firmware); json.append("\",\"hardware\":\"");
  json.escaped(hardware); json.append("\"");
  json.append(",\"elements\":[");
  for (size_t i = 0; i < node.element_count; ++i) {
    if (i) json.append(",");
    json.number("{\"index\":", i); json.number(",\"capabilities\":", node.elements[i].capabilities);
    json.append(",\"bound\":"); json.append(node.elements[i].app_key == this->config_.app_key_index ? "true" : "false");
    json.append("}");
  }
  json.append("],\"composition\":{"); json.number("\"length\":", session.composition_length);
  json.append(",\"valid\":"); json.append(session.composition_valid ? "true" : "false");
  json.append(",\"truncated\":"); json.append(session.composition_length > session.composition_data.size() ? "true" : "false");
  json.append(",\"raw\":\""); hex(session.composition_data.data(), std::min<size_t>(session.composition_length, session.composition_data.size()));
  json.append("\"},\"events\":[");
  for (size_t i = 0; i < session.count; ++i) {
    const auto &packet = session.packet(i); if (i) json.append(",");
    json.number("{\"sequence\":", packet.sequence); json.number(",\"elapsed_ms\":", packet.elapsed_ms);
    json.number(",\"element\":", packet.element); json.number(",\"event\":", packet.event);
    json.number(",\"opcode\":", packet.opcode); json.number(",\"property\":", packet.property);
    json.signed_number(",\"error\":", packet.error); json.number(",\"length\":", packet.length);
    json.append(",\"truncated\":"); json.append(packet.length > packet.stored ? "true" : "false");
    json.append(",\"raw\":\""); hex(packet.raw.data(), packet.stored); json.append("\"}");
  }
  json.append("]}"); json.finish();
}

void SteinelMesh::handle_state_trace_(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_POST) {
    if (request->arg("action") != "reset")
      return send_json_(request, 400, "{\"message\":\"Invalid trace action\"}");
    {
      std::lock_guard<std::mutex> lock(this->node_mutex_);
      this->state_trace_.clear();
    }
    return send_json_(request, 200, "{\"message\":\"State trace reset\"}");
  }
  std::unique_ptr<protocol::StateTrace> snapshot(new (std::nothrow) protocol::StateTrace);
  if (!snapshot) return send_json_(request, 503, "{\"message\":\"Not enough memory\"}");
  {
    std::lock_guard<std::mutex> lock(this->node_mutex_);
    *snapshot = this->state_trace_;
  }
  const auto &trace = *snapshot;
  auto *raw = static_cast<httpd_req_t *>(*request);
  httpd_resp_set_type(raw, "application/json; charset=utf-8");
  httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
  StatusJsonWriter json(raw);
  json.append("{\"schema\":\"steinel-state-trace/1\",\"gateway_version\":\"");
  json.escaped(ESPHOME_PROJECT_VERSION); json.append("\"");
  json.number(",\"uptime_ms\":", millis()); json.number(",\"sequence\":", trace.sequence);
  json.number(",\"incident_at_ms\":", trace.incident_at); json.number(",\"incident_node\":", trace.incident_node);
  json.append(",\"captured\":"); json.append(trace.captured ? "true" : "false");
  json.append(",\"frozen\":"); json.append(trace.frozen ? "true" : "false");
  json.append(",\"events\":[");
  for (size_t i = 0; i < trace.count; ++i) {
    const auto &entry = trace.entry(i); if (i) json.append(",");
    json.number("{\"at_ms\":", entry.at); json.number(",\"node\":", entry.node);
    json.number(",\"address\":", entry.address); json.number(",\"event\":", entry.event);
    json.number(",\"sdk_event\":", entry.sdk_event); json.number(",\"opcode\":", entry.opcode);
    json.number(",\"request\":", entry.request); json.number(",\"kind\":", entry.kind);
    json.number(",\"step\":", entry.step); json.number(",\"tid\":", entry.tid);
    json.signed_number(",\"error\":", entry.error);
    json.signed_number(",\"before_on\":", entry.before_on); json.signed_number(",\"after_on\":", entry.after_on);
    json.signed_number(",\"before_auto\":", entry.before_auto); json.signed_number(",\"after_auto\":", entry.after_auto);
    json.append(",\"raw\":\"");
    char byte[3];
    for (size_t j = 0; j < entry.length; ++j) {
      std::snprintf(byte, sizeof(byte), "%02x", entry.raw[j]); json.append(byte);
    }
    json.append("\"}");
  }
  json.append("]}"); json.finish();
}

void SteinelMesh::handle_selection_(AsyncWebServerRequest *request) {
  if (!this->catalog_valid_ || this->cloud_busy_.load() || this->reboot_pending_.load() || this->auto_update_running_.load())
    return send_json_(request, 409, "{\"message\":\"Device selection is not available\"}");
  const std::string addresses = request->arg("addresses").c_str();
  auto *updated = new (std::nothrow) protocol::Catalog(this->catalog_);
  if (updated == nullptr) return send_json_(request, 503, "{\"message\":\"Not enough memory\"}");
  for (auto &node : updated->nodes) node.selected = false;
  bool valid = addresses.size() <= 128;
  size_t start = 0, selected = 0;
  while (valid && start < addresses.size()) {
    const auto end = addresses.find(',', start);
    const std::string item = addresses.substr(start, end == std::string::npos ? end : end - start);
    uint32_t address = 0;
    valid = !item.empty() && this->parse_u32_(item, 1, 0x7FFF, address);
    bool found = false;
    for (size_t i = 0; valid && i < updated->count; ++i) {
      auto &node = updated->nodes[i];
      if (node.address != address) continue;
      const bool supported = protocol::element_with(node, protocol::ONOFF | protocol::LIGHTNESS | protocol::LC | protocol::SENSOR) >= 0;
      found = supported && !node.selected;
      if (found) { node.selected = true; ++selected; }
      break;
    }
    valid &= found;
    if (end == std::string::npos) break;
    start = end + 1;
    if (start == addresses.size()) valid = false;
  }
  std::array<uint16_t, protocol::MAX_GROUPS> groups{}; size_t group_count = 0;
  for (size_t i = 0; valid && i < updated->count; ++i)
    if (updated->nodes[i].selected)
      for (size_t j = 0; valid && j < updated->nodes[i].element_count; ++j)
        valid = protocol::add_group(groups, group_count, updated->nodes[i].elements[j].sensor_group);
  if (!valid) { delete updated; return send_json_(request, 400, "{\"message\":\"Invalid selection or too many publication groups\"}"); }
  // Do not modify a live catalog: callbacks and entity names retain its data.
  bool saved = this->save_catalog_(*updated);
  if (saved && !this->save_enabled_(selected != 0)) {
    this->save_catalog_(this->catalog_);
    saved = false;
  }
  saved = saved && global_preferences->sync();
  delete updated;
  if (!saved) return send_json_(request, 500, "{\"message\":\"Could not save device selection\"}");
  this->reboot_at_ = millis() + 1500; this->reboot_pending_.store(true);
  send_json_(request, 200, "{\"message\":\"Selection saved. Restarting gateway to update Home Assistant devices.\"}");
}

void SteinelMesh::handle_node_control_(AsyncWebServerRequest *request) {
  uint32_t address = 0, value = 0;
  const std::string command = request->arg("command").c_str();
  NodeCommand kind;
  if (command == "output") kind = NodeCommand::ONOFF;
  else if (command == "brightness") kind = NodeCommand::BRIGHTNESS;
  else if (command == "automatic") kind = NodeCommand::AUTO;
  else if (command == "threshold") kind = NodeCommand::THRESHOLD;
  else if (command == "run_time") kind = NodeCommand::RUN_TIME;
  else if (command == "mode") kind = NodeCommand::MODE;
  else return send_json_(request, 400, "{\"message\":\"Unknown command\"}");
  if (request->arg("address").empty() || request->arg("value").empty() ||
      !this->parse_u32_(request->arg("address").c_str(), 1, 0x7FFF, address) ||
      !this->parse_u32_(request->arg("value").c_str(), 0, 0xFFFFFE, value))
    return send_json_(request, 400, "{\"message\":\"Invalid command parameters\"}");
  if (!this->queue_node_command(address, kind, value))
    return send_json_(request, 409, "{\"message\":\"Device is not verified/available or command queue is full\"}");
  send_json_(request, 200, "{\"message\":\"Command queued; waiting for the device's confirmed state\"}");
}

void SteinelMesh::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                              size_t index, size_t total) {
  char path[AsyncWebServerRequest::URL_BUF_SIZE];
  if (request->url_to(path) != "/steinel/import") return;
  std::lock_guard<std::mutex> lock(this->state_mutex_);
  if (index == 0) {
    if (!request->authenticate(this->web_username_.c_str(), this->web_password_.c_str())) return;
    if (this->mesh_mode_enabled_ || this->reboot_pending_.load() || this->auto_update_running_.load()) return;
    bool expected = false;
    if (!this->cloud_busy_.compare_exchange_strong(expected, true)) return;
    this->upload_error_.clear();
    this->upload_body_ = new (std::nothrow) CloudBody{};
    this->upload_request_ = request;
    this->upload_receiving_ = true;
    if (this->upload_body_ == nullptr || !this->upload_body_->prepare(true, this->upload_error_))
      this->upload_error_ = "Could not prepare backup workspace";
    else if (total == 0 || total > this->upload_body_->limit()) this->upload_error_ = "Backup is too large";
  }
  if (!this->upload_receiving_ || this->upload_request_ != request) return;
  this->upload_last_chunk_at_ = millis();
  if (this->upload_error_.empty() && (index != this->upload_body_->length ||
      !this->upload_body_->append(data, len))) this->upload_error_ = "Backup upload failed";
}

void SteinelMesh::handle_local_import_(AsyncWebServerRequest *request) {
  std::lock_guard<std::mutex> lock(this->state_mutex_);
  if (!this->upload_receiving_ || this->upload_request_ != request)
    return send_json_(request, 409, "{\"message\":\"Disable Mesh before importing; another operation may be running\"}");
  this->upload_receiving_ = false; this->upload_request_ = nullptr;
  if (!this->upload_error_.empty()) {
    delete this->upload_body_; this->upload_body_ = nullptr; this->cloud_busy_.store(false);
    return send_json_(request, 400, "{\"message\":\"Backup upload failed or exceeds workspace\"}");
  }
  // Parsing on a worker keeps its stack off the small HTTP server stack.
  if (xTaskCreate(local_import_task_, "mesh_import", CLOUD_TASK_STACK_BYTES, this, 1, nullptr) != pdPASS) {
    delete this->upload_body_; this->upload_body_ = nullptr; this->cloud_busy_.store(false);
    return send_json_(request, 503, "{\"message\":\"Not enough memory for backup import\"}");
  }
  send_json_(request, 200, "{\"message\":\"Validating backup. Select devices after the gateway restarts.\"}");
}

void SteinelMesh::expire_upload_() {
  std::lock_guard<std::mutex> lock(this->state_mutex_);
  if (!this->upload_receiving_ || millis() - this->upload_last_chunk_at_ < 30000) return;
  delete this->upload_body_; this->upload_body_ = nullptr;
  this->upload_receiving_ = false; this->upload_request_ = nullptr;
  this->cloud_busy_.store(false);
}

void SteinelMesh::local_import_task_(void *parameter) {
  auto *self = static_cast<SteinelMesh *>(parameter);
  std::string error;
  const bool installed = self->install_backup_(*self->upload_body_, 0, 0, error);
  delete self->upload_body_; self->upload_body_ = nullptr;
  self->set_status_(installed ? "Backup imported; restarting for device selection" : error);
  if (installed) { self->reboot_at_ = millis() + 1500; self->reboot_pending_.store(true); }
  self->cloud_busy_.store(false);
  vTaskDelete(nullptr);
}

void SteinelMesh::handle_status_(AsyncWebServerRequest *request) {
  httpd_req_t *raw_request = static_cast<httpd_req_t *>(*request);
  httpd_resp_set_status(raw_request, HTTPD_200);
  httpd_resp_set_type(raw_request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(raw_request, "Pragma", "no-cache");

  std::string status;
  std::vector<NetworkChoice> networks;
  {
    std::lock_guard<std::mutex> lock(this->state_mutex_);
    status = this->status_;
    networks = this->networks_;
  }
  StatusJsonWriter body(raw_request);
  body.append("{\"configured\":");
  body.append(this->configured_ ? "true" : "false");
  body.append(",\"legacy_profile\":");
  body.append(this->legacy_profile_ ? "true" : "false");
  body.append(",\"enabled\":");
  body.append(this->mesh_mode_enabled_ ? "true" : "false");
  body.append(",\"busy\":");
  body.append(this->cloud_busy_.load() || this->auto_update_running_.load() ? "true" : "false");
  body.append(",\"mesh_ready\":");
  body.append(this->mesh_ready_.load() ? "true" : "false");
  body.append(",\"composition_received\":");
  body.append(this->composition_received_.load() ? "true" : "false");
  body.append(",\"runtime_mode\":\"");
  body.append(this->auto_update_mode_ ? "Firmware Update" :
              this->mesh_mode_enabled_ ? "Bluetooth Mesh" : "Setup");
  body.append("\",\"message\":\"");
  body.escaped(status.c_str());
  body.append("\"");
  body.append(",\"factory_password\":");
  body.append(this->using_factory_admin_password_ ? "true" : "false");
  body.append(",\"connected_ssid\":\"");
  if (wifi::global_wifi_component != nullptr) {
    const auto configured_sta = wifi::global_wifi_component->get_sta();
    body.escaped(configured_sta.get_ssid().c_str());
  }
  body.append("\"");
  body.append(",\"gateway_version\":\"");
  body.append(ESPHOME_PROJECT_VERSION);
  body.append("\",\"web_revision\":\"");
  body.append(STEINEL_PAGE_REVISION);
  body.append("\",\"restarting\":");
  body.append(this->reboot_pending_.load() ? "true" : "false");
  body.append(",\"starting\":");
  body.append(this->identity_scan_pending_.load() || this->mesh_start_pending_ ? "true" : "false");
  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  esp_ota_img_states_t running_image_state = ESP_OTA_IMG_UNDEFINED;
  const bool firmware_pending_validation =
      running_partition != nullptr &&
      esp_ota_get_state_partition(running_partition, &running_image_state) == ESP_OK &&
      running_image_state == ESP_OTA_IMG_PENDING_VERIFY;
  body.append(",\"firmware_pending_validation\":");
  body.append(firmware_pending_validation ? "true" : "false");
  body.append(",\"auto_update_mode\":");
  body.append(this->auto_update_mode_ ? "true" : "false");
  body.append(",\"auto_update_running\":");
  body.append(this->auto_update_running_.load() ? "true" : "false");
  body.number(",\"auto_update_progress\":", this->auto_update_progress_.load());
#ifdef USE_STEINEL_EXTENDED_DIAGNOSTICS
  body.append(",\"extended_diagnostics\":true");
  body.number(",\"free_internal_heap\":",
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  body.number(",\"largest_internal_block\":",
              heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  body.number(",\"gateway_uptime_seconds\":", millis() / 1000U);
  body.append(",\"gateway_reset_reason\":\"");
  body.escaped(reset_reason_name(esp_reset_reason()));
  body.append("\"");
  body.number(",\"mesh_tx_attempts\":", this->mesh_tx_attempts_.load());
  body.number(",\"mesh_tx_accepted\":", this->mesh_tx_accepted_.load());
  body.number(",\"mesh_tx_errors\":", this->mesh_tx_errors_.load());
  body.signed_number(",\"mesh_last_tx_error\":", this->mesh_last_tx_error_.load());
  body.number(",\"mesh_rx_messages\":", this->mesh_rx_messages_.load());
  body.number(",\"mesh_timeouts\":", this->mesh_timeouts_.load());
  body.append(",\"mesh_rssi_received\":");
  body.append(this->mesh_rssi_received_.load() ? "true" : "false");
  body.signed_number(",\"mesh_last_rssi_dbm\":", this->last_mesh_rssi_dbm_.load());
  const uint32_t mesh_rssi_at = this->last_mesh_rssi_at_.load();
  body.number(",\"mesh_last_rssi_age_seconds\":",
              mesh_rssi_at == 0 ? 0 : (millis() - mesh_rssi_at) / 1000U);
#else
  body.append(",\"extended_diagnostics\":false");
#endif
  body.append(",\"lux_received\":");
  body.append(this->lux_received_.load() ? "true" : "false");
  body.number(",\"last_lux_centilux\":", this->pending_lux_centilux_.load());
  body.append(",\"threshold_received\":");
  body.append(this->threshold_received_.load() ? "true" : "false");
  body.number(",\"threshold_centilux\":", this->pending_threshold_centilux_.load());
  body.append(",\"actual_output_known\":");
  body.append(this->observed_onoff_.load() >= 0 ? "true" : "false");
  body.append(",\"actual_output_on\":");
  body.append(this->observed_onoff_.load() > 0 ? "true" : "false");
  int8_t current_mode = -1;
  if (this->mode_override_pending_.load()) {
    current_mode = this->requested_mode_.load();
  } else if (this->observed_lc_mode_.load() == 1) {
    current_mode = 0;
  } else if (this->observed_lc_mode_.load() == 0 && this->observed_onoff_.load() >= 0) {
    current_mode = this->observed_onoff_.load() > 0 ? 1 : 2;
  }
  body.append(",\"mode_known\":");
  body.append(current_mode >= 0 ? "true" : "false");
  body.append(",\"mode\":\"");
  if (current_mode == 0)
    body.append("Auto");
  else if (current_mode == 1)
    body.append("Always On");
  else if (current_mode == 2)
    body.append("Always Off");
  body.append("\"");
  body.append(",\"iv_index_confirmed\":");
  body.append(this->live_iv_index_confirmed_.load() ? "true" : "false");
  if (this->composition_received_.load()) {
    char identity[80];
    const int length = std::snprintf(identity, sizeof(identity),
                                     ",\"company_id\":\"%04x\",\"product_id\":\"%04x\"",
                                     this->live_company_id_.load(), this->live_product_id_.load());
    if (length > 0 && static_cast<size_t>(length) < sizeof(identity))
      body.append(std::string_view(identity, static_cast<size_t>(length)));
  }
  uint8_t firmware_major = 0;
  uint8_t firmware_minor = 0;
  uint8_t firmware_patch = 0;
  const bool firmware_known =
      this->resolve_firmware_version_(firmware_major, firmware_minor, firmware_patch);
  body.append(",\"firmware_version_known\":");
  body.append(firmware_known ? "true" : "false");
  body.append(",\"firmware_version\":\"");
  if (firmware_known) {
    char firmware[16];
    const int length = std::snprintf(firmware, sizeof(firmware), "%u.%u.%u",
                                     firmware_major, firmware_minor, firmware_patch);
    if (length > 0 && static_cast<size_t>(length) < sizeof(firmware))
      body.append(std::string_view(firmware, static_cast<size_t>(length)));
  }
  body.append("\"");
  const bool hardware_known = this->advertised_identity_current_();
  const uint8_t hardware = this->advertised_hardware_version_.load();
  body.append(",\"hardware_version_known\":");
  body.append(hardware_known ? "true" : "false");
  body.number(",\"hardware_version\":", hardware);
  if (this->configured_) {
    char values[96];
    std::snprintf(values, sizeof(values),
                  ",\"local_address\":\"%04x\",\"node_address\":\"%04x\",\"iv_index\":%" PRIu32,
                  this->config_.local_address, this->config_.onoff_address, this->config_.iv_index);
    body.append(",\"network\":\"");
    body.escaped(this->config_.network_name);
    body.append("\",\"node\":\"");
    body.escaped(this->config_.node_name);
    body.append("\"");
    body.append(values);
  }
  body.append(",\"networks\":[");
  bool first = true;
  for (const auto &network : networks) {
    if (!first) body.append(",");
    first = false;
    body.append("{\"id\":\"");
    body.escaped(network.id.c_str());
    body.append("\",\"name\":\"");
    body.escaped(network.name.c_str());
    body.number("\",\"nodes\":", network.nodes);
    body.append(",\"last_update\":\"");
    body.escaped(network.last_update.c_str());
    body.append("\"}");
  }
  body.append("]}");
  if (!body.finish())
    ESP_LOGW(WEB_TAG, "Could not send NightmatIQ status response");
}

bool SteinelMesh::parse_u32_(const std::string &value, uint32_t minimum, uint32_t maximum, uint32_t &output) {
  if (value.empty()) { output = minimum; return true; }
  return protocol::decimal_u32(value.data(), value.size(), minimum, maximum, output);
}

bool SteinelMesh::parse_hex_u16_(const std::string &value, uint16_t &output) {
  if (value.empty()) { output = 0; return true; }
  return parse_hex_address(value, output);
}

bool SteinelMesh::is_safe_uuid_(const std::string &value) {
  return value.size() >= 32 && value.size() <= 40 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c)) || c == '-';
         });
}

void SteinelMesh::handle_discover_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load()) return send_json_(request, 409, "{\"message\":\"Another cloud request is running\"}");
  if (this->mesh_mode_enabled_)
    return send_json_(request, 409, "{\"message\":\"Disable or remove NightmatIQ before cloud setup\"}");
  if (this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway restart is pending\"}");
  const std::string email = request->arg("email").c_str();
  const std::string password = request->arg("password").c_str();
  if (email.empty() || password.empty()) return send_json_(request, 400, "{\"message\":\"Email and password are required\"}");
  if (!this->start_cloud_job_(CloudJob::DISCOVER, email, password))
    return send_json_(request, 503, "{\"message\":\"Not enough memory to start Steinel Cloud request\"}");
  // web_server_idf supports HTTP 200, not 202, for asynchronous jobs.
  send_json_(request, 200, "{\"message\":\"Downloading Steinel networks\"}");
}

void SteinelMesh::handle_install_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load()) return send_json_(request, 409, "{\"message\":\"Another cloud request is running\"}");
  if (this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway restart is pending\"}");
  if (this->mesh_mode_enabled_)
    return send_json_(request, 409, "{\"message\":\"Disable Mesh before importing a backup\"}");
  const std::string email = request->arg("email").c_str();
  const std::string password = request->arg("password").c_str();
  const std::string network_id = request->arg("network_id").c_str();
  uint32_t iv_index = 0;
  uint16_t node_address = 0;
  if (email.empty() || password.empty() || !is_safe_uuid_(network_id))
    return send_json_(request, 400, "{\"message\":\"Valid credentials and network are required\"}");
  if (!parse_u32_(request->arg("iv_index").c_str(), 0, UINT32_MAX, iv_index) ||
      !parse_hex_u16_(request->arg("node_address").c_str(), node_address))
    return send_json_(request, 400, "{\"message\":\"Invalid IV Index or node address\"}");
  if (!this->start_cloud_job_(CloudJob::INSTALL, email, password, network_id, iv_index, node_address))
    return send_json_(request, 503, "{\"message\":\"Not enough memory to start Steinel Cloud request\"}");
  send_json_(request, 200, "{\"message\":\"Downloading and validating the selected backup\"}");
}

void SteinelMesh::handle_enable_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load()) return send_json_(request, 409, "{\"message\":\"Cloud request in progress\"}");
  if (!this->configured_) return send_json_(request, 409, "{\"message\":\"Install a NightmatIQ configuration first\"}");
  if (this->catalog_valid_ && std::none_of(this->catalog_.nodes.begin(),
      this->catalog_.nodes.begin() + this->catalog_.count, [](const auto &node) { return node.selected; }))
    return send_json_(request, 409, "{\"message\":\"Select at least one supported device first\"}");
  if ((this->config_.flags & FLAG_REMOVE_PENDING) != 0)
    return send_json_(request, 409, "{\"message\":\"Configuration removal is already in progress\"}");
  if (this->mesh_mode_enabled_) return send_json_(request, 200, "{\"message\":\"NightmatIQ is already enabled\"}");
  if (!this->save_enabled_(true)) return send_json_(request, 500, "{\"message\":\"Could not save the enabled state\"}");
  this->set_status_("NightmatIQ enabled; rebooting into Bluetooth Mesh mode");
  this->reboot_at_ = millis() + 1500;
  this->reboot_pending_.store(true);
  send_json_(request, 200, "{\"message\":\"NightmatIQ enabled; configuration preserved\"}");
}

void SteinelMesh::handle_disable_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load()) return send_json_(request, 409, "{\"message\":\"Cloud request in progress\"}");
  if (!this->configured_) return send_json_(request, 409, "{\"message\":\"No NightmatIQ configuration is installed\"}");
  if ((this->config_.flags & FLAG_REMOVE_PENDING) != 0)
    return send_json_(request, 409, "{\"message\":\"Configuration removal is already in progress\"}");
  if (!this->mesh_mode_enabled_) return send_json_(request, 200, "{\"message\":\"NightmatIQ is already disabled\"}");
  if (!this->save_enabled_(false)) return send_json_(request, 500, "{\"message\":\"Could not save the disabled state\"}");
  this->force_actual_output_unavailable_();
  this->set_status_("NightmatIQ disabled; rebooting into setup mode");
  this->reboot_at_ = millis() + 1500;
  this->reboot_pending_.store(true);
  send_json_(request, 200, "{\"message\":\"NightmatIQ disabled; saved configuration preserved\"}");
}

void SteinelMesh::handle_remove_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load()) return send_json_(request, 409, "{\"message\":\"Cloud request in progress\"}");
  this->force_actual_output_unavailable_();
  if (this->configured_ && this->mesh_started_) {
    if (this->mesh_remove_pending_.exchange(true))
      return send_json_(request, 200, "{\"message\":\"Configuration removal is already in progress\"}");
    // Send the HTTP response before the main loop blocks on Mesh deinitialization.
    this->mesh_remove_not_before_ = millis() + 750;
    this->set_status_("Bluetooth Mesh configuration removal scheduled");
    return send_json_(request, 200, "{\"message\":\"Stopping Bluetooth Mesh and removing configuration\"}");
  } else if (this->configured_) {
    const esp_err_t erase_result = esp_ble_mesh_provisioner_direct_erase_settings();
    if (erase_result != ESP_OK) {
      ESP_LOGE(WEB_TAG, "Could not erase inactive ESP-BLE-MESH settings: %s", esp_err_to_name(erase_result));
      return send_json_(request, 500, "{\"message\":\"Could not erase Bluetooth Mesh settings\"}");
    }
    this->clear_config_();
    this->cloud_ble_resume_pending_.store(true);
    this->set_status_("Configuration removed; gateway ready for setup");
  } else {
    this->clear_config_();
    this->cloud_ble_resume_pending_.store(true);
    this->set_status_("No NightmatIQ configuration; gateway ready for setup");
  }
  send_json_(request, 200, "{\"message\":\"NightmatIQ configuration removed\"}");
}

void SteinelMesh::handle_factory_reset_(AsyncWebServerRequest *request) {
  if (this->factory_reset_pending_.load())
    return send_json_(request, 200, "{\"message\":\"Factory reset is already scheduled\"}");
  if (this->cloud_busy_.load() || this->auto_update_running_.load() ||
      this->auto_update_mode_ || this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");

  this->set_status_("Factory reset scheduled; all gateway settings will be erased");
  this->factory_reset_at_ = millis() + 1000;
  this->factory_reset_pending_.store(true);
  send_json_(request, 200,
             "{\"message\":\"Factory reset scheduled; reconnect to the gateway access point\"}");
}

void SteinelMesh::handle_mode_(AsyncWebServerRequest *request) {
  if (!this->legacy_profile_) return send_json_(request, 409, "{\"message\":\"Use the selected device controls\"}");
  if (!this->configured_ || !this->mesh_mode_enabled_ || !this->mesh_ready_.load())
    return send_json_(request, 409, "{\"message\":\"Bluetooth Mesh is not ready\"}");
  if (this->cloud_busy_.load() || this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");
  const std::string mode = request->arg("mode").c_str();
  if (mode != "Auto" && mode != "Always On" && mode != "Always Off")
    return send_json_(request, 400, "{\"message\":\"Invalid NightmatIQ mode\"}");
  this->set_mode(mode);
  // web_server_idf supports HTTP 200, not 202, for asynchronous jobs.
  send_json_(request, 200, "{\"message\":\"Changing NightmatIQ mode\"}");
}

void SteinelMesh::handle_threshold_(AsyncWebServerRequest *request) {
  if (!this->legacy_profile_) return send_json_(request, 409, "{\"message\":\"Use the selected device controls\"}");
  if (!this->configured_ || !this->mesh_mode_enabled_ || !this->mesh_ready_.load())
    return send_json_(request, 409, "{\"message\":\"Bluetooth Mesh is not ready\"}");
  if (this->cloud_busy_.load() || this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");
  uint32_t threshold = 0;
  if (!parse_u32_(request->arg("value").c_str(), 1, 1500, threshold))
    return send_json_(request, 400, "{\"message\":\"Threshold must be between 1 and 1500 lx\"}");
  this->set_threshold(static_cast<float>(threshold));
  send_json_(request, 200, "{\"message\":\"Changing twilight threshold\"}");
}

void SteinelMesh::handle_refresh_(AsyncWebServerRequest *request) {
  if (!this->configured_ || !this->mesh_mode_enabled_ || !this->mesh_ready_.load())
    return send_json_(request, 409, "{\"message\":\"Bluetooth Mesh is not ready\"}");
  if (this->cloud_busy_.load() || this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");
  this->web_refresh_pending_.store(true);
  send_json_(request, 200, "{\"message\":\"Refreshing NightmatIQ state\"}");
}

void SteinelMesh::handle_password_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load() || this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");

  std::string password = request->arg("password").c_str();
  std::string confirmation = request->arg("confirmation").c_str();
  if (password != confirmation) {
    std::fill(password.begin(), password.end(), '\0');
    std::fill(confirmation.begin(), confirmation.end(), '\0');
    return send_json_(request, 400, "{\"message\":\"The passwords do not match\"}");
  }
  if (!valid_admin_password_(password)) {
    std::fill(password.begin(), password.end(), '\0');
    std::fill(confirmation.begin(), confirmation.end(), '\0');
    return send_json_(request, 400,
                      "{\"message\":\"Use 8-63 printable characters without spaces\"}");
  }
  if (password == this->web_password_) {
    std::fill(password.begin(), password.end(), '\0');
    std::fill(confirmation.begin(), confirmation.end(), '\0');
    return send_json_(request, 400,
                      "{\"message\":\"Choose a password different from the current password\"}");
  }
  if (!this->save_admin_password_(password)) {
    std::fill(password.begin(), password.end(), '\0');
    std::fill(confirmation.begin(), confirmation.end(), '\0');
    return send_json_(request, 500, "{\"message\":\"Could not save the administrator password\"}");
  }

  std::fill(password.begin(), password.end(), '\0');
  std::fill(confirmation.begin(), confirmation.end(), '\0');
  this->set_status_("Administrator password changed; restarting gateway");
  this->reboot_at_ = millis() + 2000;
  this->reboot_pending_.store(true);
  send_json_(request, 200, "{\"message\":\"Password changed; sign in again after restart\"}");
}

void SteinelMesh::handle_wifi_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load() || this->auto_update_running_.load() ||
      this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");

  std::string ssid = request->arg("ssid").c_str();
  std::string password = request->arg("password").c_str();
  const bool valid_ssid = !ssid.empty() && ssid.size() <= 32 &&
                          ssid.find('\0') == std::string::npos;
  const bool valid_password = password.size() >= 8 && password.size() <= 63 &&
                              std::all_of(password.begin(), password.end(), [](unsigned char value) {
                                return value >= 0x20 && value <= 0x7E;
                              });
  if (!valid_ssid || !valid_password) {
    std::fill(password.begin(), password.end(), '\0');
    return send_json_(request, 400,
                      "{\"message\":\"SSID must contain 1-32 bytes and the Wi-Fi password must contain 8-63 printable characters\"}");
  }
  if (wifi::global_wifi_component == nullptr) {
    std::fill(password.begin(), password.end(), '\0');
    return send_json_(request, 500, "{\"message\":\"Wi-Fi component is not available\"}");
  }

  this->defer([this, ssid = std::move(ssid), password = std::move(password)]() mutable {
    wifi::global_wifi_component->save_wifi_sta(ssid, password);
    std::fill(password.begin(), password.end(), '\0');
    this->set_status_("Wi-Fi configuration changed; restarting gateway");
    this->reboot_at_ = millis() + 2000;
    this->reboot_pending_.store(true);
  });
  send_json_(request, 200,
             "{\"message\":\"Wi-Fi configuration saved; gateway is restarting\"}");
}

void SteinelMesh::handle_auto_update_(AsyncWebServerRequest *request) {
  if (this->cloud_busy_.load() || this->auto_update_running_.load() ||
      this->reboot_pending_.load())
    return send_json_(request, 409, "{\"message\":\"Gateway is busy\"}");

  const std::string version = request->arg("version").c_str();
  const std::string url = request->arg("url").c_str();
  const std::string digest_text = request->arg("sha256").c_str();
  uint32_t image_size = 0;
  std::array<uint16_t, 3> target_version{};
  std::array<uint16_t, 3> current_version{};
  std::array<uint8_t, 32> digest{};
  if (!parse_version_(version, target_version) ||
      !parse_version_(ESPHOME_PROJECT_VERSION, current_version) ||
      target_version <= current_version || !parse_sha256_(digest_text, digest) ||
      !parse_u32_(request->arg("size").c_str(), 1, UINT32_MAX, image_size))
    return send_json_(request, 400,
                      "{\"message\":\"Invalid or non-newer firmware release\"}");

  const std::string expected_url = std::string(RELEASE_DOWNLOAD_PREFIX) + version + "/" +
                                   RELEASE_ASSET_PREFIX + version + "-ota.bin";
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (url != expected_url || url.size() > AUTO_UPDATE_URL_MAX_LENGTH ||
      partition == nullptr || image_size > partition->size)
    return send_json_(request, 400,
                      "{\"message\":\"Release does not contain a valid OTA image\"}");

  StoredAutoUpdate update{};
  update.image_size = image_size;
  std::memcpy(update.target_version, version.data(), version.size());
  std::memcpy(update.url, url.data(), url.size());
  update.sha256 = digest;
  if (!this->save_auto_update_(update))
    return send_json_(request, 500,
                      "{\"message\":\"Could not save the firmware update request\"}");

  this->set_status_("Firmware update scheduled; restarting into update mode");
  this->reboot_at_ = millis() + 1500;
  this->reboot_pending_.store(true);
  send_json_(request, 200,
             "{\"message\":\"Update scheduled; the gateway will download and verify it after restart\"}");
}

bool SteinelMesh::cloud_get_(const std::string &path, const std::string &email, const std::string &password,
                                bool use_ota_workspace, CloudBody &body, int &http_status, std::string &error) {
  if (!body.prepare(use_ota_workspace, error)) return false;
  const std::string url = std::string(API_BASE) + path;
  std::string credentials = email + ':' + password;
  size_t encoded_size = 0;
  mbedtls_base64_encode(nullptr, 0, &encoded_size, reinterpret_cast<const uint8_t *>(credentials.data()),
                        credentials.size());
  std::vector<uint8_t> encoded(encoded_size + 1, 0);
  if (mbedtls_base64_encode(encoded.data(), encoded.size(), &encoded_size,
                            reinterpret_cast<const uint8_t *>(credentials.data()), credentials.size()) != 0) {
    error = "Could not prepare authorization";
    return false;
  }
  std::string authorization = "Basic " + std::string(reinterpret_cast<char *>(encoded.data()), encoded_size);
  uint8_t mac[6]{};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char device[32];
  std::snprintf(device, sizeof(device), "NMQ-C3-%02x%02x%02x%02x%02x%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  esp_err_t result = ESP_FAIL;
  constexpr unsigned MAX_ATTEMPTS = 2;
  for (unsigned attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    esp_http_client_config_t http_config{};
    http_config.url = url.c_str();
    http_config.method = HTTP_METHOD_GET;
    http_config.timeout_ms = use_ota_workspace ? 60000 : 20000;
    http_config.buffer_size = 1024;
    http_config.buffer_size_tx = 512;
    http_config.event_handler = cloud_http_event_;
    http_config.user_data = &body;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == nullptr) {
      error = "HTTPS client initialization failed";
      break;
    }
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "X-Accept-Version", "2.4");
    esp_http_client_set_header(client, "User-Agent", "A4.1-62");
    esp_http_client_set_header(client, "Device", device);
    esp_http_client_set_header(client, "Accept-Language", "en");
    esp_http_client_set_header(client, "Connection", "close");
    result = esp_http_client_perform(client);
    http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    const bool retryable_transport_error =
        result == ESP_ERR_HTTP_INCOMPLETE_DATA || (result != ESP_OK && http_status <= 0);
    if (!retryable_transport_error || attempt == MAX_ATTEMPTS || body.sink_error != ESP_OK) break;

    ESP_LOGW(WEB_TAG, "Steinel HTTPS transport error %s after %u bytes; retrying once with a fresh connection",
             esp_err_to_name(result), static_cast<unsigned>(body.length));
    body.reset_for_retry();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  std::fill(credentials.begin(), credentials.end(), '\0');
  std::fill(authorization.begin(), authorization.end(), '\0');
  std::fill(encoded.begin(), encoded.end(), 0);
  this->cloud_response_bytes_.store(static_cast<uint32_t>(std::min<size_t>(body.length, UINT32_MAX)));
  if (!error.empty()) return false;
  if (body.sink_error != ESP_OK) {
    if (body.sink_error == ESP_ERR_INVALID_SIZE)
      error = use_ota_workspace ? "Network backup exceeds inactive OTA workspace" : "Cloud response is too large";
    else if (body.sink_error == ESP_ERR_NO_MEM)
      error = "Not enough memory for the Steinel network list";
    else
      error = std::string("Cloud storage error: ") + esp_err_to_name(body.sink_error);
    return false;
  }
  if (result != ESP_OK) { error = std::string("HTTPS error: ") + esp_err_to_name(result); return false; }
  if (http_status != 200) { error = "Steinel returned HTTP " + std::to_string(http_status); return false; }
  if (body.length == 0 || body.length > body.limit()) { error = "Invalid cloud response size"; return false; }
  ESP_LOGI(WEB_TAG, "Steinel response stored in %s: %u bytes",
           use_ota_workspace ? "inactive OTA workspace" : "RAM",
           static_cast<unsigned>(body.length));
  return true;
}

bool SteinelMesh::discover_networks_(const std::string &email, const std::string &password, std::string &error) {
  CloudBody body;
  int status = 0;
  // Skip translation sync while fetching all owned networks.
  if (!this->cloud_get_("/changes?since=4102444800&force_full_personal_sync=true&force_full_translation_sync=false",
                        email, password, false, body, status, error)) return false;
  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(body.memory), body.length);
  const cJSON *changed = object_item(object_item(root, "networks"), "changed");
  if (!cJSON_IsArray(changed)) { cJSON_Delete(root); error = "Cloud response contains no networks"; return false; }
  std::vector<NetworkChoice> choices;
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, changed) {
    NetworkChoice choice;
    choice.id = json_string(item, "id");
    choice.name = json_string(item, "name");
    choice.last_update = json_string(item, "lastUpdate");
    choice.nodes = static_cast<uint16_t>(std::min<uint32_t>(json_uint(item, "nodes"), UINT16_MAX));
    if (is_safe_uuid_(choice.id) && choice.nodes > 0) choices.push_back(std::move(choice));
  }
  cJSON_Delete(root);
  if (choices.empty()) { error = "No usable Steinel network was found"; return false; }
  {
    std::lock_guard<std::mutex> lock(this->state_mutex_);
    this->networks_ = std::move(choices);
  }
  return true;
}

bool SteinelMesh::parse_backup_(const CloudBody &body, uint32_t requested_iv_index,
                                   uint16_t requested_node_address, StoredConfig &config,
                                   std::array<uint8_t, 16> &device_key,
                                   StoredAddressPolicy &address_policy, protocol::Catalog &output_catalog,
                                   std::string &error) {
  if (!body.use_flash || body.partition == nullptr || body.length == 0) {
    error = "Network backup workspace is not available";
    return false;
  }
  auto *summary = new (std::nothrow) BackupSummary{};
  if (summary == nullptr) { error = "Not enough memory for backup metadata"; return false; }
  FlashJsonReader reader(body.partition, body.length);
  const bool parsed_backup = read_backup(reader, *summary);
  reader.skip_whitespace();
  char trailing = 0;
  if (!parsed_backup || !reader.healthy() || reader.peek(trailing)) {
    error = "Backup JSON parse failed near byte " + std::to_string(reader.position());
    delete summary;
    return false;
  }
  config = StoredConfig{};
  if (!summary->mesh_uuid_valid) {
    error = "Backup has no valid Mesh UUID";
    delete summary;
    return false;
  }
  config.mesh_uuid = summary->mesh_uuid;
  if (requested_iv_index == 0) {
    uint32_t cached_iv_index = 0;
    if (this->load_cached_iv_index_(config.mesh_uuid, cached_iv_index)) {
      config.iv_index = cached_iv_index;
      ESP_LOGI(WEB_TAG, "Using cached authenticated IV Index %" PRIu32 " for this Steinel network",
               cached_iv_index);
    }
  }

  // Keep only the small key lists while reusing the metadata allocation.
  const auto app_keys = summary->app_keys;
  const auto net_keys = summary->net_keys;
  const size_t app_key_count = summary->app_key_count;
  const size_t net_key_count = summary->net_key_count;
  uint16_t best_score = 0;
  uint16_t best_anchor = 0;
  const auto filter_models = [&](uint16_t key) {
    new (summary) BackupSummary{};
    reader = FlashJsonReader(body.partition, body.length);
    const bool parsed = read_backup(reader, *summary, key);
    reader.skip_whitespace();
    return parsed && reader.healthy() && !reader.peek(trailing) &&
           summary->mesh_uuid_valid && summary->mesh_uuid == config.mesh_uuid;
  };
  for (size_t i = 0; i < app_key_count; ++i) {
    const auto &app = app_keys[i];
    if (!app.index_valid || !app.net_index_valid || !app.key_valid) continue;
    const BackupNetKey *net = nullptr;
    for (size_t j = 0; j < net_key_count; ++j)
      if (net_keys[j].index_valid && net_keys[j].key_valid && net_keys[j].index == app.net_index) {
        net = &net_keys[j]; break;
      }
    if (net == nullptr) continue;
    if (!filter_models(app.index)) {
      error = "Backup model binding validation failed"; delete summary; return false;
    }
    uint16_t anchor = 0;
    uint16_t score = 0;
    for (size_t j = 0; j < summary->node_count; ++j) {
      const auto &node = summary->nodes[j];
      if (!node.address_valid || !node.device_key_valid || node.element_count == 0 ||
          node.element_count > protocol::MAX_ELEMENTS) continue;
      uint16_t controls = 0;
      for (const auto &element : node.details.elements)
        controls |= element.capabilities & (protocol::ONOFF | protocol::LIGHTNESS | protocol::LC | protocol::SENSOR);
      if (controls == 0) continue;
      if ((requested_node_address == 0 || requested_node_address == node.address) &&
          (anchor == 0 || node.address < anchor)) anchor = node.address;
      score += 16;
      for (; controls; controls >>= 1) score += controls & 1;
    }
    if (anchor == 0 || score == 0 ||
        (best_score != 0 && (score < best_score || (score == best_score && app.index >= config.app_key_index)))) continue;
    best_score = score;
    best_anchor = anchor;
    config.app_key_index = app.index; config.net_key_index = app.net_index;
    config.app_key = app.key; config.net_key = net->key;
  }
  if (best_score == 0) {
    error = "No supported device functions have a usable AppKey/NetKey pair";
    delete summary; return false;
  }
  if (!filter_models(config.app_key_index)) {
    error = "Backup model binding validation failed"; delete summary; return false;
  }

  const BackupNode *selected = nullptr;
  for (size_t i = 0; i < summary->node_count; ++i)
    if (summary->nodes[i].address == best_anchor) { selected = &summary->nodes[i]; break; }
  if (selected == nullptr) { error = "Selected device is absent from the backup"; delete summary; return false; }
  device_key = selected->device_key;
  config.onoff_address = selected->address;
  config.lc_address = static_cast<uint16_t>(selected->address + selected->lc_element_index);
  config.sensor_address = static_cast<uint16_t>(selected->address + selected->sensor_element_index);
  if (config.iv_index == 0) config.iv_index = requested_iv_index;
  config.scene_number = 6;
  std::snprintf(config.network_name, sizeof(config.network_name), "%s", summary->mesh_name);
  std::snprintf(config.node_name, sizeof(config.node_name), "%s", selected->name);
  for (size_t i = 0; i < summary->node_info_count; ++i)
    if (summary->node_infos[i].address_valid && summary->node_infos[i].address == config.onoff_address)
      config.scene_number = summary->node_infos[i].scene;

  auto *catalog = new (std::nothrow) protocol::Catalog{};
  if (catalog == nullptr) { error = "Not enough memory for device metadata"; delete summary; return false; }
  catalog->mesh_uuid = summary->mesh_uuid;
  for (size_t i = 0; i < summary->node_count; ++i) {
    const auto &source = summary->nodes[i];
    if (!source.address_valid || !source.device_key_valid) continue;
    if (source.element_count == 0 || source.element_count > protocol::MAX_ELEMENTS) {
      error = "A device exceeds the supported element limit (8)"; break;
    }
    if (catalog->count == catalog->nodes.size()) { error = "Network exceeds the device limit (16)"; break; }
    auto &node = catalog->nodes[catalog->count++];
    node = source.details;
    for (size_t j = 0; j < summary->node_info_count; ++j)
      if (summary->node_infos[j].address == node.address) node.scene = summary->node_infos[j].scene;
    // Keep elements with incompatible AppKey bindings visible but not controllable.
    for (auto &element : node.elements)
      if (element.app_key != config.app_key_index) {
        element.capabilities = 0; element.sensor_group = 0;
      }
    node.nightmatiq = node.nightmatiq && protocol::element_with(node, protocol::ONOFF) == 0 &&
                     protocol::element_with(node, protocol::LC) == 1 &&
                     protocol::element_with(node, protocol::SENSOR) == 2 &&
                     protocol::element_with(node, protocol::SCENE) == 0;
    if (this->catalog_valid_ && this->catalog_.mesh_uuid == catalog->mesh_uuid) {
      for (size_t j = 0; j < this->catalog_.count; ++j)
        if (protocol::same_node(node, this->catalog_.nodes[j])) node.selected = this->catalog_.nodes[j].selected;
    } else if (this->configured_ && this->config_.mesh_uuid == catalog->mesh_uuid &&
             node.address == this->config_.onoff_address)
      node.selected = true;
    if (protocol::element_with(node, protocol::ONOFF | protocol::LIGHTNESS | protocol::LC | protocol::SENSOR) < 0)
      node.selected = false;
  }
  if (error.empty() && !protocol::valid_catalog(*catalog)) error = "Duplicate or overlapping device addresses";
  if (!error.empty()) { delete catalog; delete summary; return false; }

  if (!summary->unicast_range_valid) {
    error = "Backup has no valid provisioner unicast range";
    delete summary;
    delete catalog;
    return false;
  }

  uint32_t highest_occupied = static_cast<uint32_t>(summary->low_address) - 1;
  for (size_t index = 0; index < summary->node_count; index++) {
    const BackupNode &node = summary->nodes[index];
    if (!node.address_valid) continue;
    const uint32_t node_last = std::min<uint32_t>(
        static_cast<uint32_t>(node.address) + std::max<uint16_t>(1, node.element_count) - 1,
        0x7FFF);
    if (node.address <= summary->high_address && node_last >= summary->low_address)
      highest_occupied = std::max<uint32_t>(highest_occupied,
                                            std::min<uint32_t>(node_last, summary->high_address));
  }
  const uint16_t pool_high = summary->high_address;
  const uint16_t bounded_pool_low =
      pool_high >= ADDRESS_POOL_TARGET_SIZE
          ? static_cast<uint16_t>(pool_high - ADDRESS_POOL_TARGET_SIZE + 1)
          : summary->low_address;
  const uint32_t first_free = highest_occupied + 1;
  const uint16_t pool_low = static_cast<uint16_t>(
      std::max<uint32_t>(summary->low_address,
                         std::max<uint32_t>(bounded_pool_low, first_free)));
  if (pool_low > pool_high) {
    error = "Provisioner range has no safe local Mesh address pool";
    delete summary;
    delete catalog;
    return false;
  }

  address_policy = StoredAddressPolicy{};
  address_policy.pool_low = pool_low;
  address_policy.pool_high = pool_high;
  address_policy.mesh_uuid = summary->mesh_uuid;
  address_policy.installation_nonce = esp_random();
  if (address_policy.installation_nonce == 0) address_policy.installation_nonce = 1;
  const bool continuing_policy =
      this->address_policy_valid_ && this->address_policy_.mesh_uuid == summary->mesh_uuid;
  if (continuing_policy) {
    const uint16_t previous = this->address_policy_.current_address;
    config.local_address = previous <= pool_low || previous > pool_high
                               ? pool_high
                               : static_cast<uint16_t>(previous - 1);
  } else {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
      error = "Could not read the ESP32 hardware identity";
      delete summary;
      delete catalog;
      return false;
    }
    uint32_t hash = 2166136261U;
    const auto mix = [&hash](uint8_t value) {
      hash ^= value;
      hash *= 16777619U;
    };
    for (uint8_t value : summary->mesh_uuid) mix(value);
    for (uint8_t value : mac) mix(value);
    for (uint8_t shift = 0; shift < 32; shift += 8)
      mix(static_cast<uint8_t>(address_policy.installation_nonce >> shift));
    const uint32_t pool_size = static_cast<uint32_t>(pool_high) - pool_low + 1;
    config.local_address = static_cast<uint16_t>(pool_low + (hash % pool_size));
  }
  address_policy.initial_address = config.local_address;
  address_policy.current_address = config.local_address;
  output_catalog = *catalog;
  delete catalog;
  delete summary;
  if (config.local_address == 0 || config.local_address >= 0x8000) {
    error = "No free local Mesh address";
    return false;
  }
  return true;
}

bool SteinelMesh::install_network_(const std::string &email, const std::string &password,
                                      const std::string &network_id, uint32_t iv_index,
                                      uint16_t node_address, std::string &error) {
  CloudBody body;
  int status = 0;
  if (!this->cloud_get_("/project/network/" + network_id + "/backup", email, password,
                        true, body, status, error))
    return false;
  return this->install_backup_(body, iv_index, node_address, error);
}

bool SteinelMesh::install_backup_(CloudBody &body, uint32_t iv_index,
                                    uint16_t node_address, std::string &error) {
  StoredConfig parsed{};
  std::array<uint8_t, 16> device_key{};
  StoredAddressPolicy address_policy{};
  std::unique_ptr<protocol::Catalog> staged(new (std::nothrow) protocol::Catalog{});
  if (!staged) { error = "Not enough memory for device metadata"; return false; }
  if (!this->parse_backup_(body, iv_index, node_address, parsed, device_key,
                           address_policy, *staged, error))
    return false;
  parsed.flags &= ~FLAG_ENABLED;
  parsed.flags |= FLAG_DEVICE_CATALOG;
  // NVS writes are atomic per blob, not across keys. The import guard prevents
  // Mesh startup with mixed keys after a partial write.
  bool interrupted = true;
  if (!this->import_guard_preference_.save(&interrupted) || !global_preferences->sync()) {
    error = "Could not save configuration to flash"; return false;
  }
  StoredDeviceKey key{}; key.key = device_key;
  const bool keep_identity = this->config_.mesh_uuid == parsed.mesh_uuid &&
      this->config_.onoff_address == parsed.onoff_address && this->device_key_ == device_key;
  StoredAdvertisedIdentity empty_identity{}; empty_identity.magic = 0;
  const bool saved = this->device_key_preference_.save(&key) &&
                     this->address_policy_preference_.save(&address_policy) &&
                     this->catalog_preference_.save(staged.get()) &&
                     this->config_preference_.save(&parsed) &&
                     (keep_identity || this->advertised_identity_preference_.save(&empty_identity)) &&
                     global_preferences->sync();
  interrupted = false;
  if (!saved || !this->import_guard_preference_.save(&interrupted) || !global_preferences->sync()) {
    this->configured_ = false; this->mesh_mode_enabled_ = false;
    error = "Import was interrupted. Import the network again before enabling Mesh."; return false;
  }
  // Keep catalog storage referenced by callbacks and entities immutable;
  // load its replacement from NVS after restarting.
  this->config_ = parsed; this->configured_ = true; this->mesh_mode_enabled_ = false;
  if (!keep_identity) this->advertised_identity_valid_.store(false);
  this->device_key_ = device_key; this->device_key_valid_ = true;
  this->address_policy_ = address_policy; this->address_policy_valid_ = true;
  this->remember_iv_index_(parsed);
  return true;
}

bool SteinelMesh::start_cloud_job_(CloudJob job, const std::string &email, const std::string &password,
                                      const std::string &network_id, uint32_t iv_index,
                                      uint16_t node_address) {
  this->cloud_session_reboot_pending_.store(false);
  this->cloud_busy_.store(true);
  this->set_status_(job == CloudJob::DISCOVER ? "Connecting to Steinel Cloud" : "Downloading network backup");
  auto *args = new (std::nothrow) CloudTaskArgs{this, job, email, password, network_id, iv_index, node_address};
  if (args == nullptr) {
    this->cloud_busy_.store(false);
    this->schedule_cloud_session_reboot_(CLOUD_ERROR_REBOOT_DELAY_MS);
    this->set_status_("Could not allocate Steinel Cloud request");
    return false;
  }

  this->cloud_api_shutdown_started_.store(false);
  this->cloud_api_shutdown_pending_.store(true);
  this->cloud_api_shutdown_deadline_.store(millis() + CLOUD_API_SHUTDOWN_TIMEOUT_MS);
  this->cloud_pending_args_.store(args);
  this->pause_ble_for_cloud_();
  ESP_LOGI(WEB_TAG, "Cloud request queued until ESPHome API and Bluetooth release their memory");
  return true;
}

void SteinelMesh::advance_cloud_job_() {
  CloudTaskArgs *pending = this->cloud_pending_args_.load();
  if (pending == nullptr)
    return;

  if (this->cloud_api_shutdown_pending_.load()) {
    if (api::global_api_server != nullptr &&
        !this->cloud_api_shutdown_started_.exchange(true)) {
      api::global_api_server->on_shutdown();
      ESP_LOGI(WEB_TAG, "Closing ESPHome API clients before Steinel HTTPS");
    }

    const bool api_released =
        api::global_api_server == nullptr || api::global_api_server->teardown();
    if (!api_released) {
      const uint32_t deadline = this->cloud_api_shutdown_deadline_.load();
      if (static_cast<int32_t>(millis() - deadline) < 0)
        return;
      pending = this->cloud_pending_args_.exchange(nullptr);
      if (pending == nullptr)
        return;
      std::fill(pending->email.begin(), pending->email.end(), '\0');
      std::fill(pending->password.begin(), pending->password.end(), '\0');
      delete pending;
      this->cloud_api_shutdown_pending_.store(false);
      this->cloud_busy_.store(false);
      this->schedule_cloud_session_reboot_(CLOUD_ERROR_REBOOT_DELAY_MS);
      this->set_status_("Could not close Home Assistant connection for HTTPS");
      return;
    }

    this->cloud_api_shutdown_pending_.store(false);
    this->cloud_ble_pause_deadline_.store(millis() + 7500);
    this->cloud_ble_pause_pending_.store(true);
    ESP_LOGI(WEB_TAG, "ESPHome API stopped; releasing Bluetooth memory");
    return;
  }

  const bool bluetooth_released =
      esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED;
  if (!bluetooth_released) {
    const uint32_t deadline = this->cloud_ble_pause_deadline_.load();
    if (static_cast<int32_t>(millis() - deadline) < 0)
      return;
    pending = this->cloud_pending_args_.exchange(nullptr);
    if (pending == nullptr)
      return;
    std::fill(pending->email.begin(), pending->email.end(), '\0');
    std::fill(pending->password.begin(), pending->password.end(), '\0');
    delete pending;
    this->cloud_busy_.store(false);
    this->schedule_cloud_session_reboot_(CLOUD_ERROR_REBOOT_DELAY_MS);
    this->set_status_("Could not release Bluetooth memory for HTTPS");
    return;
  }

  pending = this->cloud_pending_args_.exchange(nullptr);
  if (pending == nullptr)
    return;
  const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  this->cloud_free_after_ble_.store(free_internal);
  this->cloud_largest_after_ble_.store(largest_internal);
  ESP_LOGI(WEB_TAG, "Starting cloud task after Bluetooth release: free internal heap=%u, largest block=%u, stack=%u",
           static_cast<unsigned>(free_internal), static_cast<unsigned>(largest_internal),
           static_cast<unsigned>(CLOUD_TASK_STACK_BYTES));
  if (xTaskCreate(cloud_task_, "steinel_cloud", CLOUD_TASK_STACK_BYTES, pending, 2, nullptr) != pdPASS) {
    std::fill(pending->email.begin(), pending->email.end(), '\0');
    std::fill(pending->password.begin(), pending->password.end(), '\0');
    delete pending;
    this->cloud_busy_.store(false);
    this->schedule_cloud_session_reboot_(CLOUD_ERROR_REBOOT_DELAY_MS);
    this->set_status_("Could not start cloud task after Bluetooth release; largest block " +
                      std::to_string(largest_internal) + " bytes");
  }
}

void SteinelMesh::schedule_cloud_session_reboot_(uint32_t delay_ms) {
  this->cloud_session_reboot_at_.store(millis() + delay_ms);
  this->cloud_session_reboot_pending_.store(true);
}

void SteinelMesh::cloud_task_(void *parameter) {
  auto *args = static_cast<CloudTaskArgs *>(parameter);
  std::string error;
  const bool ok = args->job == CloudJob::DISCOVER
                      ? args->owner->discover_networks_(args->email, args->password, error)
                      : args->owner->install_network_(args->email, args->password, args->network_id,
                                                      args->iv_index, args->node_address, error);
  if (ok && args->job == CloudJob::DISCOVER) {
    args->owner->set_status_("Select a network containing NightmatIQ", false);
    args->owner->schedule_cloud_session_reboot_(CLOUD_DISCOVER_SESSION_TIMEOUT_MS);
  }
  if (ok && args->job == CloudJob::INSTALL) {
    args->owner->cloud_session_reboot_pending_.store(false);
    args->owner->set_status_("Configuration saved; rebooting", false);
    args->owner->reboot_at_ = millis() + 2000;
    args->owner->reboot_pending_.store(true);
  }
  if (!ok) {
    args->owner->set_status_(error, false);
    args->owner->schedule_cloud_session_reboot_(CLOUD_ERROR_REBOOT_DELAY_MS);
  }
  args->owner->cloud_busy_.store(false);
  ESP_LOGI(WEB_TAG, "Cloud task finished: free internal heap=%u, stack high-water=%u",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  std::fill(args->email.begin(), args->email.end(), '\0');
  std::fill(args->password.begin(), args->password.end(), '\0');
  delete args;
  vTaskDelete(nullptr);
}

}  // namespace esphome::steinel_mesh
