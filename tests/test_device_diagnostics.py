"""Exercise production read-only probes, session endpoints and sanitized streaming export."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
directory = ROOT / "esphome/components/steinel_mesh"
nodes = (directory / "steinel_nodes.cpp").read_text()
web = (directory / "steinel_web.cpp").read_text()
header = (directory / "steinel_mesh.h").read_text()
mesh = (directory / "steinel_mesh.cpp").read_text()
groups = mesh[mesh.index("bool SteinelMesh::clear_diagnostic_groups_("):mesh.index("bool SteinelMesh::restore_diagnostic_node_(")]
ready = mesh[mesh.index("void SteinelMesh::mark_ready_()"):mesh.index("void SteinelMesh::advance_address_recovery_(")]
state = header[header.index("  struct NodeState {"):header.index("  struct NodeRequest {")]
requests = nodes[nodes.index("enum Request :"):nodes.index("template<class Base>")]
methods = nodes[nodes.index("bool SteinelMesh::diagnostic_request_allowed_("):nodes.index("bool SteinelMesh::queue_node_command(")]
exporter = web[web.index("void SteinelMesh::handle_diagnostic_session_("):web.index("void SteinelMesh::handle_selection_(")]
writer = web[web.index("class StatusJsonWriter {"):web.index("\n};", web.index("class StatusJsonWriter {"))+3]
request = header[header.index("  struct NodeRequest {"):header.index("  struct StoredFunctions {")]
harness = r'''
#include "mesh_protocol.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cinttypes>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#define ESPHOME_PROJECT_VERSION "2.0.0"
namespace esphome::steinel_mesh {
uint32_t now=1000;
uint32_t millis(){return now;}
uint32_t esp_random(){return 0;}
struct httpd_req_t {std::string body;int status=200;};
constexpr int ESP_OK=0;
constexpr int HTTP_GET=0,HTTP_POST=1;
constexpr uint16_t ESP_BLE_MESH_CID_NVAL=0xFFFF,ESP_BLE_MESH_MODEL_ID_SENSOR_CLI=0x1102;
std::vector<uint16_t> subscriptions;
uint16_t subscribe_failure=0,unsubscribe_failure=0;
int esp_ble_mesh_model_subscribe_group_addr(uint16_t,uint16_t,uint16_t,uint16_t group){
  if(group==subscribe_failure)return -1;
  if(std::find(subscriptions.begin(),subscriptions.end(),group)==subscriptions.end()){
    if(subscriptions.size()==protocol::MAX_GROUPS)return -1;
    subscriptions.push_back(group);
  }
  return ESP_OK;
}
int esp_ble_mesh_model_unsubscribe_group_addr(uint16_t,uint16_t,uint16_t,uint16_t group){
  if(group==unsubscribe_failure)return -1;
  subscriptions.erase(std::remove(subscriptions.begin(),subscriptions.end(),group),subscriptions.end());return ESP_OK;
}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
int httpd_resp_set_type(httpd_req_t*,const char*){return 0;}
int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return 0;}
int httpd_resp_send_chunk(httpd_req_t*r,const char*d,size_t n){if(d)r->body.append(d,n);return 0;}
struct AsyncWebServerRequest {
  httpd_req_t raw;std::map<std::string,std::string> args;
  int method_ = HTTP_GET;
  int method() const { return method_; }
  std::string arg(const char*name){return args[name];}
  operator httpd_req_t*(){return &raw;}
};
void send_json_(AsyncWebServerRequest*r,int status,const std::string&body){r->raw.status=status;r->raw.body=body;}
struct esp_ble_mesh_client_common_param_t {struct {uint16_t addr;}ctx;uint32_t opcode;};
'''
declaration = r'''
class SteinelMesh {
 public:
  enum class AccessOperation {NONE,NODE};
  protocol::Catalog catalog_{};
  struct NodeEntities {};
  std::array<NodeState,protocol::MAX_NODES> node_states_{};
  struct Config {uint16_t app_key_index=3,local_address=0x7000;}config_;
  bool catalog_valid_=true;
  std::atomic<bool> mesh_ready_{true},cloud_busy_{false},reboot_pending_{false},auto_update_running_{false};
  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  std::atomic<uint16_t> node_destination_{0};
  std::atomic<uint32_t> access_opcode_{0},node_expected_status_{0};
  protocol::DeviceDiagnostics device_diagnostics_{};
  protocol::StateTrace state_trace_{};
  uint32_t diagnostic_session_id_{};
  std::array<uint16_t,protocol::MAX_ELEMENTS>diagnostic_groups_{};
  uint8_t diagnostic_group_count_{};
  bool prepare_diagnostic_groups_(const protocol::Node&);
  bool clear_diagnostic_groups_();
  void expire_diagnostic_groups_(uint32_t);
  bool address_recovery_attempted_this_boot_=false,device_key_valid_=true,legacy_profile_=false;
  bool keys_bound_pending_=false;
  uint32_t keys_bound_at_=0;
  struct Model {std::array<uint16_t,protocol::MAX_GROUPS>groups{};}model;
  Model *node_model_(uint8_t){model.groups.fill(0);std::copy(subscriptions.begin(),subscriptions.end(),model.groups.begin());return &model;}
  uint32_t mesh_ready_at_=0;
  std::atomic<uint32_t>composition_query_at_{0};
  std::atomic<bool>ready_publish_pending_{false},composition_query_pending_{false};
  std::atomic<unsigned>composition_query_attempts_{0},composition_query_failures_{0};
  void set_status_(const char*){}
  void mark_ready_();
  std::mutex node_mutex_;
  unsigned restores{};
  bool restore_diagnostic_node_(size_t){++restores;return true;}
  bool node_event_(const esp_ble_mesh_client_common_param_t*,uint32_t,bool,uint16_t){return true;}
  bool diagnostic_request_allowed_(const NodeRequest&);
  void record_diagnostic_(uint16_t,protocol::DeviceDiagnostics::Event,uint32_t,uint16_t=0,const uint8_t * =nullptr,size_t=0,int32_t=0);
  bool diagnostic_reply_(const esp_ble_mesh_client_common_param_t*,uint32_t,bool,uint16_t=0);
  bool next_diagnostic_request_(uint32_t,NodeRequest&);
  void handle_diagnostic_session_(AsyncWebServerRequest*);
  void handle_diagnostics_(AsyncWebServerRequest*);
  void handle_state_trace_(AsyncWebServerRequest*);
'''
main = r'''
}
int main(){
  using namespace esphome::steinel_mesh;
  assert(POLL_INTERVAL==30000&&STALE_INTERVAL==180000&&IDENTITY_INTERVAL==21600000&&POLL_STEPS>0);
  assert(THRESHOLD_PROPERTY==0x2B&&RUN_PROPERTY==0x3C);
  SteinelMesh g;g.catalog_.count=2;auto&n=g.catalog_.nodes[1];
  n.address=4321;n.company_id=0x0563;n.product_id=0x1E74;n.element_count=8;
  std::strcpy(n.name,"PRIVATE_LOCATION");n.device_key.fill(0xAD);n.uuid.fill(0xBE);
  std::strcpy(g.node_states_[1].firmware,"1.2.3");
  for(auto&e:n.elements){e.capabilities=protocol::SENSOR|protocol::LC|protocol::ONOFF|protocol::LIGHTNESS;e.app_key=3;}
  AsyncWebServerRequest start;start.args={{"action","start"},{"address","4321"}};
  g.handle_diagnostic_session_(&start);assert(start.raw.status==200&&g.restores==1);
  assert(!n.selected&&g.device_diagnostics_.id==1);
  AsyncWebServerRequest busy;busy.args=start.args;g.handle_diagnostic_session_(&busy);assert(busy.raw.status==409&&g.restores==1);
  SteinelMesh::NodeRequest request{};bool composition=false;unsigned sensor[8]{},descriptors[8]{};
  for(unsigned i=0;i<80;++i){
    if(g.next_diagnostic_request_(now,request)){
      assert(request.diagnostic&&request.node==1&&request.element<8&&g.diagnostic_request_allowed_(request));
      assert(!is_write(request.kind));
      if(request.kind==COMPOSITION)composition=true;
      if(request.kind==SENSOR)++sensor[request.element];
      if(request.kind==DESCRIPTORS)++descriptors[request.element];
    }
    now+=3100;
  }
  assert(composition&&g.device_diagnostics_.probe_complete);
  for(unsigned i=0;i<8;++i)assert(sensor[i]>0&&descriptors[i]==1);
  assert(!g.device_diagnostics_.active(now));
  for(uint8_t kind:{SET_ON,SET_LEVEL,SET_AUTO,SET_THRESHOLD,SET_RUN,SET_MODE}){
    request.kind=kind;assert(!g.diagnostic_request_allowed_(request));
  }
  now=1000;g.device_diagnostics_.start(now,1,1,4321,8);
  uint8_t composition_data[]={0x63,0x05,0x74,0x1E,0x83,0x08,0,0,0,0,0,0,1,1,0,0x11,0x63,0x05,0x23,0x81};
  g.device_diagnostics_.composition(now,4321,composition_data,sizeof(composition_data),true);
  uint8_t raw[200]{};for(unsigned i=0;i<200;++i)raw[i]=i;
  for(unsigned i=0;i<20;++i)g.record_diagnostic_(4321+i%8,protocol::DeviceDiagnostics::RESPONSE,0x52,0,raw,sizeof(raw));
  g.record_diagnostic_(4319,protocol::DeviceDiagnostics::RESPONSE,0x52,0,raw,sizeof(raw));
  assert(g.device_diagnostics_.sequence==20&&g.device_diagnostics_.count==8);
  assert(g.device_diagnostics_.packet(0).sequence==13&&g.device_diagnostics_.packet(7).sequence==20);
  assert(g.device_diagnostics_.packet(0).stored==128&&g.device_diagnostics_.packet(0).length==200);
  AsyncWebServerRequest wrong;wrong.args={{"id","2"}};g.handle_diagnostics_(&wrong);assert(wrong.raw.status==409);
  AsyncWebServerRequest get;get.args={{"id","1"}};g.handle_diagnostics_(&get);assert(get.raw.status==200);
  assert(get.raw.body.find("PRIVATE_LOCATION")==std::string::npos&&get.raw.body.find("4321")==std::string::npos);
  std::puts(get.raw.body.c_str());
  AsyncWebServerRequest stop;stop.args={{"action","stop"},{"id","2"}};g.handle_diagnostic_session_(&stop);assert(stop.raw.status==409&&g.device_diagnostics_.running);
  stop.args["id"]="1";g.handle_diagnostic_session_(&stop);assert(!g.device_diagnostics_.running&&!n.selected);
  request.kind=COMPOSITION;assert(!g.diagnostic_request_allowed_(request));
  // Expiry and relative times remain valid across millis() wrap.
  g.device_diagnostics_.start(0xFFFFFFF0,9,1,4321,8);
  g.device_diagnostics_.record(10,4321,protocol::DeviceDiagnostics::SENT,0x8008);
  assert(g.device_diagnostics_.packet(0).elapsed_ms==26);
  g.device_diagnostics_.record(0xFFFFFFF0+180000U,4321,protocol::DeviceDiagnostics::SENT,0x8008);
  assert(g.device_diagnostics_.sequence==1);
  // Unselected devices receive their own publications without changing selected subscriptions.
  now=1000;subscriptions.clear();SteinelMesh routing;routing.catalog_.count=2;
  auto&a=routing.catalog_.nodes[0];a.address=1;a.element_count=1;a.selected=true;
  a.elements[0]={protocol::SENSOR,3,0xC001};
  auto&b=routing.catalog_.nodes[1];b.address=10;b.element_count=3;
  b.elements[0]={protocol::ONOFF|protocol::SENSOR,3,0xC002};
  b.elements[1]={protocol::ONOFF|protocol::LIGHTNESS,3,0};
  b.elements[2]={protocol::SENSOR,3,0xC002};
  subscriptions.push_back(0xCFFF);routing.mesh_ready_=false;unsubscribe_failure=0xCFFF;
  routing.mark_ready_();assert(routing.keys_bound_pending_&&!routing.mesh_ready_);
  unsubscribe_failure=0;routing.mark_ready_();assert(subscriptions==std::vector<uint16_t>{0xC001});
  AsyncWebServerRequest begin;begin.args={{"action","start"},{"address","10"}};
  routing.handle_diagnostic_session_(&begin);assert(begin.raw.status==200&&!b.selected);
  assert((subscriptions==std::vector<uint16_t>{0xC001,0xC002})&&routing.diagnostic_group_count_==1);
  assert(routing.next_diagnostic_request_(now,request)&&request.kind==COMPOSITION);
  now+=3100;assert(routing.next_diagnostic_request_(now,request)&&request.kind==ON&&request.element==1);
  auto&protected_state=routing.node_states_[0];protected_state.composition_checked=protected_state.composition_matches=true;
  protected_state.value_seen[5]=1000;protected_state.stale_after=180000;
  now=151000;const auto probe=routing.device_diagnostics_.probe;
  assert(!routing.next_diagnostic_request_(now,request)&&routing.device_diagnostics_.probe==probe);
  protected_state.value_seen[5]=now;
  assert(routing.next_diagnostic_request_(now,request));
  AsyncWebServerRequest end;end.args={{"action","stop"},{"id","1"}};
  routing.handle_diagnostic_session_(&end);assert(end.raw.status==200);
  assert(subscriptions==std::vector<uint16_t>{0xC001});
  // Stop and expiry retry failed queue submissions; selected/shared groups are never removed.
  routing.handle_diagnostic_session_(&begin);assert(begin.raw.status==200);
  unsubscribe_failure=0xC002;end.args["id"]="2";routing.handle_diagnostic_session_(&end);
  assert(routing.diagnostic_group_count_==1);
  unsubscribe_failure=0;routing.expire_diagnostic_groups_(now);
  assert(routing.diagnostic_group_count_==0&&subscriptions==std::vector<uint16_t>{0xC001});
  routing.handle_diagnostic_session_(&begin);assert(begin.raw.status==200);
  routing.expire_diagnostic_groups_(now+protocol::DeviceDiagnostics::DURATION_MS);
  assert(subscriptions==std::vector<uint16_t>{0xC001});
  b.elements[2].sensor_group=0xC003;subscribe_failure=0xC003;now+=180001;
  routing.handle_diagnostic_session_(&begin);assert(begin.raw.status==503);
  assert(subscriptions==std::vector<uint16_t>{0xC001}&&routing.diagnostic_group_count_==0);
  subscribe_failure=0;b.elements[0].sensor_group=b.elements[2].sensor_group=0xC001;
  routing.handle_diagnostic_session_(&begin);assert(begin.raw.status==200&&routing.diagnostic_group_count_==0);
  routing.expire_diagnostic_groups_(now+180000);assert(subscriptions==std::vector<uint16_t>{0xC001});
  // Reject an over-capacity diagnostic session before adding any subscription.
  subscriptions.clear();SteinelMesh full;full.catalog_.count=16;
  for(size_t i=0;i<15;++i){auto&v=full.catalog_.nodes[i];v.address=1+8*i;v.element_count=1;v.selected=true;
    v.elements[0]={protocol::SENSOR,3,uint16_t(0xC001+i)};}
  full.catalog_.nodes[0].element_count=2;full.catalog_.nodes[0].elements[1]={protocol::SENSOR,3,0xC010};
  auto&unselected=full.catalog_.nodes[15];unselected.address=121;unselected.element_count=1;
  unselected.elements[0]={protocol::SENSOR,3,0xC011};full.mark_ready_();
  assert(subscriptions.size()==16);begin.args["address"]="121";
  full.handle_diagnostic_session_(&begin);assert(begin.raw.status==503&&!full.device_diagnostics_.running);
  assert(subscriptions.size()==16&&full.diagnostic_group_count_==0);
  uint8_t transition[]={0,1,0x41};
  g.state_trace_.record(1234,4321,4322,protocol::StateTrace::RESPONSE,0x8294,0x8291,
                       0,3,0,0,1,1,1,0,transition,sizeof(transition));
  AsyncWebServerRequest trace;
  g.handle_state_trace_(&trace);assert(trace.raw.status==200);
  assert(trace.raw.body.find("PRIVATE_LOCATION")==std::string::npos);
  std::puts(trace.raw.body.c_str());
  AsyncWebServerRequest reset;reset.method_=HTTP_POST;
  g.handle_state_trace_(&reset);assert(reset.raw.status==400&&g.state_trace_.count==1);
  reset.args["action"]="reset";
  g.handle_state_trace_(&reset);assert(reset.raw.status==200&&g.state_trace_.count==0&&!g.state_trace_.captured);
}
'''
with tempfile.TemporaryDirectory(prefix="steinel-diagnostics-") as temporary:
    binary = str(Path(temporary) / "diagnostics")
    code = harness + requests + writer + declaration.replace(" public:\n", " public:\n  struct NodeEntities;\n" + state + request, 1)
    code += "  NodeRequest active_node_request_{};\n};\n" + groups + ready + methods + exporter + main
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(directory), "-x", "c++", "-", "-o", binary],
                   input=code, text=True, check=True)
    exported = subprocess.check_output([binary], text=True).splitlines()
    report = json.loads(exported[0])
    trace = json.loads(exported[1])
    assert trace['captured'] and trace['incident_node'] == 4321 and not trace['frozen']
    assert trace['events'][0]['before_auto'] == 1 and trace['events'][0]['after_auto'] == 0
    assert trace['events'][0]['raw'] == '000141' and trace['events'][0]['request'] == 0x8291
    assert report["firmware"] == "1.2.3" and report["composition"]["valid"]
    assert all(packet["truncated"] and len(packet["raw"]) == 256 for packet in report["events"])
    assert set(report) == {"schema", "gateway_version", "id", "elapsed_ms", "running", "probe_complete", "sequence",
                           "company", "product", "firmware", "hardware", "elements", "composition", "events"}
    assert all(set(element) == {"index", "capabilities", "bound"} for element in report["elements"])
print("Device diagnostics: read-only probes, unselected nodes, privacy, limits, expiry and session validation passed")
