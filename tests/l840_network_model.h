struct NativeEntities {
  std::array<SteinelMesh::NodeEntities, 3> nodes{};
  std::array<SteinelMesh::TestEntity, 3> available{}, output{}, identity{};
  void attach(SteinelMesh &g) {
    for(size_t i=0;i<3;++i) {
      nodes[i].available=&available[i];nodes[i].output=&output[i];
      nodes[i].firmware=nodes[i].hardware=nodes[i].product=nodes[i].model=&identity[i];
      g.node_states_[i].entities=&nodes[i];
    }
  }
};

void initialize_network(SteinelMesh &g, NativeEntities &entities, uint32_t age) {
  g.catalog_.count=3;
  for(size_t i=0;i<3;++i) {
    const auto &fixture=fixture_nodes[i];auto &node=g.catalog_.nodes[i];
    node.address=fixture.address;node.company_id=0x0563;node.product_id=fixture.product;node.selected=true;
    std::array<uint16_t,protocol::MAX_ELEMENTS> caps{};uint8_t count=0;
    assert(protocol::composition(fixture.composition.data(),fixture.composition.size(),caps,count));
    assert(protocol::le16(fixture.composition.data())==0x0563);
    assert(protocol::le16(fixture.composition.data()+2)==fixture.product);
    node.element_count=count;
    for(size_t element=0;element<count;++element)node.elements[element].capabilities=caps[element];
    auto &state=g.node_states_[i];state.composition_checked=state.composition_matches=true;
    state.composition_at=test_now+IDENTITY_INTERVAL;state.discovery_complete=true;
    state.supported=fixture.known;state.last_seen=test_now-100;
    state.on=0;state.automatic=1;state.brightness_known=state.supported&protocol::F_BRIGHTNESS;
    state.threshold=1000;state.run_time=120000;state.dirty=true;
    state.lux_element=3;state.motion_element=2;
    for(size_t field=0;field<state.value_seen.size();++field)
      if(state.supported&(1U<<field))state.value_seen[field]=test_now-1000;
    if(i==0&&age)state.value_seen[1]=test_now-age;
  }
  entities.attach(g);
}

void deliver(SteinelMesh &g, esp_ble_mesh_client_common_param_t params, bool ok,
             const std::vector<uint8_t> &bytes, uint16_t property=0, bool publication=false,int callback_error=0) {
  uint32_t route=params.ctx.recv_op;
  if(!route) switch(params.opcode) {
    case 0x8008:route=0x02;break;case 0x8201:case 0x8202:route=0x8204;break;
    case 0x824b:case 0x824c:route=0x824e;break;case 0x8291:case 0x8292:route=0x8294;break;
    case 0x829d:case 0x62:route=0x64;break;case 0x8230:route=0x51;break;case 0x8231:route=0x52;break;
  }
  if(route==ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS) {
    g.node_event_(&params,params.ctx.recv_op,ok&&!callback_error);return;
  }
  if(route==ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
    esp_ble_mesh_generic_client_cb_param_t reply{};reply.params=&params;reply.error_code=callback_error;
    if(!bytes.empty())reply.status_cb.onoff_status.present_onoff=bytes[0];
    if(bytes.size()==3) { reply.status_cb.onoff_status.op_en=true;
      reply.status_cb.onoff_status.target_onoff=bytes[1];reply.status_cb.onoff_status.remain_time=bytes[2]; }
    g.node_generic_event_(ok?(publication?ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:0):
                         ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT,&reply);return;
  }
  if(route==ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS||route==ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS) {
    Buffer buffer{};buffer.data=const_cast<uint8_t*>(bytes.data());buffer.len=bytes.size();
    esp_ble_mesh_sensor_client_cb_param_t reply{};reply.params=&params;reply.error_code=callback_error;
    reply.status_cb.sensor_status.marshalled_sensor_data=&buffer;
    reply.status_cb.descriptor_status.descriptor=&buffer;
    g.node_sensor_event_(ok?(publication?ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT:0):
                        ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT,&reply);return;
  }
  esp_ble_mesh_light_client_cb_param_t reply{};reply.params=&params;reply.error_code=callback_error;
  Buffer buffer{};buffer.data=const_cast<uint8_t*>(bytes.data());buffer.len=bytes.size();
  if(route==ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
    if(bytes.size()>=2)reply.status_cb.lightness_status.present_lightness=protocol::le16(bytes.data());
    if(bytes.size()==5) { reply.status_cb.lightness_status.op_en=true;
      reply.status_cb.lightness_status.target_lightness=protocol::le16(bytes.data()+2); }
  } else if(route==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS) {
    if(!bytes.empty())reply.status_cb.lc_mode_status.mode=bytes[0];
  } else if(route==ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS) {
    reply.status_cb.lc_property_status.property_id=property;reply.status_cb.lc_property_status.property_value=&buffer;
  } else assert(false);
  g.node_light_event_(ok?(publication?ESP_BLE_MESH_LIGHT_CLIENT_PUBLISH_EVT:0):
                       ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT,&reply);
}

void replay_captured_packets() {
  unsigned outputs=0,sensors=0;
  for(unsigned node=0;node<3;++node) {
    test_now=200000;SteinelMesh g;NativeEntities entities;initialize_network(g,entities,0);
    g.mesh_ready_=false;
    std::string expected="Off";
    for(const auto &event:captured_replies) {
      if(event.node!=node)continue;
      test_now=200000+event.time;
      esp_ble_mesh_client_common_param_t params{};
      params.ctx.addr=fixture_nodes[node].address+event.element;params.ctx.recv_op=event.opcode;
      deliver(g,params,true,event.raw,event.property,event.publication);
      if(event.element==0&&(event.opcode==0x8204||event.opcode==0x824e)) {
        const unsigned value=event.opcode==0x8204?event.raw[0]:protocol::le16(event.raw.data());
        expected=value?"On":"Off";++outputs;
      }
      if(event.opcode==0x52)++sensors;
      g.advance_nodes_(test_now);
      assert(entities.output[node].known&&entities.output[node].option==expected);
    }
  }
  std::printf("Recorded packet replay: %u output replies, %u Sensor messages; native HA output matches present values\n",outputs,sensors);
}

bool reproduce_global_gate() {
  test_now=200000;SteinelMesh g;NativeEntities entities;initialize_network(g,entities,95000);
  for(auto &state:g.node_states_) { state.value_seen[0]=test_now-25000;state.poll_at=test_now+30000; }
  unsigned before=wire_sends;g.poll_nodes_(test_now);
  bool passed=wire_sends!=before&&g.active_node_request_.kind==ON;
  std::printf("Independent output regression: old brightness on another device %s due output read\n",passed?"does not block":"BLOCKS");
  return passed;
}

struct Scenario {
  unsigned cycle,age;int diagnostic,offline;bool losses,refresh;
  uint32_t phase{0};
};

bool run_network(const Scenario &scenario, uint32_t &worst_latency) {
  constexpr uint32_t base=200000;
  const bool trace=std::getenv("L840_TRACE")&&scenario.cycle==0&&scenario.age==0&&
      scenario.diagnostic==-1&&scenario.losses&&scenario.offline==-1&&scenario.phase==0;
  test_now=base;wire_sends=0;SteinelMesh g;NativeEntities entities;
  initialize_network(g,entities,scenario.age);
  if(scenario.refresh)for(auto &state:g.node_states_) {
    state.force_poll=true;state.discovery_complete=false;state.poll_step=0;
  }
  if(scenario.diagnostic>=0)g.device_diagnostics_.start(test_now,1,scenario.diagnostic,
      fixture_nodes[scenario.diagnostic].address,scenario.diagnostic==1?2:4);
  auto cycle=physical_cycles[scenario.cycle];
  cycle.on+=scenario.phase;cycle.off+=scenario.phase;
  std::array<uint32_t,3> on_lag{},off_lag{},seen{},max_gap{};
  on_lag.fill(0xffffffffU);off_lag.fill(0xffffffffU);seen.fill(base-1000);
  std::array<unsigned,3> output_reads{},other_reads{},on_attempts{};
  std::vector<unsigned> profile_offsets(reply_profiles.size(),0);
  std::vector<bool> published(fixture_publications.size(),false);
  bool in_flight=false,accepted=false;uint32_t finish=0;unsigned captured_count=0;
  SteinelMesh::NodeRequest active{};esp_ble_mesh_client_common_param_t params{};
  while(test_now<base+180000) {
    const uint32_t elapsed=test_now-base;
    const bool physical=elapsed>=cycle.on&&elapsed<cycle.off;
    for(size_t index=0;index<fixture_publications.size();++index) {
      const auto &event=fixture_publications[index];
      if(published[index]||int64_t(elapsed)<int64_t(cycle.on)+event.relative)continue;
      published[index]=true;
      if(int(event.node)==scenario.offline)continue;
      esp_ble_mesh_client_common_param_t publication{};
      publication.ctx.addr=fixture_nodes[event.node].address+event.element;
      publication.ctx.recv_op=ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS;
      deliver(g,publication,true,event.raw,0,true);
    }
    if(in_flight&&test_now>=finish) {
      in_flight=false;
      std::vector<uint8_t> payload;
      if(active.kind==ON)payload={uint8_t(physical)};
      else if(active.kind==LEVEL)payload=physical?std::vector<uint8_t>{255,255}:std::vector<uint8_t>{0,0};
      else if(active.kind==AUTO)payload={1};
      else if(active.kind==THRESHOLD)payload={0xe8,3,0};
      else if(active.kind==RUN)payload={0xc0,0xd4,1};
      else if(active.kind==SENSOR||active.kind==SENSOR_PROPERTY) {
        if(active.element==2)payload={0x40,8,0};
        else if(active.element==3)payload=physical?std::vector<uint8_t>{0xc4,9,255,255,255}:
                                                  std::vector<uint8_t>{0xc4,9,0,0,0};
      }
      deliver(g,params,accepted,payload,g.node_expected_property_);
    }
    g.expire_access_operation_(test_now);
    const unsigned before=wire_sends;
    g.advance_nodes_(test_now);
    if(wire_sends!=before) {
      assert(!in_flight&&wire_sends==before+1);in_flight=true;
      params=wire_request;params.ctx.recv_op=g.node_expected_status_;
      active=g.active_node_request_;
      assert(active.node<3);
      uint32_t latency=120;accepted=active.kind==COMPOSITION;
      const unsigned property=active.kind==SENSOR_PROPERTY?active.value:
                              active.kind==THRESHOLD?0x2b:active.kind==RUN?0x3c:0;
      for(size_t index=0;index<reply_profiles.size();++index) {
        const auto &profile=reply_profiles[index];
        if(profile.node!=active.node||profile.element!=active.element||
            profile.opcode!=params.opcode||profile.property!=property)continue;
        accepted=profile.outcomes[profile_offsets[index]++%profile.outcomes.size()];
        latency=profile.latency;break;
      }
      if(active.kind==ON) {
        ++output_reads[active.node];
        if(scenario.losses&&++on_attempts[active.node]%4==0)accepted=false;
      } else ++other_reads[active.node];
      if(int(active.node)==scenario.offline)accepted=false;
      finish=test_now+(accepted?latency:params.msg_timeout);
      if(trace)std::printf("  t=%u node=%04x kind=%u success=%d failures=%u burst=%u\n",
          elapsed,fixture_nodes[active.node].address,active.kind,accepted,
          g.node_states_[active.node].output_failures,g.node_priority_burst_);
      ++captured_count;
    }
    for(size_t node=0;node<3;++node) {
      if(int(node)==scenario.offline)continue;
      const auto &entity=entities.output[node];const auto &state=g.node_states_[node];
      if(state.value_seen[0]&&state.value_seen[0]!=seen[node]) {
        max_gap[node]=std::max(max_gap[node],state.value_seen[0]-seen[node]);seen[node]=state.value_seen[0];
      }
      if(entity.known&&entity.option=="On"&&elapsed>=cycle.on&&elapsed<cycle.off&&state.value_seen[0]>=base+cycle.on)
        on_lag[node]=std::min(on_lag[node],elapsed-cycle.on);
      if(entity.known&&entity.option=="Off"&&elapsed>=cycle.off&&state.value_seen[0]>=base+cycle.off)
        off_lag[node]=std::min(off_lag[node],elapsed-cycle.off);
    }
    test_now+=20;
  }
  bool passed=scenario.diagnostic<0||g.device_diagnostics_.probe_complete;
  uint32_t maximum=0,gap=0;
  const uint32_t limit=scenario.losses||scenario.offline>=0?10000:5000;
  for(size_t node=0;node<3;++node) {
    if(int(node)==scenario.offline)continue;
    maximum=std::max({maximum,on_lag[node],off_lag[node]});
    gap=std::max(gap,max_gap[node]);
    passed&=on_lag[node]<=limit&&off_lag[node]<=limit;
    passed&=max_gap[node]<=limit&&test_now-seen[node]<=limit;
    passed&=output_reads[node]>=3&&output_reads[node]<=110&&other_reads[node]>=2;
  }
  worst_latency=std::max(worst_latency,maximum);
  std::printf("L840 closed-loop: cycle=%u phase=%u age=%u diag=%d loss=%d offline=%d refresh=%d lag=%s%.2fs gap=%.2fs requests=%u %s\n",
      scenario.cycle,scenario.phase,scenario.age,scenario.diagnostic,scenario.losses,scenario.offline,scenario.refresh,
      maximum==0xffffffffU?"unconfirmed/":"",maximum==0xffffffffU?0.0:maximum/1000.0,gap/1000.0,captured_count,passed?"PASS":"FAIL");
  if(!passed)for(size_t node=0;node<3;++node)if(int(node)!=scenario.offline)
    std::printf("  node=0x%04x on_ms=%u off_ms=%u output_reads=%u other_reads=%u max_reply_gap=%u\n",
        fixture_nodes[node].address,on_lag[node],off_lag[node],output_reads[node],other_reads[node],max_gap[node]);
  return passed;
}
}  // namespace esphome::steinel_mesh

int main() {
  using namespace esphome::steinel_mesh;
  std::setvbuf(stdout,nullptr,_IONBF,0);
  replay_captured_packets();
  unsigned failures=!reproduce_global_gate(),cases=0;uint32_t worst=0;
  for(unsigned cycle=0;cycle<physical_cycles.size();++cycle)
    for(unsigned age:{0U,70000U,110000U})
      for(int diagnostic:{-1,0,1,2})for(bool losses:{false,true})
        for(uint32_t phase:{0U,997U,4993U,17011U,29009U}) {
        ++cases;failures+=!run_network({cycle,age,diagnostic,-1,losses,false,phase},worst);
      }
  for(bool refresh:{false,true})for(int diagnostic:{-1,0,1,2})
    for(uint32_t phase:{0U,997U,4993U,17011U,29009U}) {
    ++cases;failures+=!run_network({0,110000,diagnostic,2,false,refresh,phase},worst);
  }
  std::printf("Closed-loop summary: %u scenarios, %u failed checks\n",cases,failures);
  return failures?1:0;
}
