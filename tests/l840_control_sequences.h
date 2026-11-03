namespace control_sequences {
using namespace installation;
using Command=SteinelMesh::NodeCommand;

void output(Network &n,bool value) {
  const std::array<unsigned,3> normal{0,1,2},reverse{2,1,0};
  for(auto i:n.scenario.reverse?reverse:normal)n.command(i,Command::ONOFF,value);
}
void automatic(Network &n,bool value) {
  const std::array<unsigned,3> normal{1,0,2},reverse{1,2,0};
  for(auto i:n.scenario.reverse?reverse:normal)n.command(i,Command::AUTO,value);
}
void check(Network &n,bool physical,bool auto_mode) {
  for(unsigned i=0;i<3;++i) {
    n.lamps[i].advance(test_now);
    if(n.lamps[i].on()!=physical||n.lamps[i].mode!=auto_mode||
       n.gateway.node_states_[i].on!=int(physical)||n.gateway.node_states_[i].automatic!=int(auto_mode)||
       !n.entities.output[i].known||n.entities.output[i].option!=(physical?"On":"Off")||
       !n.automatic[i].known||n.automatic[i].option!=(auto_mode?"On":"Off")||
       !n.actual[i].known||n.actual[i].boolean!=physical) {
      std::fprintf(stderr,"Control contract failed: node=0x%x at=%u fade=%u diagnostic=%d group=%d reverse=%d expected_on=%d expected_auto=%d\n%s",
          n.lamps[i].address,test_now-BASE,n.scenario.fade,n.scenario.diagnostic,n.scenario.lc_group,n.scenario.reverse,
          physical,auto_mode,n.trace.str().c_str());
    }
    assert(n.lamps[i].on()==physical&&n.lamps[i].mode==auto_mode);
    assert(n.gateway.node_states_[i].on==int(physical));
    assert(n.gateway.node_states_[i].automatic==int(auto_mode));
    assert(n.entities.output[i].known&&n.entities.output[i].option==(physical?"On":"Off"));
    assert(n.automatic[i].known&&n.automatic[i].option==(auto_mode?"On":"Off"));
    assert(n.actual[i].known&&n.actual[i].boolean==physical);
  }
}

void round_trip(Case c) {
  Network n(c);
  n.schedule(BASE+10000,[&]{output(n,true);});
  n.schedule(BASE+20000,[&]{n.motion(0);});
  n.schedule(BASE+30000,[&]{check(n,true,false);});
  n.schedule(BASE+40000,[&]{output(n,false);});
  n.schedule(BASE+50000,[&]{n.motion(2);});
  n.schedule(BASE+60000,[&]{check(n,false,false);});
  n.schedule(BASE+70000,[&]{automatic(n,true);});
  n.schedule(BASE+80000,[&]{check(n,false,true);});
  n.schedule(BASE+90000,[&]{n.motion(0);});
  n.schedule(BASE+110000,[&]{check(n,true,true);});
  n.schedule(BASE+250000,[&]{check(n,false,true);});
  n.run(260000);
}

void no_motion(Case c) {
  Network n(c);
  n.schedule(BASE+10000,[&]{output(n,false);automatic(n,true);});
  n.schedule(BASE+30000,[&]{check(n,false,true);});
  n.run(40000);
  for(auto on:n.physical_on)assert(on==UNSEEN);
}

void replace_commands(Case c) {
  Network n(c);
  n.schedule(BASE+10000,[&]{output(n,true);});
  n.schedule(BASE+10100,[&]{output(n,false);});
  n.schedule(BASE+10200,[&]{output(n,true);});
  n.schedule(BASE+30000,[&]{check(n,true,false);});
  n.schedule(BASE+40000,[&]{automatic(n,false);automatic(n,true);});
  n.schedule(BASE+60000,[&]{check(n,false,true);});
  n.schedule(BASE+70000,[&]{n.motion(2);});
  n.schedule(BASE+90000,[&]{check(n,true,true);});
  n.run(100000);
}

void repeated_motion(Case c) {
  Network n(c);
  n.schedule(BASE+10000,[&]{n.motion(0);});
  n.schedule(BASE+14000,[&]{n.motion(0);});
  n.schedule(BASE+132000,[&]{check(n,true,true);});
  n.schedule(BASE+150000,[&]{check(n,false,true);});
  n.run(160000);
}

void timing(Case c,unsigned source,unsigned motion_at,std::ofstream &results,const std::string &directory) {
  Network n(c);
  n.schedule(BASE+10000,[&]{output(n,false);automatic(n,true);});
  n.schedule(BASE+motion_at,[&]{n.motion(source);});
  n.run(90000);
  for(unsigned i=0;i<3;++i) {
    // A missed write may leave LC off. Record physical, confirmed and optimistic states separately.
    if(c.loss==0)assert(n.lamps[i].mode&&n.gateway.node_states_[i].automatic==1);
    results<<c.diagnostic<<','<<(c.lc_group?"lc_onoff":"occupancy")<<','<<c.reverse<<','<<c.fade<<','
           <<c.loss<<','<<c.lose_downlink<<','<<c.seed<<",0x"<<std::hex<<n.lamps[source].address<<std::dec<<','<<motion_at
           <<",0x"<<std::hex<<n.lamps[i].address<<std::dec<<','<<n.lamps[i].mode<<','<<n.lamps[i].on()<<','
           <<int(n.gateway.node_states_[i].on)<<','<<n.entities.output[i].option<<','<<n.entities.output[i].known<<','
           <<time_cell(n.physical_on[i])<<','<<time_cell(n.confirmed_on[i])<<','<<n.tx_dropped<<','<<n.rx_dropped<<'\n';
  }
  if(c.diagnostic==-1&&!c.lc_group&&!c.reverse&&!c.fade&&!c.loss) {
    std::ofstream trace(directory+"/motion-at"+std::to_string(motion_at)+"-source"+std::to_string(source)+".csv");
    trace<<n.trace.str();
  }
}
#ifdef L840_REAL_SDK
void sdk_queue_contracts(const std::string &directory) {
  for(unsigned pending:{0U,1U,3U,5U}) {
    Network n({Input::MANUAL_ON,0,1,0,-1});
    n.schedule(BASE+10000,[&n,pending]{n.adv_free=test_now+pending*90;});
    n.schedule(BASE+30000,[&n]{check(n,true,false);});
    n.run(40000);
    std::ofstream trace(directory+"/adv-pending"+std::to_string(pending)+".csv");trace<<n.trace.str();
    assert(n.trace.str().find("sdk_rejected")==std::string::npos);
  }
  // A controller stall is a fault input, not an inferred property of the reported installation.
  for(unsigned stall:{750U,1000U,2000U}) {
    Network n({Input::MANUAL_ON,100,1,0,-1});
    n.schedule(BASE+10000,[&n,stall]{n.adv_free=test_now+stall;});
    n.run(40000);
    for(auto on:n.physical_on)assert(on!=UNSEEN);
    for(auto confirmed:n.confirmed_on)assert(confirmed==UNSEEN);
    std::ofstream trace(directory+"/controller-stall"+std::to_string(stall)+"-lost-replies.csv");trace<<n.trace.str();
  }
  std::puts("SDK queue checks: configured buffer loads; separate watchdog/SDK deadlines under declared controller-stall faults");
}
#endif
}  // namespace control_sequences

int main(int argc,char **argv) {
  using namespace control_sequences;
  assert(argc==2);device_contracts();transport_contracts();
  const std::string directory=argv[1];
#ifdef L840_REAL_SDK
  sdk_queue_contracts(directory);
#endif
  std::ofstream results(directory+"/timing.csv");
  results<<"diagnostic,group_mechanism,reverse,fade_ms,loss_percent,lose_downlink,seed,motion_source,motion_at_ms,node,lc_mode,physical_on,gateway_on,ha_output,ha_known,first_physical_on_ms,first_confirmed_on_ms,tx_dropped,rx_dropped\n";
  unsigned contracts=0,sensitivity=0;
  for(int diagnostic:{-1,0,1,2})for(bool group:{false,true})for(bool reverse:{false,true}) {
    for(unsigned fade:{0U,1000U,10000U}) {
      Case c{Input::SCRIPTED,0,1,fade,diagnostic,reverse,false,group};
      round_trip(c);no_motion(c);replace_commands(c);contracts+=3;
    }
    repeated_motion({Input::SCRIPTED,0,1,0,diagnostic,reverse,false,group});++contracts;
    for(unsigned source:{0U,2U})for(unsigned at=9500;at<=15000;at+=250) {
      timing({Input::SCRIPTED,0,1,0,diagnostic,reverse,false,group},source,at,results,directory);++sensitivity;
    }
  }
  for(unsigned loss:{25U,50U})for(bool downlink:{false,true})for(unsigned seed:{1U,8U,29U,102U})
    for(int diagnostic:{-1,1})for(bool group:{false,true})for(unsigned at:{10000U,12000U,20000U}) {
      timing({Input::SCRIPTED,loss,seed,0,diagnostic,false,downlink,group},0,at,results,directory);++sensitivity;
    }
  std::printf("HA control sequences: %u fixed-contract scenarios passed; %u timing/loss scenarios recorded without prescribed On outcomes\n",contracts,sensitivity);
  return 0;
}
