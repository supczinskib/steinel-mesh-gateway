#include "l840_lamp_model.h"
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <sstream>

namespace installation {
using namespace esphome::steinel_mesh;
using l840_device::Message;
using l840_device::Lamp;
constexpr uint32_t BASE=200000,INPUT_AT=10000,UNSEEN=std::numeric_limits<uint32_t>::max();

void device_contracts() {
  auto fresh=[] { return Lamp(0x18,true,fixture_nodes[0].composition); };
  const auto set=[](unsigned value,unsigned tid) { return Message{0x100,0x18,0x8202,{uint8_t(value),uint8_t(tid)}}; };
  {
    auto a=fresh(),b=fresh();a.occupancy(1000);a.advance(1000);b.advance(1000);
    assert(a.on()&&!b.on());a.advance(120999);assert(a.on());a.advance(121000);assert(!a.on());
    a.occupancy(130000);a.advance(130000);a.occupancy(140000);a.advance(259999);assert(a.on());a.advance(260000);assert(!a.on());
  }
  {
    auto a=fresh();a.set_mode(false,1000);a.receive(set(1,1),1100);assert(a.on());
    a.advance(200000);assert(a.on());a.occupancy(210000);assert(a.lc==Lamp::LC::OFF);
    a.receive(set(0,2),211000);a.set_mode(true,212000);assert(!a.on());
    a.advance(300000);assert(!a.on()); // Mode On alone is not an On command.
  }
  {
    auto a=fresh();a.settings.fade_on_ms=1000;a.settings.fade_ms=1000;a.settings.prolong_level=1000;
    a.settings.prolong_ms=2000;a.settings.standby_auto_ms=1000;a.occupancy(1000);a.advance(1500);
    assert(a.level>0&&a.level<65535);a.advance(2000);assert(a.lc==Lamp::LC::RUN&&a.level==65535);
    a.advance(122500);assert(a.lc==Lamp::LC::FADE&&a.level>1000);a.advance(124000);assert(a.lc==Lamp::LC::PROLONG);
    a.advance(125500);assert(a.on());a.advance(126000);assert(!a.on());
  }
  {
    auto a=fresh();a.set_mode(false,1000);a.settings.default_transition_ms=1000;
    a.receive(set(1,1),1100);a.advance(1600);assert(a.level>0&&a.level<65535);
    const auto applied=a.applied_sets;a.receive(set(1,1),1700);assert(a.applied_sets==applied);
    a.advance(2100);assert(a.level==65535);a.receive(set(0,2),2200);
    auto status=a.output_status(2500);assert(status.size()==3&&status[0]==1&&status[1]==0&&status[2]>0);
    a.advance(3200);assert(!a.on());a.receive(set(1,1),8000);assert(a.applied_sets==applied+2);
    // New source and opcode family cannot be swallowed by another transaction.
    a.receive({0x101,0x18,0x8202,{0,1}},8100);assert(a.applied_sets==applied+3);
    a.receive({0x101,0x18,0x824c,{0xff,0xff,1}},8200);assert(a.applied_sets==applied+4);
  }
  {
    auto a=fresh();a.set_mode(false,1000);
    a.receive({0x100,0x18,0x8202,{1,1,0,100}},1100);a.advance(1599);assert(!a.on());a.advance(1600);assert(a.on());
    a.receive({0x100,0x18,0x8202,{0,2,0,0}},1700);assert(!a.on());
  }
  {
    auto a=fresh();a.receive({0x100,0xc002,0x52,{0x40,8,200}},1000);assert(!a.on());
    a.receive({0x100,0xc001,0x52,{0x40,8,200}},1100);a.advance(1100);assert(a.on());
    a.set_mode(false,1200);assert(a.on()); // LC disabled != physical output off.
    const auto prior=a.applied_sets;assert(!a.receive({0x100,0x18,0x8202,{2,7}},1300));assert(a.applied_sets==prior);
    assert(!a.receive({0x100,0x1a,0x8201,{}},1300)); // No output server on sensor element.
  }
  {
    auto a=fresh();a.receive(set(1,1),1000);assert(a.on()&&!a.mode);
    a.advance(200000);assert(a.on()); // Primary manual output releases LC binding.
    a.receive({0x100,0x19,0x8202,{0,2}},201000);assert(a.on());
    a.receive({0x100,0x19,0x8292,{1}},202000);assert(a.mode&&!a.on());
    a.receive({0x100,0x19,0x8202,{1,3,0,100}},203000);
    a.advance(203499);assert(!a.on());a.advance(203500);assert(a.on());
  }
  {
    auto a=fresh();a.receive({0x100,0x18,0x8202,{1,1,0,100}},1000);
    assert(a.mode&&!a.on());a.advance(1499);assert(a.mode&&!a.on());
    a.advance(1500);assert(a.on()&&!a.mode);
  }
  {
    auto a=fresh();a.settings.occupancy_delay_ms=1000;
    a.occupancy(1000);a.occupancy(1500);a.advance(2000);assert(a.on());
    a.advance(122000);assert(a.on());a.advance(122500);assert(!a.on());
    a.set_mode(false,130000);a.occupancy(131000);a.set_mode(true,131500);
    a.advance(132000);assert(a.on()); // Delayed input is evaluated when due, not when received.
    a.settings.occupancy_mode=false;a.occupancy(133000);a.advance(253999);assert(a.on());
    a.advance(254000);assert(!a.on());a.occupancy(255000);a.advance(256000);assert(!a.on());
  }
  std::puts("Independent lamp contracts: timers, fades, LC/manual separation, TID, delay, groups and capability routing PASS");
}

enum class Input { MOTION,MANUAL_ON,POSTED_OFF_AUTO,POSTED_WITH_MOTION,RESTORE_AUTO,CAPTURED_SENSOR,SCRIPTED };
const char *input_name(Input input) {
  switch(input) {
    case Input::MOTION:return "motion";case Input::MANUAL_ON:return "manual_on";
    case Input::POSTED_OFF_AUTO:return "posted_off_auto";case Input::POSTED_WITH_MOTION:return "posted_with_motion";
    case Input::RESTORE_AUTO:return "restore_auto";
    case Input::CAPTURED_SENSOR:return "captured_sensor";
    case Input::SCRIPTED:return "scripted";
  }return "invalid";
}
struct Case {
  Input input;unsigned loss,seed,fade;int diagnostic;bool reverse=false;
  // Losses are per direction. A lost ACK does not undo a delivered write.
  bool lose_downlink=false;
  bool lc_group=false;
  uint32_t fade_off=UNSEEN,occupancy_delay=0;
};
bool loss_at(unsigned seed,unsigned node,unsigned direction,uint32_t opcode,unsigned ordinal,unsigned percent) {
  uint32_t value=seed^(node+1)*0x9e3779b9U^(direction+1)*0x85ebca6bU^opcode*0xc2b2ae35U^ordinal*0x27d4eb2dU;
  value^=value>>16;value*=0x7feb352dU;value^=value>>15;value*=0x846ca68bU;value^=value>>16;
  return value%100<percent;
}
struct Timed {
  uint32_t at;uint64_t order;std::function<void()> action;
  bool operator<(const Timed &other) const { return std::tie(at,order)>std::tie(other.at,other.order); }
};
class Network {
 public:
  SteinelMesh gateway;NativeEntities entities;
  std::vector<Lamp> lamps;
  std::array<SteinelMesh::TestEntity,3> actual{},automatic{},brightness{};
  std::priority_queue<Timed> events;
  uint64_t order=0;unsigned attempts=0;
  std::map<std::tuple<unsigned,unsigned,uint32_t>,unsigned> loss_ordinals;
  std::array<uint8_t,3> motion_tid{};
  std::array<uint32_t,3> physical_on{},physical_off{},confirmed_on{},confirmed_off{},ha_on{},first_send{};
  std::array<bool,3> previous_on{};std::array<std::string,3> previous_ha{};Case scenario;
  std::ostringstream trace;
  unsigned publications_filtered=0,rx_dropped=0,tx_dropped=0;
#ifdef L840_REAL_SDK
  idf_host::Bridge sdk;
  uint32_t adv_free=BASE;
#endif
  std::vector<std::pair<uint32_t,std::array<uint16_t,3>>> physical_history;
  explicit Network(Case c):scenario(c) {
    test_now=BASE;wire_sends=0;initialize_network(gateway,entities,0);
    physical_on.fill(UNSEEN);physical_off.fill(UNSEEN);confirmed_on.fill(UNSEEN);confirmed_off.fill(UNSEEN);
    ha_on.fill(UNSEEN);first_send.fill(UNSEEN);
    for(unsigned i=0;i<3;++i) {
      lamps.emplace_back(fixture_nodes[i].address,i!=1,fixture_nodes[i].composition);
      const auto fade_off=c.fade_off==UNSEEN?c.fade:c.fade_off;
      lamps.back().settings.fade_on_ms=c.fade;lamps.back().settings.fade_ms=fade_off;lamps.back().settings.standby_auto_ms=fade_off;
      lamps.back().settings.occupancy_delay_ms=c.occupancy_delay;
      lamps.back().settings.default_transition_ms=c.fade;
      entities.nodes[i].actual_output=&actual[i];entities.nodes[i].automatic=&automatic[i];entities.nodes[i].level=&brightness[i];
      // No fabricated old confirmed output: initialize it to the independent device state.
      gateway.node_states_[i].brightness=lamps[i].level;
    }
    if(c.input==Input::RESTORE_AUTO)for(unsigned i=0;i<3;++i) {
      lamps[i].set_mode(false,BASE);lamps[i].receive({0x200,lamps[i].address,0x8202,{1,uint8_t(i)}},BASE);
      gateway.node_states_[i].on=1;gateway.node_states_[i].automatic=0;gateway.node_states_[i].brightness=65535;
    }
    if(c.diagnostic>=0)gateway.device_diagnostics_.start(BASE,1,c.diagnostic,lamps[c.diagnostic].address,lamps[c.diagnostic].models.size());
    trace<<"elapsed_ms,event,node,opcode,value,physical_on,gateway_on,ha_output,ha_actual\n";
  }
  bool drop(unsigned node,unsigned direction,uint32_t opcode) {
    const unsigned ordinal=++loss_ordinals[{node,direction,opcode}];
    return loss_at(scenario.seed,node,direction,opcode,ordinal,scenario.loss);
  }
  void schedule(uint32_t at,std::function<void()> action) { events.push({at,order++,std::move(action)}); }
  void log(const char *event,unsigned i,uint32_t op=0,int value=-1) {
    trace<<test_now-BASE<<','<<event<<",0x"<<std::hex<<lamps[i].address<<",0x"<<op<<std::dec<<','<<value<<','
         <<lamps[i].on()<<','<<int(gateway.node_states_[i].on)<<','<<entities.output[i].option<<',';
    if(actual[i].known)trace<<actual[i].boolean;else trace<<"unknown";trace<<'\n';
  }
  void motion(unsigned source) {
    log("motion_input",source);
    lamps[source].occupancy(test_now);
    const Message occupancy=scenario.lc_group?Message{lamps[source].address,0xc001,0x829b,{1,++motion_tid[source]}}:
        Message{uint16_t(lamps[source].address+2),0xc001,0x52,{0x40,8,200}};
    for(unsigned i=0;i<3;++i)if(i!=source)schedule(test_now+40,[this,i,occupancy] {
      lamps[i].receive(occupancy,test_now);log("group_occupancy",i,occupancy.opcode);
    });
    if(!scenario.lc_group)schedule(test_now+40,[this,source,occupancy] {
      esp_ble_mesh_client_common_param_t publication{};publication.ctx.addr=occupancy.source;publication.ctx.recv_op=0x52;publication.opcode=0x52;
      deliver(gateway,publication,true,occupancy.bytes,0,true);log("sensor_group_rx",source,0x52);
    });
  }
  void command(unsigned i,SteinelMesh::NodeCommand command,unsigned value) {
    assert(gateway.queue_node_command(lamps[i].address,command,value));
    log(command==SteinelMesh::NodeCommand::AUTO?"ha_auto_input":"ha_output_input",i,0,value);
  }
  void inputs() {
    if(scenario.input==Input::MOTION) { motion(0);return; }
    const std::array<unsigned,3> normal{0,1,2},reverse{2,1,0};
    const auto &output_order=scenario.reverse?reverse:normal;
    for(auto i:output_order)command(i,SteinelMesh::NodeCommand::ONOFF,scenario.input==Input::MANUAL_ON);
    if(scenario.input==Input::POSTED_OFF_AUTO||scenario.input==Input::POSTED_WITH_MOTION||scenario.input==Input::RESTORE_AUTO) {
      const std::array<unsigned,3> normal_auto{1,0,2},reverse_auto{1,2,0};
      for(unsigned i:scenario.reverse?reverse_auto:normal_auto)command(i,SteinelMesh::NodeCommand::AUTO,1);
    }
    // Separate declared hypothesis, not silently inferred from the automation.
    if(scenario.input==Input::POSTED_WITH_MOTION)schedule(test_now+2000,[this]{motion(0);});
  }
  void transmit() {
#ifdef L840_REAL_SDK
    transmit_sdk();return;
#endif
    const auto p=wire_request;const auto payload=wire_payload;
    const auto req=gateway.active_node_request_;const unsigned i=req.node;
    assert(i<3);++attempts;log("tx",i,p.opcode,payload.empty()?-1:payload[0]);
    if(is_write(req.kind)&&first_send[i]==UNSEEN)first_send[i]=test_now-BASE;
    const bool unack=p.opcode==0x8293||p.opcode==0x8203||p.opcode==0x824d||p.opcode==0x63;
    const auto token=attempts;
    auto answered=std::make_shared<bool>(false);
    if(!unack)schedule(test_now+p.msg_timeout,[this,p,payload,answered,i,token] {
      if(*answered)return;
      *answered=true;
      auto timeout=p;timeout.ctx.recv_op=expected_status(p.opcode);
      log("sdk_timeout",i,p.opcode,int(token));deliver(gateway,timeout,false,{},property_of(p.opcode,payload));
    });
    const bool lost=scenario.lose_downlink&&drop(i,0,p.opcode);
    if(lost) { ++tx_dropped;log("tx_lost",i,p.opcode);return; }
    // Nominal 100ms RTT, close/far hop difference explicit. Not a fitted command delay.
    const uint32_t hop=i==2?60:40;
    schedule(test_now+hop,[this,i,p,payload,answered,unack,hop] {
      auto response=lamps[i].receive({0x100,p.ctx.addr,p.opcode,payload},test_now);
      log("device_receive",i,p.opcode,payload.empty()?-1:payload[0]);
      if(!response||unack)return;
      if(drop(i,1,p.opcode)) { ++rx_dropped;log("reply_lost",i,response->opcode);return; }
      schedule(test_now+hop+20,[this,i,p,response,answered] {
        auto params=p;params.ctx.addr=response->source;params.ctx.recv_op=response->opcode;
        const bool late=*answered||gateway.access_operation_.load()!=SteinelMesh::AccessOperation::NODE||
            gateway.node_destination_.load()!=params.ctx.addr||gateway.access_opcode_.load()!=params.opcode;
        const auto property=response->opcode==0x64?l840_device::u16(response->bytes):0;
        auto bytes=response->bytes;if(property)bytes.erase(bytes.begin(),bytes.begin()+2);
        if(late)params.opcode=response->opcode;
        deliver(gateway,params,true,bytes,property,late);*answered=true;log("rx",i,response->opcode,bytes.empty()?-1:bytes[0]);
      });
    });
  }
#ifdef L840_REAL_SDK
  void transmit_sdk() {
    const auto p=wire_request;const auto payload=wire_payload;
    const unsigned i=gateway.active_node_request_.node;
    assert(i<3);++attempts;log("sdk_submit",i,p.opcode,payload.empty()?-1:payload[0]);
    if(is_write(gateway.active_node_request_.kind)&&first_send[i]==UNSEEN)first_send[i]=test_now-BASE;
    const bool unack=p.opcode==0x8293||p.opcode==0x8203||p.opcode==0x824d||p.opcode==0x63;
    QueuedPacket packet{};
    const int error=sdk.submit(test_now,p.ctx.addr,p.opcode,p.msg_timeout,payload.size(),!unack,packet);
    if(error) {
      log("sdk_rejected",i,p.opcode,error);
      auto failed=p;failed.ctx.recv_op=0;
      deliver(gateway,failed,true,{},property_of(p.opcode,payload),false,error);return;
    }
    const uint32_t start=std::max(test_now,adv_free);
    adv_free=start+bt_mesh_pdu_duration(bt_mesh_net_transmit_get());
    schedule(start,[this,packet,i,p,payload,unack] {
      sdk.start(test_now,packet);log("adv_start",i,p.opcode);
      if(scenario.lose_downlink&&drop(i,0,p.opcode)) { ++tx_dropped;log("tx_lost",i,p.opcode);return; }
      const uint32_t hop=i==2?60:40;
      schedule(test_now+hop,[this,i,p,payload,unack,hop] {
        auto response=lamps[i].receive({0x100,p.ctx.addr,p.opcode,payload},test_now);
        log("device_receive",i,p.opcode,payload.empty()?-1:payload[0]);
        if(!response||unack)return;
        if(drop(i,1,p.opcode)) { ++rx_dropped;log("reply_lost",i,response->opcode);return; }
        schedule(test_now+hop+20,[this,i,p,response] {
          auto params=p;params.ctx.addr=response->source;params.ctx.recv_op=response->opcode;
          params.opcode=sdk.receive(test_now,response->source,response->opcode);
          const bool publication=params.opcode==response->opcode;
          const auto property=response->opcode==0x64?l840_device::u16(response->bytes):0;
          auto bytes=response->bytes;if(property)bytes.erase(bytes.begin(),bytes.begin()+2);
          deliver(gateway,params,true,bytes,property,publication);log("rx",i,response->opcode,bytes.empty()?-1:bytes[0]);
        });
      });
    });
  }
  void sdk_tick() {
    sdk.tick(test_now,[this](uint16_t destination,uint32_t opcode) {
      unsigned i=0;while(i<3&&(destination<lamps[i].address||destination>=lamps[i].address+lamps[i].models.size()))++i;
      assert(i<3);log("sdk_timeout",i,opcode);
      esp_ble_mesh_client_common_param_t p{};p.ctx.addr=destination;p.opcode=opcode;
      deliver(gateway,p,false,{});
    });
  }
#endif
  void sample() {
    for(unsigned i=0;i<3;++i) {
      lamps[i].advance(test_now);
      if(lamps[i].on()!=previous_on[i]) {
        previous_on[i]=lamps[i].on();log("physical_transition",i,0,lamps[i].level);
        auto &first=lamps[i].on()?physical_on[i]:physical_off[i];if(first==UNSEEN)first=test_now-BASE;
        // The gateway subscribes Sensor groups, not output groups. Simulate access filtering.
        ++publications_filtered;
        log("output_group_not_subscribed",i,0x8204,lamps[i].on());
      }
      const auto &state=gateway.node_states_[i];
      if(entities.output[i].option!=previous_ha[i]) { previous_ha[i]=entities.output[i].option;log("ha_output_change",i); }
      if(physical_on[i]!=UNSEEN&&state.on==1&&state.value_seen[0]>=BASE+physical_on[i]&&confirmed_on[i]==UNSEEN)
        confirmed_on[i]=test_now-BASE;
      if(physical_off[i]!=UNSEEN&&state.on==0&&state.value_seen[0]>=BASE+physical_off[i]&&confirmed_off[i]==UNSEEN)
        confirmed_off[i]=test_now-BASE;
      if(entities.output[i].known&&entities.output[i].option=="On"&&ha_on[i]==UNSEEN)ha_on[i]=test_now-BASE;
    }
    if(scenario.input==Input::CAPTURED_SENSOR||scenario.input==Input::SCRIPTED) {
      const std::array<uint16_t,3> levels{lamps[0].level,lamps[1].level,lamps[2].level};
      if(physical_history.empty()||physical_history.back().second!=levels)physical_history.push_back({test_now-BASE,levels});
    }
  }
  void run(uint32_t duration=180000) {
    if(scenario.input==Input::CAPTURED_SENSOR)for(auto stimulus:captured_motion)schedule(BASE+stimulus.at,[this,stimulus] {
      if(stimulus.value)motion(0);
      else {
        esp_ble_mesh_client_common_param_t p{};p.ctx.addr=lamps[0].address+2;p.ctx.recv_op=0x52;p.opcode=0x52;
        deliver(gateway,p,true,{0x40,8,0},0,true);log("captured_motion_zero",0,0x52);
      }
    });
    for(test_now=BASE;test_now<=BASE+duration;test_now+=10) {
      if(test_now==BASE+INPUT_AT&&scenario.input!=Input::CAPTURED_SENSOR&&scenario.input!=Input::SCRIPTED)inputs();
      while(!events.empty()&&events.top().at<=test_now) { auto event=events.top();events.pop();event.action(); }
#ifdef L840_REAL_SDK
      sdk_tick();
#endif
      // Physical state advances before a GET is processed and never comes from gateway state.
      for(auto &lamp:lamps)lamp.advance(test_now);
      gateway.expire_access_operation_(test_now);const unsigned before=wire_sends;gateway.advance_nodes_(test_now);
      if(wire_sends!=before) { assert(wire_sends==before+1);transmit(); }
      sample();
    }
  }
  static uint32_t expected_status(uint32_t op) {
    switch(op) {
      case 0x8008:return 0x02;case 0x8201:case 0x8202:return 0x8204;
      case 0x824b:case 0x824c:return 0x824e;case 0x8291:case 0x8292:return 0x8294;
      case 0x829d:case 0x62:return 0x64;case 0x8230:return 0x51;case 0x8231:return 0x52;
      default:assert(false);return 0;
    }
  }
  static uint16_t property_of(uint32_t op,const std::vector<uint8_t> &payload) {
    return (op==0x829d||op==0x62)&&payload.size()>=2?l840_device::u16(payload):0;
  }
};

std::string time_cell(uint32_t time) { return time==UNSEEN?"not_observed":std::to_string(time); }
void transport_contracts() {
  for(unsigned percent:{25U,50U}) {
    unsigned lost=0,pairs=0;
    for(unsigned i=1;i<=10000;++i) {
      const bool a=loss_at(1,0,0,0x8202,i*2,percent),b=loss_at(1,0,0,0x8202,i*2+1,percent);
      lost+=a;pairs+=a&&b;
    }
    assert(std::abs(double(lost)/10000-percent/100.0)<0.02);
    assert(std::abs(double(pairs)/10000-std::pow(percent/100.0,2))<0.02);
  }
  Network no_delivery({Input::MANUAL_ON,100,1,0,-1,false,true});no_delivery.run();
  for(unsigned i=0;i<3;++i) {
    assert(no_delivery.physical_on[i]==UNSEEN&&no_delivery.ha_on[i]==UNSEEN);
  }
  Network no_replies({Input::MANUAL_ON,100,1,0,-1,false,false});no_replies.run();
  for(unsigned i=0;i<3;++i) {
    assert(no_replies.physical_on[i]!=UNSEEN&&no_replies.confirmed_on[i]==UNSEEN&&no_replies.ha_on[i]==UNSEEN);
  }
  std::puts("Transport controls: loss profile sanity; HA output requires a device reply; lost ACK != unapplied write PASS");
}
}  // namespace installation

int main(int argc,char **argv) {
  using namespace installation;
  assert(argc==2);device_contracts();transport_contracts();
  const std::string directory=argv[1];std::ofstream results(directory+"/scenarios.csv");
  results<<"input,loss_percent,loss_direction,seed,fade_ms,diagnostic,reverse,group_mechanism,node,first_write_ms,physical_on_ms,confirmed_on_ms,ha_on_ms,physical_off_ms,confirmed_off_ms,tx_dropped,rx_dropped\n";
  unsigned cases=0,slow_commands=0,absent_commands=0;
  std::vector<unsigned> seeds{1,8,29,102},fades{0,1000,10000};std::vector<int> diagnostics{-1,0,1,2};
#ifdef L840_QUICK
  seeds={1};fades={0};diagnostics={-1};
#endif
#ifdef L840_CAPTURE_ONLY
  seeds={};
#endif
  for(Input input:{Input::MOTION,Input::MANUAL_ON,Input::POSTED_OFF_AUTO,Input::POSTED_WITH_MOTION,Input::RESTORE_AUTO})
    for(unsigned loss:{0U,25U,50U})for(unsigned seed:seeds)
      for(unsigned fade:fades)for(int diagnostic:diagnostics)for(bool downlink:{false,true})for(bool lc_group:{false,true}) {
        // A zero-loss run has no directional ambiguity.
        if(!loss&&downlink)continue;
        Network n({input,loss,seed,fade,diagnostic,seed%2==0,downlink,lc_group});n.run();++cases;
        bool slow=false,absent=false;
        for(unsigned i=0;i<3;++i) {
          if(input==Input::MANUAL_ON) {
            slow|=n.physical_on[i]!=UNSEEN&&n.physical_on[i]-INPUT_AT>=10000;
            absent|=n.physical_on[i]==UNSEEN;
          }
          if(!loss&&input==Input::MOTION) {
            assert(n.physical_on[i]!=UNSEEN&&n.physical_off[i]!=UNSEEN);
            assert(n.confirmed_on[i]!=UNSEEN&&n.confirmed_off[i]!=UNSEEN);
          }
          if(!loss&&input==Input::MANUAL_ON)assert(n.physical_on[i]!=UNSEEN&&n.confirmed_on[i]!=UNSEEN);
          if(input==Input::POSTED_OFF_AUTO)assert(n.physical_on[i]==UNSEEN);
          results<<input_name(input)<<','<<loss<<','<<(downlink?"both":"reply")<<','<<seed<<','<<fade<<','<<diagnostic<<','<<n.scenario.reverse<<','<<(lc_group?"lc_onoff":"occupancy")
                 <<",0x"<<std::hex<<n.lamps[i].address<<std::dec;
          for(auto time:{n.first_send[i],n.physical_on[i],n.confirmed_on[i],n.ha_on[i],n.physical_off[i],n.confirmed_off[i]})results<<','<<time_cell(time);
          results<<','<<n.tx_dropped<<','<<n.rx_dropped<<'\n';
        }
        if(slow)++slow_commands;if(absent)++absent_commands;
        if((seed==1&&diagnostic==-1&&!loss&&fade==0)||(input==Input::MANUAL_ON&&slow&&seed==1&&diagnostic==-1)) {
          std::ostringstream path;path<<directory<<'/'<<input_name(input)<<"-loss"<<loss<<"-both"<<downlink<<"-fade"<<fade<<"-lcgroup"<<lc_group<<".csv";
          std::ofstream(path.str())<<n.trace.str();
        }
      }
  std::printf("Installation model: %u scenarios; %u manual-command scenarios exceed 10s; %u miss at least one lamp\n",cases,slow_commands,absent_commands);
  std::ofstream capture_results(directory+"/captured-input.csv");
  capture_results<<"fade_on_ms,fade_off_ms,occupancy_delay_ms,group_mechanism,node,physical_on_ms,physical_off_ms,confirmed_on_ms,confirmed_off_ms,trajectory_file\n";
  for(unsigned fade_on:{0U,1000U,10000U})for(unsigned fade_off:{0U,1000U,10000U})
    for(unsigned occupancy_delay:{0U,1000U,10000U})for(bool lc_group:{false,true}) {
    Network capture({Input::CAPTURED_SENSOR,0,1,fade_on,0,false,false,lc_group,fade_off,occupancy_delay});capture.run();
    const std::string identity="captured-on"+std::to_string(fade_on)+"-off"+std::to_string(fade_off)+
        "-occupancy"+std::to_string(occupancy_delay)+"-lcgroup"+std::to_string(lc_group);
    const auto trajectory_file=identity+"-physical.csv";
    std::ofstream trajectory(directory+"/"+trajectory_file);
    trajectory<<"elapsed_ms,node,lightness\n";
    for(const auto &snapshot:capture.physical_history)for(unsigned i=0;i<3;++i)
      trajectory<<snapshot.first<<",0x"<<std::hex<<capture.lamps[i].address<<std::dec<<','<<snapshot.second[i]<<'\n';
    for(unsigned i=0;i<3;++i) {
      capture_results<<fade_on<<','<<fade_off<<','<<occupancy_delay<<','<<(lc_group?"lc_onoff":"occupancy")<<",0x"<<std::hex<<capture.lamps[i].address<<std::dec;
      for(auto time:{capture.physical_on[i],capture.physical_off[i],capture.confirmed_on[i],capture.confirmed_off[i]})capture_results<<','<<time_cell(time);
      capture_results<<','<<trajectory_file<<'\n';
    }
    std::ofstream(directory+"/"+identity+"-events.csv")<<capture.trace.str();
  }
  std::puts("Model assertions passed. This is NOT a claim of rc.3 correctness or full convergence with the physical installation.");
}
