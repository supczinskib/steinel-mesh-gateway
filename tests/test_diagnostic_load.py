"""Verify production diagnostic probes do not expire healthy nodes under sustained load."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
slow = runpy.run_path(str(root / 'tests/test_node_slow_network.py'))
poll = slow['poll']
source = slow['source']
harness = slow['harness'].replace('  void poll_nodes_(uint32_t now);',
    '  bool next_diagnostic_request_(uint32_t,NodeRequest&);\n  void poll_nodes_(uint32_t now);')
probes = source[source.index('bool SteinelMesh::next_diagnostic_request_('):source.index('bool SteinelMesh::queue_node_command(')]
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(THRESHOLD_PROPERTY==0x2B && RUN_PROPERTY==0x3C);
  for(uint32_t latency : {1000U,2000U,2500U,2900U}) for(uint32_t diagnostic_timeout : {0U,3000U,3750U}) for(size_t selected_count : {15U,16U}) {
    SteinelMesh g;g.catalog_.count=16;test_now=1000;
    uint32_t seen[16][7]{},max_gap=0;unsigned expired=0,diagnostics=0;bool started=false;
    for(size_t i=0;i<16;++i){
      auto&n=g.catalog_.nodes[i];n.address=1+8*i;n.element_count=8;n.selected=i<selected_count;
      for(auto&e:n.elements)e.capabilities=protocol::SENSOR;
      n.elements[0].capabilities|=protocol::ONOFF|protocol::LIGHTNESS|protocol::LC;
      auto&s=g.node_states_[i];s.composition_checked=s.composition_matches=true;
      s.composition_at=test_now+IDENTITY_INTERVAL;s.last_seen=test_now;s.supported=127;s.lux_element=0;s.motion_element=7;
      for(size_t f=0;f<7;++f)seen[i][f]=s.value_seen[f]=n.selected?test_now:0;
      std::fill(std::begin(s.sensor_probes),std::end(s.sensor_probes),15);
      std::fill(std::begin(s.lux_probes),std::end(s.lux_probes),5);
    }
    while(test_now<1800000){
      if(!started&&test_now>=600000){g.device_diagnostics_.start(test_now,1,15,121,8);started=true;}
      for(size_t i=0;i<16;++i)for(size_t f=0;f<7;++f)
        if(g.node_states_[i].value_seen[f]&&test_now-g.node_states_[i].value_seen[f]>=g.node_states_[i].stale_after)++expired;
      g.expire_values_(test_now);g.sent=false;SteinelMesh::NodeRequest request{};
      if(protocol::deadline_pending(test_now,g.node_next_request_at_)){test_now+=20;continue;}
      if(g.next_diagnostic_request_(test_now,request))g.send_node_request_(request);
      else g.poll_nodes_(test_now);
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured;
      const uint32_t finish=test_now+(r.diagnostic&&diagnostic_timeout?
          diagnostic_timeout+(r.kind==COMPOSITION?1000U:0U):latency);
      while(test_now<finish){
        test_now=std::min(finish,test_now+20);
        for(size_t i=0;i<16;++i)for(size_t f=0;f<7;++f)
          if(g.node_states_[i].value_seen[f]&&test_now-g.node_states_[i].value_seen[f]>=g.node_states_[i].stale_after)++expired;
        g.expire_values_(test_now);
      }
      if(r.diagnostic)++diagnostics;
      else{
        auto&s=g.node_states_[r.node];s.last_seen=test_now;
        const auto note=[&](size_t f){max_gap=std::max(max_gap,test_now-seen[r.node][f]);seen[r.node][f]=s.value_seen[f]=test_now;};
        if(r.kind>=ON&&r.kind<=RUN)note(r.kind-ON);
        if(r.kind==SENSOR&&r.element==0)note(5);
        if(r.kind==SENSOR&&r.element==7)note(6);
      }
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=!r.diagnostic||!diagnostic_timeout;
      g.test_completion(test_now);test_now+=200;
    }
    assert(expired==0 && diagnostics>0 && max_gap<g.node_states_[0].stale_after);
    std::printf("Diagnostic load: selected=%zu latency=%u diag_timeout=%u max_gap=%.2fs expiry=%.2fs expirations=%u probes=%u complete=%d\n",
      selected_count,latency,diagnostic_timeout,max_gap/1000.0,g.node_states_[0].stale_after/1000.0,expired,diagnostics,g.device_diagnostics_.probe_complete);
  }
}
'''
with tempfile.TemporaryDirectory(prefix='steinel-diagnostic-load-') as directory:
    binary = str(Path(directory) / 'test')
    code = harness + poll['queue']['REQUESTS'] + poll['queue']['COMPLETION'] + slow['expiry']
    code += probes + source[poll['start']:poll['end']] + main
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I',str(root/'esphome/components/steinel_mesh'),
                    '-x','c++','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
