"""Exercise partial replies and expiry with the production scheduler."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
poll = runpy.run_path(str(ROOT / "tests/test_node_polling.py"))
harness = poll["harness"].replace("  void test_completion(uint32_t now);",
                                "  unsigned expire_values_(uint32_t now);\n  void test_completion(uint32_t now);")
start = poll["source"].index("      for (size_t field = 0; field < stored.value_seen.size(); ++field)")
end = poll["source"].index("      const bool available =", start)
expiry = poll["source"][start:end].replace("stored.value_seen[field] = 0;",
                                        "if(field!=3&&field!=4)std::printf(\"Unexpected expiry node=%zu field=%zu age=%u now=%u\\n\",i,field,now-stored.value_seen[field],now); stored.value_seen[field] = 0; ++expired;")
method = "unsigned SteinelMesh::expire_values_(uint32_t now) { unsigned expired=0;\n"
method += "for(size_t i=0;i<catalog_.count;++i){auto &stored=node_states_[i];\n" + expiry
method += "}\n return expired;\n}\n"
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  std::setvbuf(stdout,nullptr,_IONBF,0);
  assert(THRESHOLD_PROPERTY==0x2B&&RUN_PROPERTY==0x3C);
  for(uint32_t latency : {100U,1000U}) for(int scenario : {0,1,2,3,4,5,6}) {
    SteinelMesh g;g.catalog_.count=16;test_now=1000;
    for(size_t i=0;i<16;++i){
      auto &n=g.catalog_.nodes[i];n.address=1+8*i;n.element_count=8;n.selected=true;
      for(auto &e:n.elements)e.capabilities=protocol::SENSOR;
      n.elements[0].capabilities|=protocol::ONOFF|protocol::LIGHTNESS|protocol::LC;
      auto &s=g.node_states_[i];s.composition_checked=s.composition_matches=true;
      s.composition_at=test_now+IDENTITY_INTERVAL;s.last_seen=test_now;
    }
    uint32_t seen[16][7]{},max_gap=0;unsigned expired=0,timeouts=0,probes[16][8]{},identities[16]{};
    while(test_now<(scenario==5?86400000U:3600000U)){
      expired+=g.expire_values_(test_now);
      g.sent=false;g.poll_nodes_(test_now);
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured;
      bool ok=r.kind!=DESCRIPTORS&&!(r.kind==SENSOR_PROPERTY&&r.value!=0x004E);
      if(scenario>=1&&r.kind==SENSOR_PROPERTY&&r.value==0x004E&&r.element!=0)ok=false;
      if(scenario==2&&(r.kind==THRESHOLD||r.kind==RUN))ok=false;
      if(scenario==4&&test_now>=600000&&(r.kind==THRESHOLD||r.kind==RUN))ok=false;
      if(scenario==6&&test_now>=600000&&test_now<1200000&&(r.kind==THRESHOLD||r.kind==RUN))ok=false;
      if(r.kind==SENSOR_PROPERTY&&r.value==0x004E)++probes[r.node][r.element];
      if(r.kind==COMPOSITION)++identities[r.node];
      test_now+=ok?latency:3000;auto &s=g.node_states_[r.node];
      if(ok){
        s.last_seen=test_now;
        auto note=[&](size_t field){auto &previous=seen[r.node][field];
          if(previous&&!(scenario==6&&(field==3||field==4)&&previous<1200000&&test_now>=1200000))
            max_gap=std::max(max_gap,test_now-previous);
          previous=test_now;s.value_seen[field]=test_now;s.supported|=1U<<field;};
        if(r.kind>=ON&&r.kind<=RUN)note(r.kind-1);
        if(r.kind==SENSOR&&r.element==0&&scenario!=3){note(5);s.lux_element=0;s.lux_property=false;}
        if(r.kind==SENSOR_PROPERTY&&r.value==0x004E&&r.element==0){note(5);s.lux_element=0;s.lux_property=true;}
        if(r.kind==SENSOR&&r.element==7){note(6);s.motion_element=7;}
      }else ++timeouts;
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=ok;
      g.test_completion(test_now);test_now+=200;
    }
    std::printf("Partial replies: scenario=%d latency=%ums max_gap=%.2fs expired=%u timeouts=%u\n",
                scenario,latency,max_gap/1000.0,expired,timeouts);
    assert(max_gap<g.node_states_[0].stale_after&&expired==0);
    for(size_t i=0;i<16;++i){
      for(size_t f=0;f<7;++f){
        if((scenario==2||scenario==4)&&(f==3||f==4))continue;
        if(seen[i][f]==0||test_now-seen[i][f]>=g.node_states_[i].stale_after)
          std::printf("Missing/stale node=%zu field=%zu previous=%u now=%u\n",i,f,seen[i][f],test_now);
        assert(seen[i][f]!=0&&test_now-seen[i][f]<g.node_states_[i].stale_after);
      }
      for(size_t e=1;e<8;++e)assert(probes[i][e]<=5);
      if(scenario==3){
        if(probes[i][0]<=5||!g.node_states_[i].lux_property)
          std::printf("Explicit lux node=%zu probes=%u seen=%u property=%d\n",i,probes[i][0],seen[i][5],g.node_states_[i].lux_property);
        assert(probes[i][0]>5&&g.node_states_[i].lux_property);
      }
      if(scenario==4)assert(g.node_states_[i].value_seen[3]!=0&&g.node_states_[i].value_seen[4]!=0);
      if(scenario==5)assert(identities[i]>=3);
    }
  }
  std::puts("Partial replies: confirmed fields survive, unknown probes are bounded, explicit lux keeps refreshing");
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-partial-") as directory:
        binary = str(Path(directory) / "partial")
        code = harness + poll["queue"]["REQUESTS"] + poll["queue"]["COMPLETION"] + method
        code += poll["source"][poll["start"]:poll["end"]] + main
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary], input=code, text=True, check=True)
        subprocess.run([binary], check=True)
