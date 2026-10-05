"""Validate device discovery on the wire with the installed native API encoder."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

import esphome.components.api
from esphome import writer
from esphome.core import CORE
from aioesphomeapi.api_pb2 import DeviceInfoResponse as WireResponse

ROOT = Path(__file__).resolve().parents[1]
component = ROOT / "esphome/components/steinel_mesh"
spec = importlib.util.spec_from_file_location("native_api_devices", component / "native_api_devices.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
api_dir = Path(esphome.components.api.__file__).parent
source = (api_dir / "api_pb2.cpp").read_text()
header = (api_dir / "api_pb2.h").read_text()
patched = adapter.patch_device_info(source)
assert adapter.patch_device_info(patched) == patched
assert patched.count("if (it.device_id == 0 && it.name.empty()) continue;") == 2
assert source.count("if (it.device_id == 0 && it.name.empty()) continue;") == 0
try:
    adapter.patch_device_info(source.replace("this->devices)", "this->changed_devices)"))
except RuntimeError:
    pass
else:
    raise AssertionError("An incompatible encoder must stop the build")


def method(text, name):
    start = text.rfind("\n", 0, text.index(name)) + 1
    end = text.index("\n}\n", start) + 3
    return text[start:end]


def declaration(name):
    start = header.index(f"class {name} final")
    return header[start:header.index("\n};", start) + 3]


harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#define USE_DEVICES
#define ESPHOME_DEVICE_COUNT 16
#define PROTO_ENCODE_DEBUG_PARAM
#define PROTO_ENCODE_DEBUG_ARG
using StringRef = std::string_view;
struct ProtoMessage {};
struct ProtoWriteBuffer {
  uint8_t *pos;
  uint8_t *get_pos() {return pos;}
};
uint32_t varint_size(uint32_t value) {
  uint32_t size=1;
  while(value>=128){value>>=7;++size;}
  return size;
}
void varint(uint8_t *__restrict__ &pos,uint32_t value) {
  while(value>=128){*pos++=uint8_t(value)|0x80;value>>=7;}
  *pos++=value;
}
namespace ProtoSize {
uint32_t calc_uint32(uint32_t tag_size,uint32_t value) {
  return value ? tag_size+varint_size(value) : 0;
}
uint32_t calc_message_force(uint32_t tag_size,uint32_t size) {
  return tag_size+varint_size(size)+size;
}
}
namespace ProtoEncode {
void encode_uint32(uint8_t *__restrict__ &pos,uint32_t field,uint32_t value) {
  if(value){varint(pos,field<<3);varint(pos,value);}
}
void encode_short_string_force(uint8_t *__restrict__ &pos,uint8_t tag,StringRef value) {
  assert(value.size()<128);
  *pos++=tag;
  varint(pos,value.size());
  if(!value.empty())std::memcpy(pos,value.data(),value.size());
  pos+=value.size();
}
template<class T>
void encode_sub_message(uint8_t *__restrict__ &pos,ProtoWriteBuffer &,uint32_t field,const T &value) {
  varint(pos,(field<<3)|2);
  varint(pos,value.calculate_size());
  ProtoWriteBuffer nested{pos};
  pos=value.encode(nested);
}
}
'''
main = r'''
void emit(const DeviceInfoResponse &message) {
  std::array<uint8_t,4096> bytes{};
  ProtoWriteBuffer buffer{bytes.data()};
  const auto end=message.encode(buffer);
  assert(size_t(end-bytes.data())==message.calculate_size());
  assert(end<=bytes.data()+bytes.size());
  for(auto pos=bytes.data();pos<end;++pos)std::printf("%02x",*pos);
  std::puts("");
}
int main() {
  for(unsigned count : {0,1,4,16}) {
    DeviceInfoResponse message;
    message.name="gateway";message.friendly_name="Steinel Mesh Gateway";
    std::array<std::string,16> names;
    for(unsigned i=0;i<count;++i){
      names[i]="Device "+std::to_string(i);
      message.devices[i].device_id=100+i;
      message.devices[i].name=names[i];
      message.devices[i].area_id=i;
    }
    emit(message);
  }
  DeviceInfoResponse sparse;
  sparse.devices[8].device_id=800;sparse.devices[8].name="Eight";
  sparse.devices[15].device_id=1500;sparse.devices[15].name="Fifteen";
  emit(sparse);
  DeviceInfoResponse named_zero;
  named_zero.devices[0].name="Named device";
  emit(named_zero);
}
'''


def wire_messages(text, directory, label):
    production = "\n".join(method(text, name) for name in (
        "DeviceInfo::encode(", "DeviceInfo::calculate_size(",
        "DeviceInfoResponse::encode(", "DeviceInfoResponse::calculate_size("))
    declarations = declaration("DeviceInfo") + declaration("DeviceInfoResponse")
    binary = str(directory / label)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-x", "c++", "-", "-o", binary],
                   input=harness+declarations+production+main, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, check=True)
    return [WireResponse.FromString(bytes.fromhex(line)) for line in result.stdout.splitlines()]


with tempfile.TemporaryDirectory(prefix="steinel-native-api-") as temporary:
    directory = Path(temporary)
    before = wire_messages(source, directory, "before")
    assert len(before[1].devices) == 16
    assert sum(device.device_id == 0 and not device.name for device in before[1].devices) == 15
    after = wire_messages(patched, directory, "after")
    for index, count in enumerate((0, 1, 4, 16)):
        assert len(after[index].devices) == count
        assert [device.device_id for device in after[index].devices] == list(range(100, 100+count))
        assert after[index].friendly_name == "Steinel Mesh Gateway"
    assert [device.device_id for device in after[4].devices] == [800, 1500]
    assert len(after[5].devices) == 1 and after[5].devices[0].name == "Named device"
    target = directory / "api_pb2.cpp"
    target.write_text(source)
    adapter.patch_build(target)
    assert target.read_text() == patched
    copies = []

    def copy_sources():
        copies.append(True)
        target.write_text(source)

    with patch.object(writer, "copy_src_tree", copy_sources), \
            patch.object(CORE, "relative_src_path", return_value=str(target)):
        adapter.install_build_hook()
        hook = writer.copy_src_tree
        adapter.install_build_hook()
        assert writer.copy_src_tree is hook
        writer.copy_src_tree()
        assert target.read_text() == patched
        writer.copy_src_tree()
        assert target.read_text() == patched and len(copies) == 2
    assert (api_dir / "api_pb2.cpp").read_text() == source
    adapter.patch_build(target)
    assert target.read_text() == patched

codegen = (component / "__init__.py").read_text()
assert 'from .native_api_devices import install_build_hook' in codegen
assert 'install_build_hook()' in codegen
assert 'cg.add_define("ESPHOME_DEVICE_COUNT", 16)' in codegen
assert 'if (!node.selected) continue;' in (component / "steinel_nodes.cpp").read_text()
print("Native API discovery: actual encoder, wire decoding, 0/1/4/16 devices, sparse slots and build hook passed")
