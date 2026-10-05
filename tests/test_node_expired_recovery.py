"""Recover previously confirmed properties after a burst of missing replies."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
slow = runpy.run_path(str(ROOT / "tests/test_node_slow_network.py"))
poll = slow["poll"]
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(THRESHOLD_PROPERTY == 0x2B && RUN_PROPERTY == 0x3C);
  std::setvbuf(stdout,nullptr,_IONBF,0);
  for (uint8_t missing : {uint8_t(THRESHOLD), uint8_t(RUN)}) {
    SteinelMesh g;g.catalog_.count=1;test_now=400000;
    auto &n=g.catalog_.nodes[0];n.address=2;n.element_count=3;n.selected=true;
    n.elements[0].capabilities=protocol::ONOFF;
    n.elements[1].capabilities=protocol::LC;
    n.elements[2].capabilities=protocol::SENSOR;
    auto &s=g.node_states_[0];s.composition_checked=s.composition_matches=true;
    s.discovery_complete=true;s.composition_at=test_now+IDENTITY_INTERVAL;
    s.supported=protocol::F_OUTPUT|protocol::F_AUTO|protocol::F_THRESHOLD|protocol::F_RUN_TIME|protocol::F_LUX;
    s.last_seen=test_now;s.value_seen.fill(test_now);
    s.value_seen[missing-ON]=test_now-STALE_INTERVAL;
    s.value_poll_at[missing-ON]=test_now-10000;
    s.lc_probes[missing-THRESHOLD]=5;
    std::fill(std::begin(s.sensor_probes),std::end(s.sensor_probes),15);
    std::fill(std::begin(s.lux_probes),std::end(s.lux_probes),5);
    const uint32_t start=test_now;bool recovered=false;
    while(test_now-start<65000 && !recovered) {
      g.expire_values_(test_now);g.sent=false;g.poll_nodes_(test_now);
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured;test_now+=100;
      if(r.kind==missing)recovered=true;
      s.last_seen=test_now;
      if(r.kind>=ON&&r.kind<=RUN)s.value_seen[r.kind-ON]=test_now;
      if(r.kind==SENSOR)s.value_seen[5]=test_now;
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=true;
      g.test_completion(test_now);test_now+=200;
    }
    std::printf("Expired property %u: recovery %.2fs\n",missing,(test_now-start)/1000.0);
    assert(recovered && test_now-start<65000);
  }
}
'''
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-expired-recovery-") as directory:
        binary = str(Path(directory) / "test")
        code = slow["harness"] + poll["queue"]["REQUESTS"] + poll["queue"]["COMPLETION"] + slow["expiry"]
        code += poll["source"][poll["start"]:poll["end"]] + main
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary], input=code, text=True, check=True)
        subprocess.run([binary], check=True)
