#include "mesh_protocol.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
using namespace esphome::steinel_mesh::protocol;

int main() {
  {
    esphome::steinel_mesh::protocol::PendingControls controls;
    controls.begin(5,0,1000);
    assert(controls.has(5) && !controls.observe(5,0,2000,2000));
    controls.ready(5,3000);
    assert(!controls.observe(5,0,2999,3100));
    assert(!controls.observe(5,1,3100,3100));
    assert(controls.observe(5,0,3200,3200) && !controls.has(5));
    controls.begin(3,500,4000);controls.begin(3,600,5000);
    controls.ready(3,6000);
    assert(!controls.observe(3,500,6100,6100));
    assert(controls.observe(3,600,6200,6200));
    controls.begin(0,1,0xFFFFFF00);controls.ready(0,0xFFFFFFF0);
    assert(controls.observe(0,1,10,10));
    controls.begin(2,0,1000);controls.ready(2,2000);
    assert(!controls.observe(2,-1,0,61999));
    assert(controls.observe(2,-1,0,62000));
    assert(!controls.has(6));
  }
  StateTrace trace;
  uint8_t value=1;
  for (uint32_t i=0; i<40; ++i)
    trace.record(i,2,3,StateTrace::RESPONSE,0x8294,0x8291,0,3,0,0,1,1,1,1,&value,1);
  assert(trace.count==32 && trace.entry(0).at==8 && !trace.captured);
  value=0;
  trace.record(40,2,3,StateTrace::SEND,0x8293,0x8293,0,14,0,9,1,1,1,1,&value,1);
  assert(!trace.captured);
  trace.record(41,2,3,StateTrace::RESPONSE,0x8294,0x8291,0,3,0,0,1,1,1,0,&value,1);
  assert(trace.captured && !trace.frozen && trace.incident_at==41 && trace.remaining==16);
  trace.record(42,5,6,StateTrace::RESPONSE,0x8294,0x8291,0,3,0,0,1,1,1,0,&value,1);
  assert(trace.remaining==16);
  for (uint32_t i=0; i<16; ++i)
    trace.record(43+i,2,3,StateTrace::RESPONSE,0x8294,0x8291,0,3,0,0,1,0,1,1,&value,1);
  assert(trace.frozen && trace.entry(15).at==41 && trace.entry(31).at==58);
  const auto sequence=trace.sequence;
  trace.record(59,2,3,StateTrace::RESPONSE,0x8294,0x8291,0,3,0,0,1,1,1,0,&value,1);
  assert(trace.sequence==sequence);
  trace.clear(); assert(trace.count==0 && !trace.frozen && !trace.captured);
  assert(std::strcmp(product_name(0x0563, 0x1B1B), "L 830 SC") == 0);
  assert(std::strcmp(product_name(0x0563, 0x1E74), "L 820 SC") == 0);
  assert(std::strcmp(product_name(0x0563, 0x1DCE), "NightmatIQ Plus") == 0);
  assert(std::strcmp(product_name(0x0563, 0x1DE0), "IS 180") == 0);
  assert(std::strcmp(product_name(0x0563, 0x1E79), "L 810 SC") == 0);
  assert(std::strcmp(product_name(0x0563, 0x1EBD), "L 810 C") == 0);
  assert(std::strcmp(product_name(0x0563, 0x1F27), "L 42 SC") == 0);
  assert(!*product_name(0, 0x1DE0) && !*product_name(0x0563, 0xFFFF));
  assert(!*product_name(0, 0x1B1B) && !*product_name(0, 0x1E74));
  uint8_t major = 0, minor = 0, patch = 0;
  assert(firmware_from_vid(0x0563, 0x1DE0, 0x0883, major, minor, patch));
  assert(major == 1 && minor == 2 && patch == 3);
  assert(firmware_from_vid(0x0563, 0x1E79, 0x0841, major, minor, patch));
  assert(major == 1 && minor == 1 && patch == 1);
  assert(!firmware_from_vid(0x0563, 0x1DCE, 0x0841, major, minor, patch));
  assert(!firmware_from_vid(0, 0x1DE0, 0x0883, major, minor, patch));
  assert(!firmware_from_vid(0x0563, 0x1DE0, 0xFFFF, major, minor, patch));
  assert(firmware_from_vid(0x0563, 0x1B1B, 0x0883, major, minor, patch));
  assert(major == 1 && minor == 2 && patch == 3);
  assert(firmware_from_vid(0x0563, 0x1E74, 0x0841, major, minor, patch));
  assert(!firmware_from_vid(0x0563, 0, 0x0841, major, minor, patch));
  assert(!firmware_from_vid(0x0563, 0x1E74, 0, major, minor, patch));
  assert(composition_allows(false, false) && composition_allows(true, true));
  assert(!composition_allows(true, false));
  assert(stale_interval(1) == 180000 && stale_interval(112) == 474400);
  SensorReadings readings;
  const uint8_t raw[]={1,2,3,4,5,6,7,8,9,10};
  assert(readings.store(0,{0x004E,raw,3},1000));
  assert(readings.store(1,{0x004E,raw,3},1001) && readings.count == 2);
  assert(readings.store(0,{0x004E,raw,10},1002) && readings.count == 2);
  assert(readings.values[0].length == 10 && readings.values[0].raw[7] == 8);
  assert(readings.store(0,{0x004E,nullptr,0},1003));
  assert(readings.values[0].length == 0 && readings.values[0].raw[0] == 0);
  for(uint16_t i=0;i<6;++i) assert(readings.store(2,{uint16_t(0x100+i),raw,1},1004));
  assert(!readings.store(3,{0x9999,raw,1},1005) && readings.overflow && readings.count == 8);
  assert(readings.store(1,{0x004E,raw,1},1006) && readings.values[1].seen == 1006);
  assert(!readings.store(8,{0x004E,raw,1},1007));
  assert(!readings.store(0,{0x004E,nullptr,1},1007));
  char revision[17]{};
  const uint8_t version[] = {'1', '.', '2', '.', '3', 0, ' '};
  assert(revision_string(version, sizeof(version), revision) && std::strcmp(revision, "1.2.3") == 0);
  assert(!revision_string(version, 17, revision));
  const uint8_t binary_version[] = {1, 2, 3};
  assert(!revision_string(binary_version, 3, revision));
  unsigned descriptors_seen = 0;
  const uint8_t descriptors[] = {0x4E, 0, 0, 0, 0, 0, 0, 0, 0x42, 0, 0, 0, 0, 0, 0, 0};
  assert(sensor_descriptors(descriptors, sizeof(descriptors), [&](uint16_t property) {
    assert(property == 0x4E || property == 0x42); ++descriptors_seen;
  }) && descriptors_seen == 2);
  descriptors_seen = 0;
  assert(!sensor_descriptors(descriptors, 15, [&](uint16_t) { ++descriptors_seen; }) && descriptors_seen == 0);
  assert(sensor_descriptors(descriptors, 2, [&](uint16_t) { ++descriptors_seen; }) && descriptors_seen == 0);
  Node multiple; multiple.element_count = 2;
  multiple.elements[0].capabilities = ONOFF | SENSOR;
  multiple.elements[1].capabilities = ONOFF | LIGHTNESS | LC | SCENE;
  assert(output_element(multiple) == 1);
  assert(possible_functions(multiple) == 127);
  multiple.nightmatiq = true;
  const auto dusk_functions = F_OUTPUT | F_AUTO | F_THRESHOLD | F_LUX | F_MODE;
  assert(possible_functions(multiple) == dusk_functions);
  assert(!(possible_functions(multiple) & (F_BRIGHTNESS | F_RUN_TIME | F_MOTION)));
  multiple.company_id=0x0563; multiple.product_id=0x1DCE;
  assert(initial_functions(multiple,0) == dusk_functions);
  assert(initial_functions(multiple,255) == dusk_functions);
  multiple.nightmatiq=false;
  assert(initial_functions(multiple,0)==0 && initial_functions(multiple,127)==127);
  multiple.nightmatiq=true;
  multiple.elements[1].capabilities = 0;
  assert(possible_functions(multiple) == (F_OUTPUT | F_LUX));
  assert(initial_functions(multiple,255) == (F_OUTPUT | F_LUX));
  std::array<uint8_t, 16> network{}; network[0] = 1; multiple.uuid[0] = 9;
  const auto stable_identity = node_identity(multiple, network);
  multiple.address = 300;
  assert(node_identity(multiple, network) == stable_identity);
  network[0] = 2; assert(node_identity(multiple, network) != stable_identity);
  uint32_t integer = 99;
  assert(decimal_u32("4294967295", 10, 0, UINT32_MAX, integer) && integer == UINT32_MAX);
  assert(!decimal_u32("4294967296", 10, 0, UINT32_MAX, integer) && integer == UINT32_MAX);
  assert(!decimal_u32("-1", 2, 0, UINT32_MAX, integer));
  assert(!decimal_u32("+1", 2, 0, UINT32_MAX, integer));
  assert(!decimal_u32("1 ", 2, 0, UINT32_MAX, integer));
  assert(!decimal_u32("", 0, 0, UINT32_MAX, integer));
  assert(!decimal_u32("1", 1, 0, 0, integer));
  assert(!decimal_u32("0", 1, 1, 100, integer));
  assert(decimal_u32("0001", 4, 0, 100, integer) && integer == 1);
  assert(unicast(1) && unicast(0x7FFF) && !unicast(0) && !unicast(0x8000));
  assert(group(0xC000) && group(0xFEFF) && !group(0xFFFF) && !group(0xBFFF));
  assert(!deadline_pending(0x80000000U, 0));
  assert(deadline_pending(0xFFFFFFF0U, 20));
  assert(!deadline_pending(20, 20) && !deadline_pending(21, 20));
  assert(!deadline_pending(20, 0xFFFFFFF0U));
  assert(capability(0x1200) == 0 && capability(0x1203) == SCENE);
  assert(capability(0x1206) == SCHEDULER);
  assert(matching_reply(1, 2, 3, 4, 1, 2, 3, 4, true));
  assert(!matching_reply(1, 2, 3, 4, 5, 2, 3, 4, true));
  assert(!matching_reply(1, 2, 3, 4, 1, 5, 3, 4, true));
  assert(!matching_reply(1, 2, 3, 4, 1, 2, 5, 4, true));
  assert(!matching_reply(1, 2, 3, 4, 1, 2, 3, 5, true));
  assert(matching_reply(1, 2, 3, 4, 1, 2, 0, 0, false));
  assert(!matching_reply(1, 2, 3, 4, 1, 2, 3, 5, false));
  assert(!matching_reply(1, 2, 3, 4, 1, 2, 5, 4, false));
  assert(matching_reply(1, 2, 3, 4, 1, 2, 3, 4, false));
  Catalog catalog; catalog.count = 2;
  for (int i = 0; i < 2; ++i) {
    catalog.nodes[i].address = 1 + i * 3; catalog.nodes[i].element_count = 3;
    catalog.nodes[i].uuid[0] = i + 1;
  }
  assert(valid_catalog(catalog));
  catalog.nodes[1].address = 3; assert(!valid_catalog(catalog));
  catalog.nodes[1].address = 0x7FFF; assert(!valid_catalog(catalog));
  catalog.nodes[1].address = 4; catalog.nodes[1].uuid = catalog.nodes[0].uuid;
  assert(!valid_catalog(catalog));
  catalog.nodes[1].uuid[0] = 2; catalog.nodes[1].element_count = 9;
  assert(!valid_catalog(catalog));
  std::array<uint16_t, MAX_GROUPS> groups{}; size_t group_count = 0;
  for (size_t i = 0; i < MAX_GROUPS; ++i) assert(add_group(groups, group_count, 0xC000 + i));
  assert(add_group(groups, group_count, 0xC000));
  assert(!add_group(groups, group_count, 0xC010) && group_count == MAX_GROUPS);
  assert(!add_group(groups, group_count, 0x8000));
  // Format A: ambient illuminance 0x004E, length 3, 25.00 lx.
  const uint8_t lux[] = {0xC4, 0x09, 0xC4, 0x09, 0x00};
  unsigned emitted = 0;
  assert(sensor_records(lux, sizeof(lux), [&](const auto &r) {
    assert(r.property == 0x004E && r.length == 3 && le24(r.value) == 2500); ++emitted;
  }));
  assert(emitted == 1);
  // A valid record followed by malformed data must publish NOTHING.
  const uint8_t truncated[] = {0xC4, 0x09, 0xC4, 0x09, 0, 1};
  emitted = 0; assert(!sensor_records(truncated, sizeof(truncated), [&](const auto &) { ++emitted; }));
  assert(emitted == 0);
  const uint8_t absent[] = {0xFF, 0x4E, 0};
  assert(sensor_records(absent, 3, [&](const auto &r) { assert(r.property == 0x4E && r.length == 0); }));
  const uint8_t b[] = {5, 0x4E, 0, 0xFF, 0xFF, 0xFF};
  assert(sensor_records(b, 6, [&](const auto &r) { assert(le24(r.value) == UNKNOWN_24); }));
  const uint8_t composition_data[] = {0x63,5,0xE0,0x1D,1,0,0,0,0,0,
                                    0,0,2,0,0,0x10,0,0x13,
                                    0,0,1,0,0x0F,0x13};
  std::array<uint16_t, MAX_ELEMENTS> caps{}; uint8_t count = 0;
  assert(composition(composition_data, sizeof(composition_data), caps, count));
  assert(count == 2 && caps[0] == (ONOFF | LIGHTNESS) && caps[1] == LC);
  const auto original = caps;
  assert(!composition(composition_data, sizeof(composition_data) - 1, caps, count));
  assert(caps == original && count == 2);
  const uint8_t lamp_composition[] = {
    0x63,0x05,0x1B,0x1B,0x83,0x88,0x90,0x00,0x01,0x00,
    0,0,15,2,
    0,0,2,0,0,0x10,2,0x10,4,0x10,6,0x10,7,0x10,
    0,0x12,1,0x12,3,0x12,4,0x12,6,0x12,7,0x12,0,0x13,1,0x13,
    0x63,0x05,0x0B,0x10,0x63,0x05,0x0D,0x10,
    0,0,5,4,
    0,0x10,1,0x10,5,0x12,0x0F,0x13,0x10,0x13,
    0x63,0x05,1,0x10,0x63,0x05,4,0x10,0x63,0x05,5,0x10,0x63,0x05,6,0x10,
    0,0,2,1,0,0x11,1,0x11,0x63,0x05,3,0x10,
    0,0,2,0,0,0x11,1,0x11
  };
  assert(composition(lamp_composition, sizeof(lamp_composition), caps, count));
  assert(count == 4 && caps[0] == (ONOFF | LIGHTNESS | SCENE | SCHEDULER) &&
         caps[1] == (ONOFF | LC) && caps[2] == SENSOR && caps[3] == SENSOR);
  const auto lamp_caps = caps;
  assert(!composition(lamp_composition, sizeof(lamp_composition) - 1, caps, count));
  assert(caps == lamp_caps && count == 4);
  // Exercise all payload lengths under AddressSanitizer/UndefinedBehaviorSanitizer.
  std::mt19937 random(0x534D4331);
  for (int i = 0; i < 50000; ++i) {
    std::vector<uint8_t> payload(random() % 385);
    for (auto &byte : payload) byte = random();
    sensor_records(payload.data(), payload.size(), [&](const auto &r) {
      assert(r.value >= payload.data() && r.value + r.length <= payload.data() + payload.size());
    });
    composition(payload.data(), payload.size(), caps, count);
    sensor_descriptors(payload.data(), payload.size(), [](uint16_t) {});
    revision_string(payload.data(), payload.size(), revision);
  }
  std::printf("Protocol tests passed; Catalog=%zu bytes, Node=%zu bytes\n", sizeof(Catalog), sizeof(Node));
}
