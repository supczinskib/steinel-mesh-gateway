"""Keep responding functions live when a different model omits its replies."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
poll = runpy.run_path(str(ROOT / "tests/test_node_polling.py"))
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  std::setvbuf(stdout,nullptr,_IONBF,0);
  assert(THRESHOLD_PROPERTY==0x2B&&RUN_PROPERTY==0x3C);
  for(unsigned count : {1U,16U}) for(uint32_t latency : {100U,1000U}) {
    SteinelMesh g;g.catalog_.count=count;test_now=1000;
    for(unsigned i=0;i<count;++i) {
      auto &n=g.catalog_.nodes[i];n.address=1+8*i;n.element_count=4;n.selected=true;
      n.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS;
      n.elements[1].capabilities=protocol::LC;
      n.elements[2].capabilities=n.elements[3].capabilities=protocol::SENSOR;
      auto &s=g.node_states_[i];s.composition_checked=s.composition_matches=true;
      s.composition_at=test_now+IDENTITY_INTERVAL;
    }
    uint32_t seen[16][7]{},max_gap=0,discovered_at[16]{};unsigned on_attempts[16]{};
    while(test_now<1200000) {
      g.sent=false;g.poll_nodes_(test_now);
      for(unsigned i=0;i<count;++i)
        if(g.node_states_[i].discovery_complete&&!discovered_at[i])discovered_at[i]=test_now;
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured;
      // A missing LC Mode Status, OnOff Status, or one Sensor element must
      // not pause unrelated controls and the remaining Sensor element.
      const unsigned missing=r.node%3;
      if(r.kind==ON)++on_attempts[r.node];
      const bool ok=r.kind!=DESCRIPTORS&&r.kind!=SENSOR_PROPERTY&&
          !(missing==0&&r.kind==AUTO)&&!(missing==1&&r.kind==ON)&&
          !(missing==2&&r.kind==SENSOR&&r.element==2)&&
          !(missing==0&&r.kind==ON&&on_attempts[r.node]<=2);
      test_now+=ok?latency:3000;
      auto &s=g.node_states_[r.node];
      if(ok) {
        s.last_seen=test_now;
        const auto note=[&](size_t field) {
          auto &previous=seen[r.node][field];
          if(previous)max_gap=std::max(max_gap,test_now-previous);
          previous=test_now;s.value_seen[field]=test_now;s.supported|=1U<<field;
        };
        if(r.kind>=ON&&r.kind<=RUN)note(r.kind-ON);
        if(r.kind==SENSOR&&r.element==2){note(6);s.motion_element=2;}
        if(r.kind==SENSOR&&r.element==3){note(5);s.lux_element=3;}
      }
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=ok;
      g.test_completion(test_now);test_now+=200;
    }
    std::printf("Missing model replies: %u nodes, %ums latency, refresh gap %.2fs, first discovery %.2fs\n",
                count,latency,max_gap/1000.0,discovered_at[0]/1000.0);
    assert(max_gap<(count==1?60000:g.node_states_[0].stale_after));
    for(unsigned i=0;i<count;++i) {
      assert(discovered_at[i]&&discovered_at[i]<(count==1?30000U:180000U));
      for(size_t f=0;f<7;++f) {
        if((i%3==0&&f==2)||(i%3==1&&f==0)||(i%3==2&&f==6))continue;
        assert(seen[i][f]&&test_now-seen[i][f]<g.node_states_[i].stale_after);
      }
    }
  }
  // A single missed property response must be retried before its value expires.
  for(unsigned count : {1U,16U}) for(uint32_t latency : {100U,1000U}) {
    SteinelMesh g;g.catalog_.count=count;test_now=1000;
    for(unsigned i=0;i<count;++i) {
      auto &n=g.catalog_.nodes[i];n.address=1+8*i;n.element_count=1;n.selected=true;
      n.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS|protocol::LC|protocol::SENSOR;
      auto &s=g.node_states_[i];s.composition_checked=s.composition_matches=true;
      s.composition_at=test_now+IDENTITY_INTERVAL;
    }
    uint32_t seen[16][7]{},max_gap=0;unsigned attempts[16][2]{};
    while(test_now<1200000) {
      g.sent=false;g.poll_nodes_(test_now);
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured;
      const bool property=r.kind==THRESHOLD||r.kind==RUN;
      const bool ok=r.kind!=DESCRIPTORS&&r.kind!=SENSOR_PROPERTY&&
          (!property||++attempts[r.node][r.kind-THRESHOLD]%3!=2);
      test_now+=ok?latency:3000;
      auto &s=g.node_states_[r.node];
      if(ok) {
        s.last_seen=test_now;
        const auto note=[&](size_t field) {
          auto &previous=seen[r.node][field];
          if(previous)max_gap=std::max(max_gap,test_now-previous);
          previous=test_now;s.value_seen[field]=test_now;s.supported|=1U<<field;
        };
        if(r.kind>=ON&&r.kind<=RUN)note(r.kind-ON);
        if(r.kind==SENSOR){note(5);note(6);s.lux_element=s.motion_element=0;}
      }
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=ok;
      g.test_completion(test_now);test_now+=200;
    }
    std::printf("Lost property replies: %u nodes, %ums latency, refresh gap %.2fs\n",
                count,latency,max_gap/1000.0);
    assert(max_gap<g.node_states_[0].stale_after);
    for(unsigned i=0;i<count;++i)for(size_t f=0;f<7;++f)
      assert(seen[i][f]&&test_now-seen[i][f]<g.node_states_[i].stale_after);
  }
  std::puts("Missing model replies: independent discovery and freshness passed");
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-missing-replies-") as directory:
        binary = str(Path(directory) / "missing")
        code = poll["harness"] + poll["queue"]["REQUESTS"] + poll["queue"]["COMPLETION"]
        code += poll["source"][poll["start"]:poll["end"]] + main
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary], input=code, text=True, check=True)
        subprocess.run([binary], check=True)
