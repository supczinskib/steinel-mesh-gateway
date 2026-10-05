"""Run the production polling scheduler and completion block on a simulated clock."""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
queue = runpy.run_path(str(ROOT / "tests/test_node_queue.py"))
source = queue["SOURCE"]
header = (ROOT / "esphome/components/steinel_mesh/steinel_mesh.h").read_text()
state = header[header.index("  struct NodeState {"):header.index("  struct NodeRequest {")]
harness = re.sub(r"  struct NodeState \{[^\n]+\};", "  struct NodeEntities {};\n" + state, queue["HARNESS"])
harness = harness.replace("  void test_completion(uint32_t now);", r'''
  uint32_t node_poll_at_{0};
  bool sent{false};
  NodeRequest captured{};
  unsigned sync_calls{0};
  bool sync_node_functions_() { ++sync_calls; return false; }
  bool send_node_request_(const NodeRequest &request) { sent=true; captured=request; return true; }
  void poll_nodes_(uint32_t now);
  void test_completion(uint32_t now);
''')
start = source.index("void SteinelMesh::poll_nodes_(")
end = source.rindex("}  // namespace")
main = r'''
}
int main() {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  using namespace esphome::steinel_mesh;
  assert(THRESHOLD_PROPERTY==0x2B && RUN_PROPERTY==0x3C && IDENTITY_INTERVAL==21600000);
  // Every writable field gets targeted verification ahead of scheduled discovery.
  for(uint8_t field=0;field<5;++field) {
    SteinelMesh verification;verification.catalog_.count=1;
    auto &node=verification.catalog_.nodes[0];node.address=1;node.selected=true;node.element_count=2;
    node.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS;
    node.elements[1].capabilities=protocol::LC;
    auto &s=verification.node_states_[0];s.composition_checked=s.composition_matches=true;
    s.poll_at=test_now+30000;s.verify_fields=1U<<field;
    verification.poll_nodes_(test_now);
    assert(verification.sent&&verification.captured.kind==ON+field&&verification.captured.verification);
    assert(verification.captured.element==(field<2?0:1)&&s.verify_fields==0);
  }
  {
    SteinelMesh verification;verification.catalog_.count=1;
    auto &node=verification.catalog_.nodes[0];node.selected=true;node.element_count=2;
    node.elements[0].capabilities=protocol::ONOFF;node.elements[1].capabilities=protocol::LC;
    auto &s=verification.node_states_[0];s.composition_checked=s.composition_matches=true;
    s.verify_fields=5;s.verify_next=2;verification.poll_nodes_(test_now);
    assert(verification.captured.kind==AUTO);
    verification.active_node_request_=verification.captured;
    verification.node_completion_pending_=true;verification.node_completion_success_=false;
    verification.test_completion(test_now);
    verification.poll_nodes_(test_now);
    assert(verification.captured.kind==ON&&s.verify_fields==4); // A timeout must not block the other read.
  }
  {
    SteinelMesh night;night.catalog_.count=1;
    auto &node=night.catalog_.nodes[0];node.address=2;node.selected=true;node.nightmatiq=true;
    node.element_count=3;node.elements[0].capabilities=protocol::ONOFF|protocol::SCENE;
    node.elements[1].capabilities=protocol::LC;node.elements[2].capabilities=protocol::SENSOR;
    auto &s=night.node_states_[0];s.composition_checked=s.composition_matches=true;
    s.supported=protocol::F_RUN_TIME;s.value_seen[4]=1;s.poll_step=RUN;s.verify_fields=16;
    night.poll_nodes_(test_now);
    assert(s.verify_fields==0 && (!night.sent || night.captured.kind!=RUN));
  }
  // Offline nodes must not hold back entity registration for responding devices.
  SteinelMesh discovery;discovery.catalog_.count=16;
  for(size_t i=0;i<16;++i)discovery.catalog_.nodes[i].selected=true;
  discovery.node_states_[0].supported=protocol::F_OUTPUT;
  discovery.node_states_[0].discovery_complete=true;
  discovery.poll_nodes_(test_now);assert(discovery.sync_calls==1);
  discovery.node_poll_at_=0;discovery.node_states_[0].discovery_complete=false;
  discovery.poll_nodes_(test_now);assert(discovery.sync_calls==1);
  SteinelMesh g; g.catalog_.count=16;
  for (size_t i=0;i<16;++i) {
    auto &node=g.catalog_.nodes[i]; node.address=1+4*i; node.selected=true; node.element_count=3;
    node.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS;
    node.elements[1].capabilities=protocol::LC; node.elements[2].capabilities=protocol::SENSOR;
    auto &state=g.node_states_[i]; state.composition_checked=state.composition_matches=true; state.last_seen=1000;
  }
  uint32_t last[6]{}, max_gap=0;
  while (test_now<1200000) {
    g.sent=false; g.poll_nodes_(test_now);
    if (!g.sent) { test_now+=20; continue; }
    auto request=g.captured;
    test_now+=request.node==0 ? 10 : 3000;
    const bool success=request.node==0;
    if (success) {
      auto &state=g.node_states_[0]; state.last_seen=test_now;
      if (request.kind>=ON && request.kind<=RUN) {
        state.value_seen[request.kind-1]=test_now;
        auto &previous=last[request.kind-1];
        if (previous) max_gap=std::max(max_gap,test_now-previous);
        previous=test_now;
      }
      if (request.kind==SENSOR) state.value_seen[5]=test_now;
    }
    g.active_node_request_=request;g.node_completion_pending_=true;g.node_completion_success_=success;
    g.test_completion(test_now);test_now+=200;
  }
  std::printf("Production polling: healthy/offline refresh gap %.2fs\n",max_gap/1000.0);
  assert(max_gap<STALE_INTERVAL);
  for (auto previous:last) if(previous) assert(test_now-previous<STALE_INTERVAL);
  std::printf("Production polling: 1 healthy / 15 offline, maximum value refresh gap %.2fs < %.0fs\n",max_gap/1000.0,STALE_INTERVAL/1000.0);
  // Recently published Sensor values replace periodic polling; explicit refresh still reads them.
  g.catalog_.count=1;g.node_poll_index_=0;g.node_poll_at_=0;
  auto &state=g.node_states_[0];state.retry_at=state.poll_at=0;state.poll_step=6;
  state.sensor_push_at[2]=test_now;state.value_seen[5]=test_now;
  g.sent=false;g.poll_nodes_(test_now);
  assert(!g.sent || g.captured.kind!=SENSOR);
  // A stream of lux publications must not starve an independently stale motion value.
  state.supported|=protocol::F_MOTION;state.value_seen[6]=0;
  state.poll_step=16;state.poll_at=0;g.node_poll_at_=0;g.sent=false;
  g.poll_nodes_(test_now);
  assert(g.sent && g.captured.kind==SENSOR);
  state.poll_step=16;state.force_poll=true;state.poll_at=0;g.node_poll_at_=0;
  g.sent=false;g.poll_nodes_(test_now);
  assert(g.sent && g.captured.kind==SENSOR && g.captured.element==2);
  // Failure of initial composition does not prevent authenticated SIG reads.
  state.composition_checked=false;
  g.active_node_request_={0,0,COMPOSITION,0,0,0};g.node_completion_pending_=true;g.node_completion_success_=false;
  g.test_completion(test_now);
  assert(state.retry_at==0);
  state.composition_matches=false;state.poll_step=ON;state.poll_at=0;state.composition_at=test_now+60000;
  g.node_poll_at_=0;g.sent=false;g.poll_nodes_(test_now);
  assert(g.sent&&g.captured.kind==ON);
  state.composition_checked=true;
  state.composition_matches=true;
  // Every node responds to control/state, but ignores all optional metadata probes.
  for(uint32_t latency : {10U,1000U}) {
  SteinelMesh busy;busy.catalog_=g.catalog_;busy.catalog_.count=16;test_now=1000;
  for(size_t i=0;i<16;++i){
    busy.catalog_.nodes[i]=g.catalog_.nodes[i];
    auto &s=busy.node_states_[i];s.composition_checked=s.composition_matches=true;s.last_seen=test_now;
  }
  uint32_t seen[16][6]{},optional_gap=0;
  while(test_now<1200000){
    busy.sent=false;busy.poll_nodes_(test_now);
    if(!busy.sent){test_now+=20;continue;}
    auto r=busy.captured;
    const bool ok=r.kind!=DESCRIPTORS&&!(r.kind==SENSOR_PROPERTY&&r.value!=0x004E);
    test_now+=ok?latency:3000;
    auto &s=busy.node_states_[r.node];
    if(ok){
      s.last_seen=test_now;
      if(r.kind>=ON&&r.kind<=RUN){auto &previous=seen[r.node][r.kind-1];
        if(previous) optional_gap=std::max(optional_gap,test_now-previous);previous=test_now;
        s.value_seen[r.kind-1]=test_now;}
      if(r.kind==SENSOR)s.value_seen[5]=test_now;
    }
    busy.active_node_request_=r;busy.node_completion_pending_=true;busy.node_completion_success_=ok;
    busy.test_completion(test_now);test_now+=200;
  }
  std::printf("Production polling: 16 healthy / missing metadata / %ums latency, maximum value refresh gap %.2fs\n",latency,optional_gap/1000.0);
  assert(optional_gap<STALE_INTERVAL);
  }
  // Zero is an inactive deadline, also beyond 2^31 ms and across clock wrap.
  for(uint32_t now : {0x7FFFFFFFU,0x80000000U,0x80000001U,0xFFFFFFF0U,20U}) {
    SteinelMesh uptime;uptime.catalog_.count=1;
    auto &node=uptime.catalog_.nodes[0];node.address=1;node.element_count=1;node.selected=true;
    node.elements[0].capabilities=protocol::ONOFF;
    auto &s=uptime.node_states_[0];s.composition_checked=s.composition_matches=true;
    s.last_seen=now;s.poll_step=ON;s.poll_at=s.retry_at=0;test_now=now;
    uptime.poll_nodes_(now);assert(uptime.sent&&uptime.captured.kind==ON);
    uptime.sent=false;s.poll_step=ON;s.poll_at=now+100;uptime.node_poll_at_=0;
    uptime.poll_nodes_(now);assert(!uptime.sent);
    uptime.node_poll_at_=0;s.poll_step=ON;test_now=now+100;
    uptime.poll_nodes_(test_now);assert(uptime.sent);
    uptime.sent=false;uptime.node_poll_at_=0;s.poll_step=ON;s.poll_at=0;s.retry_at=now+200;
    uptime.poll_nodes_(now);assert(!uptime.sent);
    uptime.node_poll_at_=0;s.poll_step=ON;test_now=now+200;
    uptime.poll_nodes_(test_now);assert(uptime.sent);
  }
  std::puts("Production polling: long uptime, zero deadlines and clock wrap passed");
  // Four-element lamps: output and LC are separate; both Sensor elements must be read.
  SteinelMesh lamps;lamps.catalog_.count=2;test_now=1000;
  for(size_t i=0;i<2;++i){
    auto &n=lamps.catalog_.nodes[i];n.address=0x100*(i+1);n.element_count=4;n.selected=true;
    n.company_id=0x0563;n.product_id=i==0?0x1B1B:0x1E74;
    n.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS|protocol::SCENE|protocol::SCHEDULER;
    n.elements[1].capabilities=protocol::ONOFF|protocol::LC;
    n.elements[2].capabilities=n.elements[3].capabilities=protocol::SENSOR;
    auto &s=lamps.node_states_[i];s.composition_checked=s.composition_matches=true;
    s.composition_at=test_now+IDENTITY_INTERVAL;s.last_seen=test_now;
  }
  unsigned lamp_controls[2][5]{},lamp_sensors[2][2]{};
  while(test_now<300000){
    lamps.sent=false;lamps.poll_nodes_(test_now);
    if(!lamps.sent){test_now+=20;continue;}
    auto r=lamps.captured;
    const bool ok=r.kind>=ON&&r.kind<=SENSOR;
    test_now+=ok?100:3000;auto &s=lamps.node_states_[r.node];
    if(r.kind>=ON&&r.kind<=RUN){
      assert(r.element==(r.kind<=LEVEL?0:1));
      ++lamp_controls[r.node][r.kind-ON];s.value_seen[r.kind-ON]=test_now;
    }
    if(r.kind==SENSOR){
      assert(r.element==2||r.element==3);++lamp_sensors[r.node][r.element-2];
      const size_t field=r.element==2?6:5;s.value_seen[field]=test_now;
      if(r.element==2)s.motion_element=2;else s.lux_element=3;
      s.supported|=1U<<field;
    }
    if(ok)s.last_seen=test_now;
    lamps.active_node_request_=r;lamps.node_completion_pending_=true;lamps.node_completion_success_=ok;
    lamps.test_completion(test_now);test_now+=200;
  }
  for(size_t i=0;i<2;++i){
    for(auto reads:lamp_controls[i])assert(reads>1);
    for(auto reads:lamp_sensors[i])assert(reads>1);
  }
  std::puts("Production polling: L 820 SC / L 830 SC output, LC and separate motion/lux elements passed");
  for(uint32_t latency : {10U,500U,1000U}) {
    SteinelMesh maximum;maximum.catalog_.count=16;test_now=1000;
    for(size_t i=0;i<16;++i){
      auto &n=maximum.catalog_.nodes[i];n.address=1+8*i;n.element_count=8;n.selected=true;
      for(auto &e:n.elements)e.capabilities=protocol::SENSOR;
      n.elements[0].capabilities|=protocol::ONOFF|protocol::LIGHTNESS|protocol::LC;
      auto &s=maximum.node_states_[i];s.composition_checked=s.composition_matches=true;s.last_seen=test_now;
    }
    uint32_t seen[16][7]{},max_gap=0;unsigned sensor_polls[16][8]{};
    while(test_now<1200000){
      maximum.sent=false;maximum.poll_nodes_(test_now);
      if(!maximum.sent){test_now+=20;continue;}
      auto r=maximum.captured;
      const bool ok=r.kind!=DESCRIPTORS&&!(r.kind==SENSOR_PROPERTY&&r.value!=0x004E);
      test_now+=ok?latency:3000;auto &s=maximum.node_states_[r.node];
      if(ok){
        s.last_seen=test_now;
        const auto note=[&](size_t field){auto &previous=seen[r.node][field];
          if(previous)max_gap=std::max(max_gap,test_now-previous);
          previous=test_now;s.value_seen[field]=test_now;};
        if(r.kind>=ON&&r.kind<=RUN)note(r.kind-1);
        if(r.kind==SENSOR){
          ++sensor_polls[r.node][r.element];
          if(r.element==0){note(5);s.lux_element=0;}
          if(r.element==7){note(6);s.motion_element=7;}
        }
      }
      maximum.active_node_request_=r;maximum.node_completion_pending_=true;maximum.node_completion_success_=ok;
      maximum.test_completion(test_now);test_now+=200;
    }
    std::printf("Production polling: 16 nodes / 8 sensor elements / %ums latency, maximum refresh gap %.2fs\n",latency,max_gap/1000.0);
    assert(max_gap<STALE_INTERVAL);
    for(size_t i=0;i<16;++i){
      for(size_t f=0;f<7;++f){
        if(seen[i][f]==0||test_now-seen[i][f]>=STALE_INTERVAL)
          std::printf("Missing/stale node=%zu field=%zu previous=%u now=%u\n",i,f,seen[i][f],test_now);
        assert(seen[i][f]!=0&&test_now-seen[i][f]<STALE_INTERVAL);
      }
      for(auto polls:sensor_polls[i])assert(polls!=0);
      assert(maximum.node_states_[i].discovery_complete);
    }
  }
}
'''
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-polling-") as directory:
        binary = str(Path(directory) / "polling")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=harness + queue["REQUESTS"] + queue["COMPLETION"] + source[start:end] + main,
                       text=True, check=True)
        subprocess.run([binary], check=True)
