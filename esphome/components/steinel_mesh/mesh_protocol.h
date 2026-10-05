#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::steinel_mesh::protocol {
inline bool decimal_u32(const char *value, size_t length, uint32_t minimum, uint32_t maximum,
                        uint32_t &output) {
  if (length == 0 || minimum > maximum) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    const uint32_t digit = value[i] - '0';
    if (digit > maximum || parsed > (maximum - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  if (parsed < minimum) return false;
  output = parsed;
  return true;
}

constexpr size_t MAX_NODES = 16;
constexpr size_t MAX_ELEMENTS = 8;
constexpr size_t MAX_GROUPS = 16;
constexpr uint16_t ONOFF = 1;
constexpr uint16_t LIGHTNESS = 2;
constexpr uint16_t LC = 4;
constexpr uint16_t SENSOR = 8;
constexpr uint16_t SCENE = 16;
constexpr uint16_t SCHEDULER = 32;
constexpr uint16_t UNKNOWN_BINDING = 0xFFFF;
constexpr uint32_t UNKNOWN_24 = 0xFFFFFF;
// Model presence and individual function support are tracked separately.
constexpr uint16_t F_OUTPUT = 1, F_BRIGHTNESS = 2, F_AUTO = 4, F_THRESHOLD = 8,
                   F_RUN_TIME = 16, F_LUX = 32, F_MOTION = 64, F_MODE = 128;

inline const char *product_name(uint16_t company, uint16_t product) {
  if (company != 0x0563) return "";
  switch (product) {
    case 0x1B1B: return "L 830 SC";
    case 0x1DCE: return "NightmatIQ Plus";
    case 0x1DE0: return "IS 180";
    case 0x1E74: return "L 820 SC";
    case 0x1E79: return "L 810 SC";
    case 0x1EBD: return "L 810 C";
    case 0x1F27: return "L 42 SC";
    default: return "";
  }
}
inline bool firmware_from_vid(uint16_t company, uint16_t product, uint16_t vid,
                              uint8_t &major, uint8_t &minor, uint8_t &patch) {
  // Vendor encoding, not a Bluetooth SIG interpretation of an arbitrary VID.
  if (company != 0x0563 || vid == 0 || vid == 0xFFFF ||
      product == 0 || product == 0xFFFF || product == 0x1DCE) return false;
  major = vid >> 11; minor = (vid >> 6) & 31; patch = vid & 63;
  return true;
}
inline bool revision_string(const uint8_t *data, size_t size, char (&out)[17]) {
  // Revision properties are UTF-8 strings; accept only bounded printable ASCII.
  if (data == nullptr || size == 0 || size > 16) return false;
  size_t end = size;
  while (end && (data[end - 1] == 0 || data[end - 1] == ' ')) --end;
  if (!end) return false;
  for (size_t i = 0; i < end; ++i) if (data[i] < 32 || data[i] > 126) return false;
  std::memcpy(out, data, end); out[end] = 0; return true;
}
template<typename Consumer> bool sensor_descriptors(const uint8_t *data, size_t size, Consumer consume) {
  if (data == nullptr || size < 2 || (size != 2 && size % 8 != 0)) return false;
  // A two-byte reply means the requested descriptor does not exist.
  if (size == 2) return true;
  for (size_t i = 0; i < size; i += 8) consume(uint16_t(data[i]) | (uint16_t(data[i + 1]) << 8));
  return true;
}

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t le24(const uint8_t *p) { return uint32_t(le16(p)) | (uint32_t(p[2]) << 16); }
inline bool unicast(uint32_t address) { return address > 0 && address < 0x8000; }
inline bool group(uint16_t address) { return address >= 0xC000 && address <= 0xFEFF; }
inline bool deadline_pending(uint32_t now, uint32_t deadline) {
  return deadline != 0 && int32_t(now - deadline) < 0;
}
struct ConfirmedMode {
  uint32_t confirmed_at{0}, verify_after{0};
  int8_t value{-1};
  bool writing{false}, verifying{false};

  void begin_write() { writing = true; verifying = false; }
  void finish_write(uint32_t now) { writing = false; verifying = true; verify_after = now; }
  void observe(uint32_t now, int8_t on, int8_t automatic,
               uint32_t on_seen, uint32_t automatic_seen, uint32_t stale_after) {
    if (value >= 0 && (now - confirmed_at >= stale_after || automatic < 0 ||
                      (value != 0 && on < 0))) {
      value = -1; confirmed_at = 0;
    }
    if (writing || automatic < 0 || automatic_seen == 0) return;
    const uint32_t boundary = verifying ? verify_after : confirmed_at;
    const auto fresh = [&](uint32_t seen) {
      return seen != 0 && (boundary == 0 ||
          (verifying ? int32_t(seen - boundary) >= 0 : int32_t(seen - boundary) > 0));
    };
    if (!fresh(automatic_seen) || (automatic == 0 && (on < 0 || !fresh(on_seen)))) return;
    // Manual mode depends on two separate replies. Never mix a new output
    // with the LC reading from before the command or previous polling cycle.
    value = automatic == 1 ? 0 : on == 1 ? 1 : 2;
    confirmed_at = automatic == 1 || int32_t(automatic_seen - on_seen) >= 0 ? automatic_seen : on_seen;
    verifying = false;
  }
};
static_assert(sizeof(ConfirmedMode) <= 12);
struct PendingControls {
  std::array<uint32_t, 6> values{}, after{};
  uint8_t pending{0}, writing{0};

  bool has(size_t index) const { return index < values.size() && (pending & (1U << index)); }
  void cancel(size_t index) { pending &= ~(1U << index); writing &= ~(1U << index); }
  void begin(size_t index, uint32_t value, uint32_t now) {
    values[index] = value; after[index] = now;
    pending |= 1U << index; writing |= 1U << index;
  }
  void ready(size_t index, uint32_t now) {
    if (!has(index)) return;
    after[index] = now; writing &= ~(1U << index);
  }
  bool observe(size_t index, int64_t value, uint32_t seen, uint32_t now) {
    if (!has(index)) return false;
    const bool confirmed = !(writing & (1U << index)) && value >= 0 && uint32_t(value) == values[index] &&
        seen != 0 && int32_t(seen - after[index]) >= 0;
    if (!confirmed && now - after[index] < 60000) return false;
    cancel(index); return true;
  }
};
static_assert(sizeof(PendingControls) <= 52);
inline bool matching_reply(uint16_t expected_address, uint32_t expected_request,
                           uint32_t expected_status, uint16_t expected_property,
                           uint16_t address, uint32_t request, uint32_t status,
                           uint16_t property, bool success) {
  return expected_address == address && expected_request == request &&
         ((!success && status == 0) || (expected_status == status && expected_property == property));
}
inline uint16_t capability(uint16_t model) {
  switch (model) {
    case 0x1000: return ONOFF;
    case 0x1300: return LIGHTNESS;
    case 0x130F: return LC;
    case 0x1100: return SENSOR;
    case 0x1203: return SCENE;  // 0x1200 is Time Server, not Scene Server.
    case 0x1206: return SCHEDULER;
    default: return 0;
  }
}

struct Element {
  uint16_t capabilities{0};
  uint16_t app_key{UNKNOWN_BINDING};
  uint16_t sensor_group{0};
};
struct Node {
  std::array<uint8_t, 16> uuid{};
  std::array<uint8_t, 16> device_key{};
  std::array<Element, MAX_ELEMENTS> elements{};
  char name[48]{};
  uint16_t address{0};
  uint16_t company_id{0};
  uint16_t product_id{0};
  uint8_t element_count{0};
  bool selected{false};
  bool nightmatiq{false};
  uint16_t scene{6};
};
struct Catalog {
  uint32_t magic{0x534D4331};
  uint16_t version{1};
  uint16_t count{0};
  std::array<uint8_t, 16> mesh_uuid{};
  std::array<Node, MAX_NODES> nodes{};
};
inline int element_with(const Node &node, uint16_t cap) {
  for (size_t i = 0; i < node.element_count && i < MAX_ELEMENTS; ++i)
    if ((node.elements[i].capabilities & cap) != 0) return int(i);
  return -1;
}
inline int output_element(const Node &node) {
  for (size_t i = 0; i < node.element_count && i < MAX_ELEMENTS; ++i)
    if ((node.elements[i].capabilities & (ONOFF | LIGHTNESS)) == (ONOFF | LIGHTNESS)) return int(i);
  return element_with(node, ONOFF);
}
inline uint16_t possible_functions(const Node &node) {
  uint16_t functions = 0;
  if (element_with(node, ONOFF) >= 0) functions |= F_OUTPUT;
  if (element_with(node, LIGHTNESS) >= 0) functions |= F_BRIGHTNESS;
  if (element_with(node, LC) >= 0) functions |= F_AUTO | F_THRESHOLD | F_RUN_TIME;
  if (element_with(node, SENSOR) >= 0) functions |= F_LUX | F_MOTION;
  if (node.nightmatiq && (functions & (F_OUTPUT | F_AUTO)) == (F_OUTPUT | F_AUTO) &&
      element_with(node, SCENE) >= 0) functions |= F_MODE;
  if (node.nightmatiq) functions &= ~(F_BRIGHTNESS | F_RUN_TIME | F_MOTION);
  return functions;
}
inline uint16_t initial_functions(const Node &node, uint16_t retained) {
  if (node.nightmatiq && node.company_id == 0x0563 && node.product_id == 0x1DCE)
    retained |= F_OUTPUT | F_AUTO | F_THRESHOLD | F_LUX | F_MODE;
  return retained & possible_functions(node);
}
inline bool same_node(const Node &a, const Node &b) {
  const std::array<uint8_t, 16> empty{};
  if (a.uuid != empty && b.uuid != empty) return a.uuid == b.uuid;
  return a.address == b.address && a.device_key == b.device_key;
}
inline uint32_t node_identity(const Node &node, const std::array<uint8_t, 16> &mesh_uuid) {
  uint32_t value = 2166136261U;
  const std::array<uint8_t, 16> empty{};
  const auto &bytes = node.uuid == empty ? node.device_key : node.uuid;
  for (auto byte : bytes) value = (value ^ byte) * 16777619U;
  for (auto byte : mesh_uuid) value = (value ^ byte) * 16777619U;
  return value == 0 ? 1 : value;
}
inline bool valid_catalog(const Catalog &catalog) {
  if (catalog.magic != 0x534D4331 || catalog.version != 1 || catalog.count > MAX_NODES) return false;
  for (size_t i = 0; i < catalog.count; ++i) {
    const auto &n = catalog.nodes[i];
    if (!unicast(n.address) || n.element_count == 0 || n.element_count > MAX_ELEMENTS ||
        !unicast(uint32_t(n.address) + n.element_count - 1) || n.name[47] != '\0') return false;
    for (size_t j = 0; j < i; ++j) {
      const auto &other = catalog.nodes[j];
      if (same_node(n, other) ||
          (n.address < uint32_t(other.address) + other.element_count &&
           other.address < uint32_t(n.address) + n.element_count)) return false;
    }
  }
  return true;
}
inline bool add_group(std::array<uint16_t, MAX_GROUPS> &groups, size_t &count, uint16_t address) {
  if (!group(address)) return address == 0;
  for (size_t i = 0; i < count; ++i) if (groups[i] == address) return true;
  if (count == groups.size()) return false;
  groups[count++] = address;
  return true;
}

struct SensorRecord { uint16_t property; const uint8_t *value; size_t length; };
struct SensorReadings {
  struct Value {
    uint32_t seen{0};
    uint16_t property{0};
    uint8_t element{0}, length{0};
    std::array<uint8_t, 8> raw{};
  };
  std::array<Value, 8> values{};
  uint8_t count{0};
  bool overflow{false};
  bool store(uint8_t element, const SensorRecord &record, uint32_t now) {
    if (element >= MAX_ELEMENTS || record.length > 127 ||
        (record.length != 0 && record.value == nullptr)) return false;
    size_t index = 0;
    for (; index < count; ++index)
      if (values[index].element == element && values[index].property == record.property) break;
    if (index == values.size()) { overflow = true; return false; }
    if (index == count) ++count;
    auto &value = values[index];
    value.element = element; value.property = record.property;
    value.length = record.length; value.seen = now;
    value.raw.fill(0);
    if (record.length) std::memcpy(value.raw.data(), record.value, std::min(record.length, value.raw.size()));
    return true;
  }
};
static_assert(sizeof(SensorReadings) <= 164, "Sensor diagnostics must remain bounded");
inline bool composition_allows(bool checked, bool matches) { return !checked || matches; }

struct DeviceDiagnostics {
  static constexpr uint32_t DURATION_MS = 180000;
  enum Event : uint8_t { SENT, SEND_ERROR, RESPONSE, TIMEOUT, INVALID, PUBLICATION };
  struct Packet {
    uint32_t sequence{}, elapsed_ms{}, opcode{};
    int32_t error{};
    uint16_t property{}, length{};
    uint8_t element{}, event{}, stored{};
    std::array<uint8_t, 128> raw{};
  };
  uint32_t id{}, started{}, next_probe{}, sequence{};
  uint16_t address{}, composition_length{};
  uint8_t node{}, elements{}, probe{}, count{}, head{};
  bool running{}, probe_complete{}, composition_valid{};
  std::array<uint8_t, 512> composition_data{};
  std::array<Packet, 8> packets{};

  bool active(uint32_t now) const { return running && now - started < DURATION_MS; }
  bool contains(uint16_t source, uint32_t now) const {
    return active(now) && source >= address && source - address < elements;
  }
  void start(uint32_t now, uint32_t session, uint8_t index, uint16_t target, uint8_t element_count) {
    *this = DeviceDiagnostics{};
    id = session; started = now; next_probe = now; node = index;
    address = target; elements = element_count; running = true;
  }
  void record(uint32_t now, uint16_t source, Event type, uint32_t opcode,
              uint16_t property = 0, const uint8_t *data = nullptr, size_t length = 0, int32_t error = 0) {
    if (!contains(source, now) || (length && data == nullptr)) return;
    auto &packet = packets[head]; packet = Packet{};
    packet.sequence = ++sequence; packet.elapsed_ms = now - started;
    packet.opcode = opcode; packet.property = property; packet.error = error;
    packet.element = source - address; packet.event = type;
    packet.length = std::min<size_t>(length, 65535);
    packet.stored = std::min(length, packet.raw.size());
    if (packet.stored) std::memcpy(packet.raw.data(), data, packet.stored);
    head = (head + 1) % packets.size();
    if (count < packets.size()) ++count;
  }
  const Packet &packet(size_t index) const {
    return packets[(head + packets.size() - count + index) % packets.size()];
  }
  void composition(uint32_t now, uint16_t source, const uint8_t *data, size_t length, bool valid) {
    if (!contains(source, now) || source != address || data == nullptr) return;
    composition_data.fill(0);
    composition_length = std::min<size_t>(length, 65535);
    composition_valid = valid;
    std::memcpy(composition_data.data(), data, std::min(length, composition_data.size()));
  }
};
static_assert(sizeof(DeviceDiagnostics) <= 2048, "One bounded diagnostic session per gateway");

struct StateTrace {
  enum Event : uint8_t { QUEUED, SEND, RESPONSE, TIMEOUT, SEND_ERROR };
  struct Entry {
    uint32_t at{}, opcode{}, request{};
    int32_t error{};
    uint16_t node{}, address{};
    uint8_t event{}, sdk_event{}, kind{}, step{}, tid{}, length{};
    int8_t before_on{-1}, before_auto{-1}, after_on{-1}, after_auto{-1};
    std::array<uint8_t, 3> raw{};
  };
  std::array<Entry, 32> entries{};
  uint32_t sequence{}, incident_at{};
  uint16_t incident_node{};
  uint8_t head{}, count{}, remaining{};
  bool captured{}, frozen{};

  void clear() {
    sequence = incident_at = 0; incident_node = 0;
    head = count = remaining = 0; captured = frozen = false;
  }

  void record(uint32_t now, uint16_t node, uint16_t address, Event event,
              uint32_t opcode, uint32_t request, uint8_t sdk_event, uint8_t kind,
              uint8_t step, uint8_t tid, int8_t before_on, int8_t before_auto,
              int8_t after_on, int8_t after_auto, const uint8_t *data = nullptr,
              size_t length = 0, int32_t error = 0) {
    if (frozen || (captured && node != incident_node) || (length && data == nullptr)) return;
    auto &entry = entries[head]; entry = Entry{};
    entry.at = now; entry.node = node; entry.address = address;
    entry.event = event; entry.opcode = opcode; entry.request = request;
    entry.sdk_event = sdk_event; entry.kind = kind; entry.step = step; entry.tid = tid;
    entry.before_on = before_on; entry.before_auto = before_auto;
    entry.after_on = after_on; entry.after_auto = after_auto; entry.error = error;
    entry.length = std::min(length, entry.raw.size());
    if (entry.length) std::memcpy(entry.raw.data(), data, entry.length);
    head = (head + 1) % entries.size();
    if (count < entries.size()) ++count;
    ++sequence;
    if (captured) {
      if (remaining && --remaining == 0) frozen = true;
    } else if (event == RESPONSE && error == 0 &&
               ((before_on == 1 && after_on == 0) || (before_auto == 1 && after_auto == 0))) {
      captured = true; incident_at = now; incident_node = node; remaining = 16;
    }
  }
  const Entry &entry(size_t index) const {
    return entries[(head + entries.size() - count + index) % entries.size()];
  }
};
static_assert(sizeof(StateTrace) <= 1280, "State trace must remain bounded per gateway");

inline uint32_t stale_interval(size_t fields) {
  return std::max<uint32_t>(180000, uint32_t(fields) * 3700 + 60000);
}
// Validate the entire packet before permitting a caller to publish any of it.
template<typename Consumer> bool sensor_records(const uint8_t *data, size_t size, Consumer consume) {
  if (size != 0 && data == nullptr) return false;
  const auto walk = [&](bool publish) {
    size_t offset = 0;
    while (offset < size) {
      const bool b = (data[offset] & 1) != 0;
      const size_t header = b ? 3 : 2;
      if (size - offset < header) return false;
      const uint8_t encoded = b ? data[offset] >> 1 : (data[offset] >> 1) & 15;
      const size_t length = b && encoded == 127 ? 0 : size_t(encoded) + 1;
      if (size - offset - header < length) return false;
      const uint16_t property = b ? le16(data + offset + 1) : le16(data + offset) >> 5;
      if (publish) consume(SensorRecord{property, data + offset + header, length});
      offset += header + length;
    }
    return true;
  };
  return walk(false) && walk(true);
}

inline bool composition(const uint8_t *data, size_t size, std::array<uint16_t, MAX_ELEMENTS> &caps,
                        uint8_t &count) {
  if (data == nullptr || size < 10) return false;
  std::array<uint16_t, MAX_ELEMENTS> parsed{};
  size_t offset = 10;
  uint8_t elements = 0;
  while (offset < size) {
    if (size - offset < 4 || elements == MAX_ELEMENTS) return false;
    const uint8_t sig = data[offset + 2], vendor = data[offset + 3];
    offset += 4;
    const size_t needed = size_t(sig) * 2 + size_t(vendor) * 4;
    if (needed > size - offset) return false;
    for (size_t i = 0; i < sig; ++i) parsed[elements] |= capability(le16(data + offset + i * 2));
    offset += needed;
    ++elements;
  }
  if (elements == 0) return false;
  caps = parsed;
  count = elements;
  return true;
}
}  // namespace esphome::steinel_mesh::protocol
