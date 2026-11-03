"""Exercise the production OTA request handler for RC-to-stable updates."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
directory = ROOT / 'esphome/components/steinel_mesh'
source = (directory / 'steinel_web.cpp').read_text()


def method(start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]


constants = '\n'.join(re.findall(
    r'static const char \*const RELEASE_(?:DOWNLOAD|ASSET)_PREFIX\s*=\s*"[^"]+";', source))
code = r'''
#include "mesh_protocol.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <cstdio>
#define ESPHOME_PROJECT_VERSION "2.0.1-rc.5"
namespace esphome::steinel_mesh {
uint32_t millis(){return 1000;}
struct esp_partition_t {uint32_t size=0x1c0000;};
esp_partition_t partition;
const esp_partition_t *esp_ota_get_next_update_partition(void*){return &partition;}
struct AsyncWebServerRequest {
  std::map<std::string,std::string> args;
  int status=0;std::string body;
  std::string arg(const char *name){return args[name];}
};
class SteinelMesh {
 public:
  static constexpr size_t AUTO_UPDATE_VERSION_MAX_LENGTH=23,AUTO_UPDATE_URL_MAX_LENGTH=255;
  struct StoredAutoUpdate {
    uint32_t image_size=0;char target_version[24]{},url[256]{};std::array<uint8_t,32>sha256{};
  } saved;
  std::atomic<bool>cloud_busy_{false},auto_update_running_{false},reboot_pending_{false};
  unsigned saves=0;uint32_t reboot_at_=0;
  static bool parse_version_(const std::string&,std::array<uint16_t,3>&);
  static bool parse_sha256_(const std::string&,std::array<uint8_t,32>&);
  static bool parse_u32_(const std::string&text,uint32_t low,uint32_t high,uint32_t&value){
    return protocol::decimal_u32(text.data(),text.size(),low,high,value);
  }
  static void send_json_(AsyncWebServerRequest*r,int code,const std::string&body){r->status=code;r->body=body;}
  bool save_auto_update_(const StoredAutoUpdate&update){saved=update;++saves;return true;}
  void set_status_(const char*){}
  void handle_auto_update_(AsyncWebServerRequest*);
};
'''
code += constants
code += method('bool SteinelMesh::parse_version_(', 'bool SteinelMesh::load_auto_update_(')
code += method('void SteinelMesh::handle_auto_update_(', 'bool SteinelMesh::cloud_get_(')
code += r'''
}
int main(){
  using namespace esphome::steinel_mesh;
  for(const char *version:{"2.0.0","2.0.1","2.0.2","2.1.0","2.0.1-rc.2","2.0.01","65536.0.0"}){
    SteinelMesh gateway;AsyncWebServerRequest request;
    request.args={{"version",version},{"url",std::string(RELEASE_DOWNLOAD_PREFIX)+version+"/"+RELEASE_ASSET_PREFIX+version+"-ota.bin"},
                  {"size","1555000"},{"sha256",std::string(64,'a')}};
    gateway.handle_auto_update_(&request);
    const bool accepted=std::string(version)=="2.0.1"||std::string(version)=="2.0.2"||std::string(version)=="2.1.0";
    assert(request.status==(accepted?200:400));
    assert(gateway.saves==unsigned(accepted)&&gateway.reboot_pending_==accepted);
    if(accepted){
      assert(gateway.saved.target_version==std::string(version)&&gateway.saved.url==request.args["url"]);
      assert(gateway.saved.image_size==1555000&&gateway.saved.sha256[0]==0xaa);
      std::array<uint16_t,3>parsed{};assert(SteinelMesh::parse_version_(gateway.saved.target_version,parsed));
    }
  }
  for(unsigned invalid=0;invalid<4;++invalid){
    SteinelMesh gateway;AsyncWebServerRequest request;
    request.args={{"version","2.0.1"},{"url",std::string(RELEASE_DOWNLOAD_PREFIX)+"2.0.1/"+RELEASE_ASSET_PREFIX+"2.0.1-ota.bin"},
                  {"size","1555000"},{"sha256",std::string(64,'a')}};
    if(invalid==0)request.args["url"]="https://other.invalid/firmware.bin";
    if(invalid==1)request.args["size"]="99999999";
    if(invalid==2)request.args["sha256"]="invalid";
    if(invalid==3)gateway.cloud_busy_=true;
    gateway.handle_auto_update_(&request);
    assert(request.status==(invalid==3?409:400)&&gateway.saves==0&&!gateway.reboot_pending_);
  }
  std::puts("OTA handler: RC -> stable accepted, older/RC/invalid targets rejected, URL/size/digest/busy checks retained");
}
'''
with tempfile.TemporaryDirectory(prefix='steinel-update-versions-') as temporary:
    binary = str(Path(temporary) / 'versions')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(directory), '-x', 'c++', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
