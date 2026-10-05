"""Check per-field expiry and native select invalidation notifications."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
poll = runpy.run_path(str(ROOT / "tests/test_node_polling.py"))
source = poll["source"]
start = source.index("void SteinelMesh::advance_nodes_(uint32_t now) {")
end = source.index("  if (this->node_completion_pending_.exchange(false))", start)
method = source[start:end] + "}\n"
entities = r'''
  struct TestEntity {
    bool known{false}, value{false}; float number{0}; unsigned events{}; std::string option;
    void publish_state(bool v) { known=true; value=v; ++events; }
    void publish_state(float v) { known=true; number=v; ++events; }
    void publish_state(const char *v) { known=true; option=v; ++events; }
    void invalidate_state() { if(known){known=false; ++events;} }
  };
  struct NodeEntities {
    TestEntity *available{}, *actual_output{}, *mode{}, *output{}, *automatic{};
    TestEntity *firmware{}, *hardware{}, *product{}, *model{}, *lux{}, *threshold{}, *run{}, *level{}, *motion{}, *diagnostics{};
  };
'''
select_stub = r'''
#define USE_SELECT
#define USE_CONTROLLER_REGISTRY
class MockSelect {
 public:
  bool known{false}; unsigned callbacks{0};
  bool has_state()const{return known;}
  void set_has_state(bool v){known=v;}
 protected:
  size_t active_index_{0};
  struct Callback { unsigned count{0};void call(size_t){++count;} } state_callback_;
 public:
  unsigned callback_count()const{return state_callback_.count;}
};
namespace select {using Select=MockSelect;}
template<class Base>class AttachedEntity:public Base{};
struct ControllerRegistry {static unsigned calls;static void notify_select_update(MockSelect *s){assert(!s->has_state());++calls;}};
unsigned ControllerRegistry::calls=0;
'''
select_class = source[source.index("class MeshSelect :"):source.index("class MeshDiagnostic final")]
harness = poll["harness"].replace("  struct NodeEntities {};", entities)
harness = harness.replace("class SteinelMesh {", select_stub + select_class + "class SteinelMesh {")
harness = harness.replace("  void poll_nodes_(uint32_t now);", "  void advance_nodes_(uint32_t now);\n  void poll_nodes_(uint32_t now);")
harness = harness.replace("#include <cstdio>", "#include <cstdio>\n#include <cmath>\n#include <string>")
main = r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(POLL_INTERVAL && STALE_INTERVAL && IDENTITY_INTERVAL && THRESHOLD_PROPERTY && RUN_PROPERTY && POLL_STEPS);
  MeshSelect native;native.set_has_state(true);native.invalidate_state();
  assert(!native.has_state()&&native.callback_count()==1&&ControllerRegistry::calls==1);
  native.invalidate_state();assert(native.callback_count()==1&&ControllerRegistry::calls==1);
  SteinelMesh g; g.catalog_.count=1;
  SteinelMesh::TestEntity available, actual, mode, output, automatic, identity, diagnostics, threshold, level, run;
  SteinelMesh::NodeEntities entities{};
  entities.available=&available; entities.actual_output=&actual; entities.mode=&mode;
  entities.output=&output; entities.automatic=&automatic;entities.diagnostics=&diagnostics;
  entities.threshold=&threshold;entities.level=&level;entities.run=&run;
  entities.firmware=entities.hardware=entities.product=entities.model=&identity;
  auto &s=g.node_states_[0];s.entities=&entities;s.composition_checked=s.composition_matches=true;
  s.on=1;s.automatic=0;s.value_seen[0]=s.value_seen[2]=1000;s.last_seen=1000;s.dirty=true;
  uint8_t data[10]={1,2,3,4,5,6,7,8,9,10};
  s.sensors.store(3,{0x8123,data,10},1000);
  test_now=1000;g.advance_nodes_(1000);
  assert(mode.known&&mode.option=="Always On"&&output.option=="On"&&automatic.option=="Off"&&actual.known);
  assert(diagnostics.option=="0003/8123=0102030405060708+");
  const unsigned mode_events=mode.events,output_events=output.events;
  // Other traffic must not preserve a stale mode or output state.
  s.last_seen=181000;s.value_seen[5]=181000;s.lux=2500;s.dirty=true;
  test_now=181000;g.advance_nodes_(181000);
  assert(s.on==-1&&s.automatic==-1&&s.was_available);
  assert(!actual.known&&!output.known&&!mode.known&&!automatic.known);
  assert(mode.events==mode_events+1&&output.events==output_events+1);
  assert(diagnostics.option=="0003/8123=?");
  test_now=361001;g.advance_nodes_(361001);
  assert(!s.was_available&&!available.value&&!diagnostics.known);
  // Fresh replies restore normal states, without optimistic command state.
  s.last_seen=361002;s.value_seen[0]=s.value_seen[2]=361002;s.on=0;s.automatic=1;s.dirty=true;
  test_now=361002;g.advance_nodes_(361002);
  assert(s.was_available&&mode.known&&mode.option=="Auto"&&output.option=="Off");
  // A callback can refresh a value after loop() samples its timestamp but before
  // advance_nodes_ obtains node_mutex_. The new reading is not 49 days old.
  test_now=361005;
  s.last_seen=s.value_seen[0]=s.value_seen[2]=361004;s.on=1;s.automatic=1;s.dirty=true;
  g.advance_nodes_(361003);
  assert(s.was_available&&s.on==1&&s.automatic==1&&mode.known&&mode.option=="Auto");
  test_now=2;
  s.last_seen=s.value_seen[0]=s.value_seen[2]=1;s.on=1;s.automatic=1;s.dirty=true;
  g.advance_nodes_(0xFFFFFFFF);
  assert(s.was_available&&s.on==1&&s.automatic==1&&mode.option=="Auto");
  // One new output reading must not be combined with the previous LC mode.
  test_now=1000;s.on=0;s.automatic=0;s.value_seen[0]=s.value_seen[2]=s.last_seen=1000;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always Off");
  test_now=1100;s.on=1;s.value_seen[0]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(actual.value&&mode.option=="Always Off");
  test_now=4500;s.automatic=1;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Auto");
  // Ignore partial replies while sending the command, then require readback.
  s.mode.begin_write();
  test_now=4600;s.automatic=0;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Auto");
  test_now=4700;s.mode.finish_write(test_now);
  test_now=4800;s.on=0;s.value_seen[0]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(!actual.value&&mode.option=="Auto");
  test_now=8500;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always Off");
  // LC-first readback also waits for a fresh output, without assuming success.
  s.mode.begin_write();s.mode.finish_write(8600);
  test_now=8700;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always Off");
  test_now=9000;s.on=1;s.value_seen[0]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On");
  // Superseding commands and unrelated replies must not confirm a partial mode.
  s.mode.begin_write();s.mode.finish_write(9100);
  test_now=9200;s.on=0;s.value_seen[0]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On");
  s.mode.begin_write();
  test_now=9300;s.automatic=1;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On");
  s.mode.finish_write(9400);
  test_now=9500;s.value_seen[5]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On");
  test_now=10000;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Auto");
  // Missing confirmation expires the old value, never publishes the request.
  s.mode.begin_write();s.mode.finish_write(10100);
  test_now=190000;s.last_seen=test_now;s.value_seen[5]=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(!mode.known);
  s.mode.finish_write(0xFFFFFF00);
  test_now=5;s.on=0;s.automatic=0;s.value_seen[0]=s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.known&&mode.option=="Always Off");
  // Requested settings are optimistic; measured output remains authenticated.
  test_now=100;s.controls.begin(5,0,test_now);s.mode.begin_write();s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Auto"&&!actual.value&&s.mode.value==2);
  test_now=200;s.controls.begin(5,1,test_now);s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On"&&!actual.value);
  test_now=300;s.controls.ready(5,test_now);s.mode.finish_write(test_now);
  test_now=350;s.automatic=0;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On"&&!actual.value&&s.controls.has(5));
  test_now=400;s.on=1;s.value_seen[0]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(mode.option=="Always On"&&actual.value&&!s.controls.has(5));
  test_now=500;s.controls.begin(0,0,test_now);s.dirty=true;
  g.advance_nodes_(test_now);assert(output.option=="Off"&&actual.value&&s.on==1);
  test_now=600;s.controls.ready(0,test_now);s.dirty=true;
  g.advance_nodes_(test_now);assert(output.option=="Off"&&actual.value);
  test_now=60600;s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(!s.controls.has(0)&&output.option=="On"&&actual.value);
  test_now=60700;s.controls.begin(2,1,test_now);s.dirty=true;
  g.advance_nodes_(test_now);assert(automatic.option=="On"&&s.automatic==0);
  test_now=60800;s.controls.ready(2,test_now);
  test_now=60900;s.automatic=1;s.value_seen[2]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);assert(!s.controls.has(2)&&automatic.option=="On");
  test_now=61000;
  s.threshold=500;s.run_time=30000;s.brightness=65535;s.brightness_known=true;
  s.value_seen[1]=s.value_seen[3]=s.value_seen[4]=s.last_seen=test_now;
  s.controls.begin(1,16384,test_now);s.controls.begin(3,2000,test_now);s.controls.begin(4,120000,test_now);s.dirty=true;
  g.advance_nodes_(test_now);
  assert(std::abs(level.number-25.0f)<0.01f&&threshold.number==20&&run.number==120);
  assert(s.brightness==65535&&s.threshold==500&&s.run_time==30000);
  test_now=61100;s.controls.ready(1,test_now);s.controls.ready(3,test_now);s.controls.ready(4,test_now);
  test_now=61200;s.brightness=16384;s.threshold=2000;s.run_time=120000;
  s.value_seen[1]=s.value_seen[3]=s.value_seen[4]=s.last_seen=test_now;s.dirty=true;
  g.advance_nodes_(test_now);
  assert(!s.controls.has(1)&&!s.controls.has(3)&&!s.controls.has(4));
  // Configuration persists while fresh measurements keep the device online.
  test_now=241201;s.last_seen=s.value_seen[5]=test_now;s.lux=10;s.dirty=true;
  g.advance_nodes_(test_now);
  assert(s.was_available&&s.threshold==2000&&s.run_time==120000);
  assert(threshold.number==20&&run.number==120&&!s.brightness_known);
  std::puts("State expiry: mode, On/Off controls, sensor diagnostics, recovery and native API notifications passed");
}
'''
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="steinel-expiry-") as directory:
        binary = str(Path(directory) / "expiry")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "esphome/components/steinel_mesh"),
                        "-x", "c++", "-", "-o", binary],
                       input=harness + poll["queue"]["REQUESTS"] + method + main,
                       text=True, check=True)
        subprocess.run([binary], check=True)
