"""Verify fair refreshing and recovery with near-timeout response latency."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
poll = runpy.run_path(str(ROOT / "tests/test_node_polling.py"))
harness = poll["harness"].replace("  void test_completion(uint32_t now);",
    "  void expire_values_(uint32_t now);\n  void test_completion(uint32_t now);")
source = poll["source"]
start = source.index("      for (size_t field = 0; field < stored.value_seen.size(); ++field)")
end = source.index("      const bool available =", start)
expiry = "void SteinelMesh::expire_values_(uint32_t now) {\n"
expiry += "for(size_t i=0;i<catalog_.count;++i){auto &stored=node_states_[i];\n" + source[start:end]
expiry += "}\n}\n"
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(THRESHOLD_PROPERTY==0x2B && RUN_PROPERTY==0x3C);
  for(uint32_t latency : {1000U,2000U,2500U,2900U}) for(bool expired_start : {false,true}) {
    SteinelMesh g; g.catalog_.count=16; test_now=1000;
    uint32_t seen[16][7]{}, max_gap=0;
    unsigned sensors[16][8]{},compositions[16]{};
    for(size_t i=0;i<16;++i) {
      auto &n=g.catalog_.nodes[i]; n.address=1+8*i; n.element_count=8; n.selected=true;
      for(auto &e:n.elements)e.capabilities=protocol::SENSOR;
      n.elements[0].capabilities|=protocol::ONOFF|protocol::LIGHTNESS|protocol::LC;
      auto &s=g.node_states_[i]; s.composition_checked=s.composition_matches=true;
      s.composition_at=test_now+IDENTITY_INTERVAL; s.last_seen=test_now;
      s.supported=127; s.lux_element=0; s.motion_element=7;
      for(size_t f=0;f<7;++f) { seen[i][f]=1000; s.value_seen[f]=expired_start?0:1000; }
      std::fill(std::begin(s.sensor_probes),std::end(s.sensor_probes),15);
      std::fill(std::begin(s.lux_probes),std::end(s.lux_probes),5);
    }
    while(test_now<(expired_start?3600000U:28800000U)) {
      g.expire_values_(test_now); g.sent=false; g.poll_nodes_(test_now);
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured; test_now+=latency;
      if(r.kind==SENSOR)++sensors[r.node][r.element];
      if(r.kind==COMPOSITION)++compositions[r.node];
      auto &s=g.node_states_[r.node]; s.last_seen=test_now;
      const auto note=[&](size_t f){
        max_gap=std::max(max_gap,test_now-seen[r.node][f]);
        seen[r.node][f]=s.value_seen[f]=test_now;
      };
      if(r.kind>=ON&&r.kind<=RUN)note(r.kind-ON);
      if(r.kind==SENSOR&&r.element==0)note(5);
      if(r.kind==SENSOR&&r.element==7)note(6);
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=true;
      g.test_completion(test_now);test_now+=200;
    }
    for(size_t i=0;i<16;++i)for(size_t f=0;f<7;++f)
      assert(g.node_states_[i].value_seen[f] && test_now-seen[i][f]<g.node_states_[i].stale_after);
    assert(max_gap < g.node_states_[0].stale_after);
    if(!expired_start)for(size_t i=0;i<16;++i){
      assert(compositions[i]>0);
      for(auto count:sensors[i])assert(count>0);
    }
    std::printf("Slow Mesh: latency=%ums expired_start=%d max_gap=%.2fs expiry=%.2fs\n",
                latency,expired_start,max_gap/1000.0,g.node_states_[0].stale_after/1000.0);
  }
}
'''
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-slow-") as directory:
        binary = str(Path(directory) / "slow")
        code = harness + poll["queue"]["REQUESTS"] + poll["queue"]["COMPLETION"] + expiry
        code += source[poll["start"]:poll["end"]] + main
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary], input=code, text=True, check=True)
        subprocess.run([binary], check=True)
