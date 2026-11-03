"""Replay public L 840 reports through production callbacks and test polling fairness."""
from pathlib import Path
import json
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/steinel_mesh'
callbacks = runpy.run_path(str(ROOT / 'tests/test_node_callbacks.py'))
diagnostics = runpy.run_path(str(ROOT / 'tests/test_device_diagnostics.py'))
polling = runpy.run_path(str(ROOT / 'tests/test_node_polling.py'))
source = callbacks['source']


def compile_run(name, code):
    with tempfile.TemporaryDirectory(prefix=f'steinel-{name}-') as directory:
        binary = str(Path(directory) / name)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(COMPONENT), '-x', 'c++', '-', '-o', binary],
                       input=code, text=True, check=True)
        subprocess.run([binary], check=True)


harness = callbacks['harness'].replace('uint32_t millis(){return 1000;}',
    'uint32_t test_now=1000; uint32_t millis(){return test_now;}')
harness = harness.replace('template<class P>bool diagnostic_reply_(const P*,uint32_t,bool,uint16_t=0){return false;}',
    'bool diagnostic_reply_(const esp_ble_mesh_client_common_param_t*,uint32_t,bool,uint16_t=0);')
definitions = 'enum { ' + ', '.join(f'{name}={i+1}' for i, name in enumerate(callbacks['constants'])) + ' };\n'
reply = source[source.index('bool SteinelMesh::diagnostic_reply_('):source.index('bool SteinelMesh::next_diagnostic_request_(')]
code = harness + definitions + callbacks['requests'] + source[callbacks['start']:callbacks['end']] + reply
code += '''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(POLL_INTERVAL==30000 && STALE_INTERVAL==180000 && IDENTITY_INTERVAL && POLL_STEPS);
'''
opcodes = {
    0x8204: ('ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS', 0x8201, 'ON'),
    0x824e: ('ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS', 0x824b, 'LEVEL'),
    0x8294: ('ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS', 0x8291, 'AUTO'),
}
for name, address in [('sc-near', 0x18), ('c-near', 0x0c), ('sc-far', 0x12)]:
    report = json.loads((ROOT / f'tests/fixtures/l840/{name}.json').read_text())
    assert report['schema'] == 'steinel-device-report/1'
    assert report['missed_events'] == 0 and not report['event_limit_reached']
    code += f'''
  {{
    SteinelMesh g;g.catalog_.count=1;auto &node=g.catalog_.nodes[0];
    node.address={address};node.selected=true;node.element_count={len(report['device']['elements'])};
    node.company_id={report['device']['company']};node.product_id={report['device']['product']};
    auto &state=g.node_states_[0];state.on=0;
    esp_ble_mesh_client_common_param_t params{{}};
    esp_ble_mesh_generic_client_cb_param_t generic{{}};generic.params=&params;
    esp_ble_mesh_light_client_cb_param_t light{{}};light.params=&params;
    uint32_t received_before=0;
'''
    for element in report['device']['elements']:
        code += f"    node.elements[{element['index']}].capabilities={element['capabilities']};\n"
    composition = report['device']['composition']
    if composition['valid']:
        data = ','.join(str(value) for value in bytes.fromhex(composition['raw']))
        code += f'''    const uint8_t composition[]={{{data}}};
    std::array<uint16_t,protocol::MAX_ELEMENTS> caps{{}};uint8_t count=0;
    assert(protocol::composition(composition,sizeof(composition),caps,count)&&count==node.element_count);
    assert(protocol::le16(composition)==node.company_id&&protocol::le16(composition+2)==node.product_id);
    for(size_t i=0;i<count;++i)assert(caps[i]==node.elements[i].capabilities);
'''
    pending = None
    for event in report['events']:
        if event['event'] == 0:
            pending = event['opcode']
        if event['event'] == 3:
            pending = None
        if event['event'] != 2 or event['opcode'] not in opcodes:
            continue
        status, request_opcode, kind = opcodes[event['opcode']]
        value = int.from_bytes(bytes.fromhex(event['raw']), 'little')
        diagnostic = pending == request_opcode
        code += f'''    test_now={1000 + event['elapsed_ms']};params.ctx.addr={address + event['element']};
    params.ctx.recv_op={status};params.opcode={request_opcode};
    g.active_node_request_={{0,{event['element']},{kind},0,0,0}};
    g.active_node_request_.diagnostic={'true' if diagnostic else 'false'};
    g.access_operation_=SteinelMesh::AccessOperation::{'NODE' if diagnostic else 'NONE'};
    g.node_destination_=params.ctx.addr;g.access_opcode_=params.opcode;g.node_expected_status_=params.ctx.recv_op;
    received_before=g.mesh_rx_messages_.load();
'''
        if kind == 'ON':
            code += f'''    generic.status_cb.onoff_status={{false,{value},0,0}};
    assert(g.node_generic_event_(0,&generic));assert(state.on=={value} && state.value_seen[0]==test_now);
'''
        elif kind == 'LEVEL':
            code += f'''    light.status_cb.lightness_status={{false,{value},0}};
    assert(g.node_light_event_(0,&light));assert(state.brightness=={value});
    assert(state.on=={int(value != 0)} && state.value_seen[0]==test_now);
'''
        else:
            code += f'''    light.status_cb.lc_mode_status.mode={value};
    assert(g.node_light_event_(0,&light));assert(state.automatic=={value});
'''
        if diagnostic:
            code += '    assert(g.completed && g.last_success && g.mesh_rx_messages_==received_before+1);\n'
            pending = None
        else:
            code += '    assert(g.mesh_rx_messages_==received_before+1);\n'
    code += f'    std::puts("L 840 report replay: {name} passed");\n  }}\n'

code += r'''
  {
    SteinelMesh g;g.catalog_.count=1;auto &node=g.catalog_.nodes[0];
    node.address=12;node.selected=true;node.element_count=2;
    node.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS;
    node.elements[1].capabilities=protocol::LC;
    auto &state=g.node_states_[0];state.on=1;state.automatic=1;
    esp_ble_mesh_client_common_param_t params{};params.ctx.addr=12;
    params.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS;
    esp_ble_mesh_light_client_cb_param_t light{};light.params=&params;
    light.status_cb.lightness_status={true,0,65535};
    assert(g.node_light_event_(0,&light));assert(state.on==0 && state.automatic==1);
    light.status_cb.lightness_status={true,200,0};
    assert(g.node_light_event_(0,&light));assert(state.on==1);
    light.status_cb.lightness_status.present_lightness=0;
    assert(g.node_light_event_(ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT,&light));assert(state.on==1);
    node.nightmatiq=true;assert(g.node_light_event_(0,&light));assert(state.on==1);
    node.nightmatiq=false;node.elements[0].capabilities=protocol::ONOFF;
    node.elements[1].capabilities=protocol::LIGHTNESS;params.ctx.addr=13;
    assert(g.node_light_event_(0,&light));assert(state.on==1);
    node.elements[0].capabilities|=protocol::LIGHTNESS;params.ctx.addr=12;
    state.composition_matches=false;assert(g.node_light_event_(0,&light));assert(state.on==1);
    state.composition_matches=true;node.selected=false;
    g.active_node_request_={0,0,LEVEL,0,0,0};g.active_node_request_.diagnostic=true;
    g.access_operation_=SteinelMesh::AccessOperation::NODE;
    g.node_destination_=12;g.access_opcode_=params.opcode=0x824b;g.node_expected_status_=params.ctx.recv_op;
    assert(g.node_light_event_(0,&light)&&g.completed&&g.last_success);assert(state.on==1);
    std::puts("Lightness: present values, separate outputs, twilight profile, invalid composition and unselected diagnostics passed");
  }
}
'''
compile_run('l840-callbacks', code)

probes = source[source.index('bool SteinelMesh::next_diagnostic_request_('):source.index('bool SteinelMesh::queue_node_command(')]
harness = polling['harness'].replace('  void poll_nodes_(uint32_t now);',
    '  bool next_diagnostic_request_(uint32_t,NodeRequest&);\n  void poll_nodes_(uint32_t now);')
code = harness + polling['queue']['REQUESTS'] + polling['queue']['COMPLETION']
code += probes + source[polling['start']:polling['end']]
code += r'''
}
int main() {
  using namespace esphome::steinel_mesh;
  assert(THRESHOLD_PROPERTY==0x2b && RUN_PROPERTY==0x3c);
  for(bool collecting:{false,true})for(bool offline:{false,true}) {
    SteinelMesh g;g.catalog_.count=3;test_now=1000;
    uint32_t previous[3]{1000,1000,1000},max_gap=0;unsigned diagnostics=0;
    for(size_t i=0;i<3;++i) {
      auto &node=g.catalog_.nodes[i];node.address=i==0?0x18:i==1?0x0c:0x12;
      node.selected=true;node.company_id=0x0563;node.product_id=i==1?0x1eba:0x1e7e;
      node.element_count=i==1?2:4;
      node.elements[0].capabilities=protocol::ONOFF|protocol::LIGHTNESS|protocol::SCENE|protocol::SCHEDULER;
      node.elements[1].capabilities=protocol::ONOFF|protocol::LC;
      if(i!=1)node.elements[2].capabilities=node.elements[3].capabilities=protocol::SENSOR;
      auto &state=g.node_states_[i];state.composition_checked=state.composition_matches=true;
      state.composition_at=test_now+IDENTITY_INTERVAL;state.last_seen=test_now;
      state.supported=i==1?7:127;state.lux_element=3;state.motion_element=2;
      for(size_t f=0;f<state.value_seen.size();++f)if(state.supported&(1U<<f))state.value_seen[f]=test_now;
    }
    bool started=false,complete=false;
    while(test_now<600000) {
      if(collecting&&!started&&test_now>=120000) {
        g.device_diagnostics_.start(test_now,1,0,0x18,4);started=true;
      }
      g.sent=false;SteinelMesh::NodeRequest request{};
      if(g.next_diagnostic_request_(test_now,request))g.send_node_request_(request);
      else g.poll_nodes_(test_now);
      if(!g.sent){test_now+=20;continue;}
      auto r=g.captured;
      const bool ok=!(offline&&r.node==2)&&r.kind!=THRESHOLD&&r.kind!=DESCRIPTORS&&
          !(r.kind==SENSOR_PROPERTY&&r.value!=0x004e);
      test_now+=ok?120:3000;
      if(r.diagnostic)++diagnostics;
      if(ok) {
        auto &state=g.node_states_[r.node];state.last_seen=test_now;
        if(r.kind>=ON&&r.kind<=RUN)state.value_seen[r.kind-ON]=test_now;
        if(r.kind==LEVEL)state.value_seen[0]=test_now;
        if(r.kind==ON||r.kind==LEVEL) {
          max_gap=std::max(max_gap,test_now-previous[r.node]);previous[r.node]=test_now;
        }
        if(r.kind==SENSOR||r.kind==SENSOR_PROPERTY)state.value_seen[r.element==2?6:5]=test_now;
      }
      g.active_node_request_=r;g.node_completion_pending_=true;g.node_completion_success_=ok;
      g.test_completion(test_now);test_now+=200;
      complete|=g.device_diagnostics_.probe_complete;
    }
    std::printf("L 840 polling: diagnostics=%d offline=%d max_output_gap=%.2fs probes=%u complete=%d\n",
        collecting,offline,max_gap/1000.0,diagnostics,complete);
    assert(max_gap<30000);
    if(collecting)assert(diagnostics>0&&complete);
  }
  {
    SteinelMesh idle;idle.catalog_.count=1;test_now=20000;
    auto &node=idle.catalog_.nodes[0];node.address=1;node.element_count=1;node.selected=true;
    node.elements[0].capabilities=protocol::ONOFF;
    auto &state=idle.node_states_[0];state.last_seen=state.value_seen[0]=1000;
    state.supported=protocol::F_OUTPUT;state.discovery_complete=true;state.poll_at=60000;
    idle.node_priority_burst_=8;idle.poll_nodes_(test_now);assert(!idle.sent);
    test_now+=20;idle.poll_nodes_(test_now);
    assert(idle.sent&&idle.captured.kind==ON&&state.value_poll_at[0]==test_now);
    std::puts("Output polling: an idle background cycle cannot hold the priority reservation until its next pass");
  }
  {
    SteinelMesh g;g.catalog_.count=2;test_now=200000;
    for(auto &node:g.catalog_.nodes){node.address=10;node.element_count=2;node.selected=true;
      node.elements[0].capabilities=protocol::ONOFF;node.elements[1].capabilities=protocol::LC;}
    auto &state=g.node_states_[0];state.value_seen[3]=test_now-130000;
    g.device_diagnostics_.start(test_now,1,1,10,2);
    SteinelMesh::NodeRequest request{};
    assert(!g.next_diagnostic_request_(test_now,request));
    state.value_poll_at[3]=test_now-200; // A failed regular refresh must still allow diagnostic progress.
    assert(g.next_diagnostic_request_(test_now,request)&&request.kind==COMPOSITION);
    std::puts("Diagnostic fairness: an old optional reading cannot block probes after its refresh was attempted");
  }
}
'''
compile_run('l840-polling', code)
