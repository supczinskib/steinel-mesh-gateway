"""Check production trace against known scheduler faults, not fitted installation timings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from test_l840_installation import ROOT, build_code

MAIN = r'''
#include <set>
int main() {
  using namespace installation;
  using Trace=protocol::StateTrace;
  for(unsigned loss:{0U,100U})for(unsigned gap:{0U,5000U}) {
    std::string reference;
    for(bool enabled:{false,true}) {
      Network n({Input::SCRIPTED,loss,1,0,-1});
      std::vector<Trace::Entry> collected;
      uint32_t last=0;
      if(enabled)n.gateway.state_trace_.start(BASE,1);
      const auto drain=[&] {
        const auto &trace=n.gateway.state_trace_;
        for(unsigned i=0;i<trace.count;i++)if(trace.entry(i).sequence>last) {
          assert(trace.entry(i).sequence==last+1);last=trace.entry(i).sequence;
          collected.push_back(trace.entry(i));
        }
      };
      for(unsigned at=0;at<=60000;at+=500)n.schedule(BASE+at,drain);
      n.schedule(BASE+INPUT_AT,[&] {
        n.gateway.node_next_request_at_=test_now+gap;
        for(unsigned i:{0U,1U,2U})n.command(i,SteinelMesh::NodeCommand::ONOFF,1);
      });
      n.run(60000);drain();
      if(!enabled){reference=n.trace.str();continue;}
      assert(reference==n.trace.str());
      std::set<uint16_t> received,queued,sent;
      unsigned timeouts=0,retries=0,completions=0,accepted=0,waits=0;bool known_gap=false;
      for(const auto &event:collected) {
        if(event.event==Trace::RECEIVED)received.insert(event.node);
        if(event.event==Trace::QUEUED)queued.insert(event.node);
        if(event.event==Trace::SEND&&event.kind==7&&event.step==1)sent.insert(event.node);
        if(event.event==Trace::TIMEOUT)++timeouts;
        if(event.event==Trace::SEND&&event.attempt==1)++retries;
        if(event.event==Trace::COMPLETED)++completions;
        if(event.event==Trace::SDK_RESULT&&event.error==0)++accepted;
        if(event.event==Trace::WAIT){++waits;if((event.flags&Trace::REQUEST_GAP)&&event.detail==5000)known_gap=true;}
      }
      assert((received==std::set<uint16_t>{0xc,0x12,0x18})&&received==queued&&queued==sent);
      assert(completions&&accepted&&waits);
      if(gap)assert(known_gap);
      if(loss)assert(timeouts&&retries);
      n.gateway.state_trace_.stop(test_now);
      std::printf("Trace model: loss=%u%% declared_gate=%ums events=%zu timeouts=%u retries=%u buffer=%zuB, command timings unchanged\n",
          loss,gap,collected.size(),timeouts,retries,sizeof(Trace));
    }
  }
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',action='store_true',help='Use the installed ESP-IDF client implementation')
    args=parser.parse_args()
    source=build_code().rsplit('\nint main(int argc,char **argv) {',1)[0] + MAIN
    if args.sdk:
        from test_l840_sdk_timing import DEFAULT_IDF, function
        sdk=DEFAULT_IDF/'components/bt/esp_ble_mesh'
        client=(sdk/'models/client/client_common.c').read_text()
        adv=(sdk/'core/adv_common.c').read_text()
        names=['client_pick_node_unsafe','client_pick_node','bt_mesh_is_client_recv_publish_msg',
               'client_get_status_op','client_get_adv_duration','client_calc_timeout','msg_send_start']
        extracted=''.join(function(client,name) for name in names)
        extracted+='static const struct bt_mesh_send_cb send_cb = {msg_send_start, nullptr};\n'
        extracted+=function(client,'bt_mesh_client_send_msg')+function(client,'bt_mesh_client_free_node')
        extracted+=function(adv,'bt_mesh_pdu_duration')
        extracted=extracted.replace('bt_mesh_client_node_t *node = cb_data;',
                                  'bt_mesh_client_node_t *node = static_cast<bt_mesh_client_node_t *>(cb_data);')
        boundary=(ROOT/'tests/l840_sdk_contract.h').read_text().replace('// IDF_CLIENT_FUNCTIONS',extracted)
        boundary=boundary[:boundary.index('\nint main() {')]+(ROOT/'tests/l840_sdk_bridge.h').read_text()
        source=source.replace('#include "l840_lamp_model.h"',boundary+'\n#include "l840_lamp_model.h"')
    output=ROOT/'output/validation/command-trace'
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='steinel-command-trace-') as directory:
        binary=str(Path(directory)/'trace')
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-Wno-unused-const-variable','-fsanitize=address,undefined',
                        *(['-DL840_REAL_SDK=1'] if args.sdk else []),
                        '-I',str(ROOT/'tests'),'-I',str(ROOT/'esphome/components/steinel_mesh'),
                        '-x','c++','-','-o',binary],input=source,text=True,check=True)
        result=subprocess.run([binary],capture_output=True,text=True)
    print(result.stdout,end='')
    if result.stderr:print(result.stderr,end='')
    name='sdk' if args.sdk else 'model'
    (output/f'{name}.log').write_text(result.stdout+result.stderr)
    (output/f'{name}.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
        'passed':result.returncode==0,'scope':'Diagnostic observability of declared faults; not reproduction of the remaining installation delay.'},indent=2)+'\n')
    raise SystemExit(result.returncode)

if __name__=='__main__':
    main()
