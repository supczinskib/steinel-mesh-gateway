// Independent access-layer device model. No gateway state or scheduler helpers.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace l840_device {
struct Message {
  uint16_t source{}, destination{};
  uint32_t opcode{};
  std::vector<uint8_t> bytes;
};
inline uint16_t u16(const std::vector<uint8_t> &v,size_t i=0) { return v.at(i)|(uint16_t(v.at(i+1))<<8); }
inline uint32_t u24(const std::vector<uint8_t> &v,size_t i=0) { return u16(v,i)|(uint32_t(v.at(i+2))<<16); }
inline std::vector<uint8_t> le(uint32_t value,unsigned count) {
  std::vector<uint8_t> out;while(count--) { out.push_back(value);value>>=8; }return out;
}
inline uint32_t transition_ms(uint8_t encoded) {
  const uint32_t resolution[]={100,1000,10000,600000};
  assert((encoded&63)!=63);return (encoded&63)*resolution[encoded>>6];
}
inline uint8_t remaining(uint32_t ms) {
  const uint32_t resolution[]={100,1000,10000,600000};
  for(unsigned i=0;i<4;++i)if((ms+resolution[i]-1)/resolution[i]<=62)
    return uint8_t((i<<6)|((ms+resolution[i]-1)/resolution[i]));
  return 0x3f;
}

struct Settings {
  uint32_t run_ms=120000,fade_on_ms=0,fade_ms=0,prolong_ms=0,standby_auto_ms=0,standby_manual_ms=0;
  uint32_t occupancy_delay_ms=0,default_transition_ms=0;
  uint16_t on_level=65535,prolong_level=0,standby_level=0;
  bool occupancy_mode=true;
};

class Lamp {
 public:
  enum class LC { OFF,STANDBY,FADE_ON,RUN,FADE,PROLONG,STANDBY_AUTO,STANDBY_MANUAL };
  uint16_t address,group=0xc001;
  bool has_sensor,mode=true;
  Settings settings;
  LC lc=LC::STANDBY;
  uint32_t threshold=1000,ambient=0;
  uint16_t level=0,last_nonzero=65535;
  uint32_t applied_sets=0;
  std::vector<uint8_t> composition;
  std::vector<std::vector<uint16_t>> models;

  Lamp(uint16_t addr,bool sensor,std::vector<uint8_t> raw):address(addr),has_sensor(sensor),composition(std::move(raw)) {
    // Parse the captured SIG list independently of the gateway composition parser.
    size_t at=10;assert(composition.size()>=10);
    while(at<composition.size()) {
      assert(at+4<=composition.size());const unsigned sig=composition[at+2],vendor=composition[at+3];at+=4;
      std::vector<uint16_t> element;
      for(unsigned i=0;i<sig;++i) { element.push_back(u16(composition,at));at+=2; }
      at+=vendor*4;assert(at<=composition.size());models.push_back(element);
    }
    assert(at==composition.size()&&supports(0,0x1000)&&supports(0,0x1300)&&supports(1,0x130f));
  }
  bool supports(unsigned element,uint16_t model) const {
    return element<models.size()&&std::find(models[element].begin(),models[element].end(),model)!=models[element].end();
  }
  bool on() const { return level!=0; }
  bool controller_on() const { return lc!=LC::OFF&&lc!=LC::STANDBY; }
  uint16_t target() const { return pending_manual_?pending_manual_->target:transition_target_; }
  uint32_t remaining_ms(uint32_t now) const {
    const auto end=pending_manual_?pending_manual_->at+pending_manual_->duration:transition_end_;
    return end>now?end-now:0;
  }

  void advance(uint32_t now) {
    // Process overdue timers at their deadlines, not at the host polling instant.
    for(unsigned guard=0;guard<16;++guard) {
      uint32_t next=0;
      if(timer_)next=timer_;
      if(!occupancy_due_.empty()&&(!next||occupancy_due_.front()<next))next=occupancy_due_.front();
      if(pending_lc_&&(!next||pending_lc_->at<next))next=pending_lc_->at;
      if(pending_manual_&&(!next||pending_manual_->at<next))next=pending_manual_->at;
      if(!next||next>now)break;
      interpolate(next);
      if(pending_manual_&&pending_manual_->at==next) {
        const auto command=*pending_manual_;pending_manual_.reset();manual(command.target,next,command.duration,0);
      } else if(pending_lc_&&pending_lc_->at==next) {
        const auto command=*pending_lc_;pending_lc_.reset();light_onoff(command.on,next,command.duration);
      } else if(!occupancy_due_.empty()&&occupancy_due_.front()==next) {
        occupancy_due_.erase(occupancy_due_.begin());occupancy_event(next);
      } else {
        timer_=0;
        switch(lc) {
          case LC::FADE_ON: enter(LC::RUN,next);break;
          case LC::RUN: enter(LC::FADE,next);break;
          case LC::FADE: enter(LC::PROLONG,next);break;
          case LC::PROLONG: enter(LC::STANDBY_AUTO,next);break;
          case LC::STANDBY_AUTO: case LC::STANDBY_MANUAL: enter(LC::STANDBY,next);break;
          default: assert(false);
        }
      }
    }
    interpolate(now);
  }
  void set_mode(bool enabled,uint32_t now) {
    advance(now);if(mode==enabled)return;mode=enabled;++applied_sets;
    if(enabled)enter(LC::STANDBY,now);
    else {
      // Disabling LC releases its binding; it does not command physical Off.
      lc=LC::OFF;timer_=0;
      transition_start_=transition_end_=now;transition_target_=level;
    }
  }
  void occupancy(uint32_t now) {
    advance(now);
    if(settings.occupancy_delay_ms) {
      const auto due=now+settings.occupancy_delay_ms;
      occupancy_due_.insert(std::upper_bound(occupancy_due_.begin(),occupancy_due_.end(),due),due);
    }
    else occupancy_event(now);
  }
  void light_onoff(bool on,uint32_t now,std::optional<uint32_t> duration={}) {
    advance(now);if(!mode)return;
    if(on) {
      if(lc==LC::RUN)timer_=now+settings.run_ms;
      else if(lc!=LC::FADE_ON&&lc!=LC::OFF)enter(LC::FADE_ON,now,duration);
    } else if(lc!=LC::OFF&&lc!=LC::STANDBY&&lc!=LC::STANDBY_MANUAL)
      enter(LC::STANDBY_MANUAL,now,duration);
    advance(now);
  }
  std::vector<uint8_t> output_status(uint32_t now) {
    advance(now);std::vector<uint8_t> out{uint8_t(on())};
    if(remaining_ms(now)) { out.push_back(target()!=0);out.push_back(remaining(remaining_ms(now))); }
    return out;
  }
  std::vector<uint8_t> lightness_status(uint32_t now) {
    advance(now);auto out=le(level,2);
    if(remaining_ms(now)) { auto t=le(target(),2);out.insert(out.end(),t.begin(),t.end());out.push_back(remaining(remaining_ms(now))); }
    return out;
  }
  std::optional<Message> receive(const Message &message,uint32_t now) {
    advance(now);const auto &b=message.bytes;const auto op=message.opcode;
    const bool grouped=message.destination==group;
    if(!grouped&&(message.destination<address||message.destination>=address+models.size()))return {};
    const unsigned element=grouped?1:message.destination-address;
    auto response=[&](uint32_t status,std::vector<uint8_t> bytes)->std::optional<Message> {
      return Message{uint16_t(address+element),message.source,status,std::move(bytes)};
    };
    if(grouped) {
      // Explicit hypothesis: occupancy multicast to each LC, not a shared output.
      if(op==0x52&&b==std::vector<uint8_t>{0x40,0x08,0xc8})occupancy(now);
      else if(op==0x829b&&(b.size()==2||b.size()==4)&&b[0]<=1&&!duplicate(message,b[1],now)) {
        const auto duration=b.size()==4?std::optional<uint32_t>(transition_ms(b[2])):std::nullopt;
        if(b.size()==4&&b[3])pending_lc_=PendingLC{now+uint32_t(b[3])*5,bool(b[0]),duration};
        else light_onoff(b[0],now,duration);
      }
      return {};
    }
    if(op==0x8008&&element==0)return response(0x02,composition);
    if((op==0x8201||op==0x8202||op==0x8203)&&supports(element,0x1000)) {
      if(op!=0x8201) {
        if((b.size()!=2&&b.size()!=4)||b[0]>1)return {};
        if(!duplicate(message,b[1],now)) {
          ++applied_sets;
          const auto duration=b.size()==4?std::optional<uint32_t>(transition_ms(b[2])):std::nullopt;
          const uint32_t delay=b.size()==4?uint32_t(b[3])*5:0;
          if(element==1) {
            // A controller element is not the physical Lightness output element.
            if(delay)pending_lc_=PendingLC{now+delay,bool(b[0]),duration};
            else light_onoff(b[0],now,duration);
          } else manual(b[0]?(last_nonzero?last_nonzero:65535):0,now,duration.value_or(settings.default_transition_ms),delay);
        }
      }
      if(op==0x8203)return {};
      return response(0x8204,element==0?output_status(now):std::vector<uint8_t>{uint8_t(controller_on())});
    }
    if((op==0x824b||op==0x824c||op==0x824d)&&supports(element,0x1300)) {
      if(op!=0x824b) {
        if(b.size()!=3&&b.size()!=5)return {};
        if(!duplicate(message,b[2],now)) {
          ++applied_sets;manual(u16(b),now,b.size()==5?transition_ms(b[3]):settings.default_transition_ms,b.size()==5?uint32_t(b[4])*5:0);
        }
      }
      if(op==0x824d)return {};
      return response(0x824e,lightness_status(now));
    }
    if((op==0x8291||op==0x8292||op==0x8293)&&supports(element,0x130f)) {
      if(op!=0x8291) { if(b.size()!=1||b[0]>1)return {};set_mode(b[0],now); }
      if(op==0x8293)return {};
      return response(0x8294,{uint8_t(mode)});
    }
    if(op==0x829d&&supports(element,0x130f)&&b.size()==2) {
      const auto property=u16(b);
      const auto value=property_value(property);if(!value)return {};
      auto bytes=le(property,2),value_bytes=le(*value,3);bytes.insert(bytes.end(),value_bytes.begin(),value_bytes.end());
      return response(0x64,bytes);
    }
    if((op==0x62||op==0x63)&&supports(element,0x130f)&&b.size()==5) {
      const auto property=u16(b);const auto value=u24(b,2);
      if(property==0x2b)threshold=value;else if(property==0x3c)settings.run_ms=value;else return {};
      ++applied_sets;if(op==0x63)return {};return response(0x64,b);
    }
    if((op==0x8230||op==0x8231)&&supports(element,0x1100)) {
      const uint16_t property=has_sensor&&element==2?0x42:0x4e;
      if(op==0x8230) { auto bytes=le(property,2);bytes.resize(8,0);return response(0x51,bytes); }
      if(!b.empty()&&(b.size()!=2||u16(b)!=property))return response(0x52,{});
      if(property==0x42)return response(0x52,{0x40,0x08,0});
      auto bytes=std::vector<uint8_t>{0xc4,0x09};auto value=le(ambient,3);bytes.insert(bytes.end(),value.begin(),value.end());
      return response(0x52,bytes);
    }
    return {};
  }

 private:
  uint32_t timer_=0,transition_start_=0,transition_end_=0;
  std::vector<uint32_t> occupancy_due_;
  uint16_t transition_initial_=0,transition_target_=0;
  struct PendingLC { uint32_t at;bool on;std::optional<uint32_t> duration; };
  std::optional<PendingLC> pending_lc_;
  struct PendingManual { uint32_t at;uint16_t target;uint32_t duration; };
  std::optional<PendingManual> pending_manual_;
  std::map<std::tuple<uint16_t,uint16_t,uint32_t,uint8_t>,uint32_t> transactions_;
  bool duplicate(const Message &m,uint8_t tid,uint32_t now) {
    // ACK/unack variants share a transaction family; other senders stay independent.
    const auto family=(m.opcode==0x8203?0x8202:m.opcode==0x824d?0x824c:m.opcode);
    auto key=std::make_tuple(m.source,m.destination,family,tid);
    auto found=transactions_.find(key);
    if(found!=transactions_.end()&&now-found->second<6000)return true;
    transactions_[key]=now;return false;
  }
  void interpolate(uint32_t now) {
    if(now<transition_start_)return;
    if(transition_end_<=transition_start_||now>=transition_end_)level=transition_target_;
    else level=uint16_t(int64_t(transition_initial_)+(int64_t(transition_target_)-transition_initial_)*
                       (now-transition_start_)/(transition_end_-transition_start_));
    if(level&&now>=transition_end_)last_nonzero=level;
  }
  void ramp(uint16_t target,uint32_t now,uint32_t duration,uint32_t delay=0) {
    interpolate(now);transition_initial_=level;transition_target_=target;
    transition_start_=now+delay;transition_end_=transition_start_+duration;interpolate(now);
  }
  void manual(uint16_t target,uint32_t now,uint32_t duration,uint32_t delay) {
    pending_manual_.reset();
    if(delay) { pending_manual_=PendingManual{now+delay,target,duration};return; }
    // Unsolicited changes of the bound output release automatic control.
    if(target!=level&&mode)set_mode(false,now);
    ramp(target,now,duration,delay);
  }
  void enter(LC state,uint32_t now,std::optional<uint32_t> duration={}) {
    lc=state;timer_=0;
    uint32_t time=0;
    switch(state) {
      case LC::OFF: return;
      case LC::STANDBY:ramp(settings.standby_level,now,0);return;
      case LC::FADE_ON:time=duration.value_or(settings.fade_on_ms);ramp(settings.on_level,now,time);break;
      case LC::RUN:timer_=now+settings.run_ms;return;
      case LC::FADE:time=settings.fade_ms;ramp(settings.prolong_level,now,time);break;
      case LC::PROLONG:time=settings.prolong_ms;break;
      case LC::STANDBY_AUTO:time=settings.standby_auto_ms;ramp(settings.standby_level,now,time);break;
      case LC::STANDBY_MANUAL:time=duration.value_or(settings.standby_manual_ms);ramp(settings.standby_level,now,time);break;
    }
    timer_=now+time;
  }
  void occupancy_event(uint32_t now) {
    if(!mode||(lc==LC::STANDBY&&!settings.occupancy_mode))return;
    if(lc==LC::RUN)timer_=now+settings.run_ms;
    else if(lc!=LC::OFF&&lc!=LC::FADE_ON&&lc!=LC::STANDBY_MANUAL)enter(LC::FADE_ON,now);
  }
  std::optional<uint32_t> property_value(uint16_t id) const {
    switch(id) {
      case 0x2b:return threshold;case 0x3c:return settings.run_ms;
      case 0x36:return settings.fade_ms;case 0x37:return settings.fade_on_ms;
      case 0x38:return settings.standby_auto_ms;case 0x39:return settings.standby_manual_ms;
      case 0x3a:return settings.occupancy_delay_ms;case 0x3b:return settings.prolong_ms;
      default:return {};
    }
  }
};
}  // namespace l840_device
