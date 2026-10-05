"""Run the production BLE identity parser and address-recovery guards."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "esphome/components/steinel_mesh/steinel_mesh.cpp").read_text()


def function(start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


production = function("bool SteinelMesh::parse_scan_result_(", "void SteinelMesh::gap_scan_event_handler")
production += function("bool SteinelMesh::capture_advertised_identity_(", "void SteinelMesh::begin_identity_scan_")
production += function("void SteinelMesh::finish_identity_scan_()", "void SteinelMesh::persist_pending_advertised_identity_")
production += function("void SteinelMesh::advance_address_recovery_(", "void SteinelMesh::keys_bound_")
# The generic loop must reach recovery before its legacy-only exit.
loop = source[source.index("void SteinelMesh::loop()"):]
assert loop.index("this->advance_address_recovery_(now)") < loop.index("if (!this->legacy_profile_) return")

harness = r'''
#include "mesh_protocol.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
namespace esphome::steinel_mesh {
uint32_t millis(){return 1000;}
constexpr uint16_t STEINEL_COMPANY_ID=0x0563,NIGHTMATIQ_PRODUCT_ID=0x1DCE;
constexpr uint8_t ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE=0xFF;
constexpr uint32_t AUTO_ADDRESS_RECOVERY_DELAY_MS=60000,AUTO_ADDRESS_MIN_ACCEPTED_TX=10,AUTO_ADDRESS_MIN_TIMEOUTS=10;
namespace esp32_ble { struct BLEScanResult { uint8_t adv_data_len{},scan_rsp_len{},ble_adv[62]{},bda[6]{};int16_t rssi{-50}; }; }
class SteinelMesh {
 public:
  enum class AccessOperation {NONE,NODE};
  std::atomic<bool> identity_scan_pending_{true},identity_found_this_boot_{false},
    advertised_identity_fresh_{false},advertised_identity_save_pending_{false},advertised_identity_valid_{false},
    advertised_identity_publish_pending_{false},composition_received_{false},mesh_ready_{true},cloud_busy_{false},
    reboot_pending_{false},composition_query_in_flight_{false};
  std::atomic<uint16_t> advertised_product_id_{0},advertised_firmware_hash_{0},advertised_composition_version_id_{0},live_version_id_{0};
  std::atomic<uint8_t> advertised_firmware_major_{0},advertised_firmware_minor_{0},advertised_firmware_patch_{0},
    advertised_bootloader_version_{0},advertised_hardware_version_{0};
  std::atomic<int16_t> advertised_rssi_{0};
  std::atomic<uint32_t> mesh_rx_messages_{0},mesh_tx_accepted_{10},mesh_timeouts_{10};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  bool legacy_profile_{false},identity_advertiser_seen_{false},identity_advertiser_conflict_{false},
    identity_scan_started_{true},mesh_start_pending_{false},address_recovery_attempted_this_boot_{false},
    configured_{true},mesh_mode_enabled_{true},address_policy_valid_{true},confirmed{false};
  std::array<uint8_t,6> identity_advertiser_{};
  uint32_t mesh_start_not_before_{0},mesh_start_deadline_{0},mesh_ready_at_{1};unsigned rotations{0};
  void set_status_(const std::string&){}
  bool current_address_confirmed_()const{return confirmed;}
  bool rotate_local_address_(std::string&){++rotations;return true;}
  bool parse_scan_result_(const esp32_ble::BLEScanResult&);
  bool capture_advertised_identity_(const uint8_t*,size_t,int16_t);
  void finish_identity_scan_();void advance_address_recovery_(uint32_t);
};
'''
main = r'''
}
int main(){
  using namespace esphome::steinel_mesh;
  SteinelMesh g;esp32_ble::BLEScanResult scan{};
  const uint8_t adv[]={10,0xFF,0x63,0x05,0xCE,0x1D,1,2,3,4,5};
  std::memcpy(scan.ble_adv,adv,sizeof(adv));scan.adv_data_len=sizeof(adv);scan.bda[0]=1;
  assert(g.parse_scan_result_(scan));
  assert(g.advertised_firmware_major_==1&&g.advertised_firmware_minor_==2&&g.advertised_firmware_patch_==3);
  assert(g.advertised_hardware_version_==5&&g.identity_found_this_boot_);
  assert(g.parse_scan_result_(scan)&&!g.identity_advertiser_conflict_);
  scan.bda[0]=2;assert(!g.parse_scan_result_(scan)&&g.identity_advertiser_conflict_);
  g.finish_identity_scan_();
  assert(!g.identity_found_this_boot_&&!g.advertised_identity_valid_&&!g.advertised_identity_save_pending_);
  assert(g.mesh_start_pending_&&!g.identity_scan_pending_);
  SteinelMesh malformed;scan.adv_data_len=63;assert(!malformed.parse_scan_result_(scan));
  scan.adv_data_len=sizeof(adv)-1;assert(!malformed.parse_scan_result_(scan));
  scan.adv_data_len=sizeof(adv);scan.ble_adv[4]=0;assert(!malformed.parse_scan_result_(scan));
  SteinelMesh recover;
  recover.advance_address_recovery_(60000);assert(recover.rotations==0);
  recover.access_operation_=SteinelMesh::AccessOperation::NODE;
  recover.advance_address_recovery_(60001);assert(recover.rotations==0);
  recover.access_operation_=SteinelMesh::AccessOperation::NONE;
  recover.mesh_tx_accepted_=9;recover.advance_address_recovery_(60001);assert(recover.rotations==0);
  recover.mesh_tx_accepted_=10;recover.cloud_busy_=true;recover.advance_address_recovery_(60001);assert(recover.rotations==0);
  recover.cloud_busy_=false;recover.advance_address_recovery_(60001);assert(recover.rotations==1);
  recover.advance_address_recovery_(70000);assert(recover.rotations==1);
  SteinelMesh responding;responding.mesh_rx_messages_=1;responding.advance_address_recovery_(60001);assert(responding.rotations==0);
  responding.mesh_rx_messages_=0;responding.confirmed=true;responding.advance_address_recovery_(60001);assert(responding.rotations==0);
  SteinelMesh legacy;legacy.legacy_profile_=true;legacy.advance_address_recovery_(60001);assert(legacy.rotations==0);
  legacy.identity_found_this_boot_=true;legacy.advance_address_recovery_(60001);assert(legacy.rotations==1);
  std::puts("Production recovery/identity: bounded retries, active/confirmed guards, BLE bounds and ambiguous advertisers passed");
}
'''
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-identity-") as directory:
        binary = str(Path(directory) / "identity")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=harness+production+main,text=True,check=True)
        subprocess.run([binary],check=True)
