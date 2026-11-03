namespace idf_host {
constexpr bt_mesh_client_op_pair_t pairs[] = {
    {0x8008,0x02},{0x8201,0x8204},{0x8202,0x8204},{0x824b,0x824e},{0x824c,0x824e},
    {0x8291,0x8294},{0x8292,0x8294},{0x829d,0x64},{0x62,0x64},{0x8230,0x51},{0x8231,0x52}};
struct Endpoint : Client {
  Endpoint() { user.op_pair=idf_host::pairs;user.op_pair_size=sizeof(idf_host::pairs)/sizeof(idf_host::pairs[0]); }
};
class Bridge {
 public:
  std::array<Endpoint,5> clients;
  static unsigned family(uint32_t opcode) {
    switch(opcode) {
      case 0x8008:case 0x02:return 0;
      case 0x8201:case 0x8202:case 0x8203:case 0x8204:return 1;
      case 0x824b:case 0x824c:case 0x824d:case 0x824e:return 2;
      case 0x8230:case 0x8231:case 0x51:case 0x52:return 3;
      default:return 4;
    }
  }
  int submit(uint32_t now,uint16_t destination,uint32_t opcode,uint32_t timeout,size_t payload,
             bool acknowledged,QueuedPacket &packet) {
    sdk_now=now;packets.clear();
    bt_mesh_client_common_param_t p{&clients[family(opcode)].model,{destination,0,0,0,0,7},opcode,int32_t(timeout)};
    const unsigned opcode_length=opcode<=0x7f?1:opcode<=0xffff?2:3;
    net_buf_simple message{unsigned(payload)+opcode_length};
    assert(message.len<=11); // Outgoing requests in this installation are unsegmented.
    int error=bt_mesh_client_send_msg(&p,&message,acknowledged,timeout_handler);
    if(!error) { assert(packets.size()==1);packet=packets[0]; }
    packets.clear();return error;
  }
  void start(uint32_t now,QueuedPacket packet) {
    sdk_now=now;
    if(packet.cb&&packet.cb->start)packet.cb->start(bt_mesh_pdu_duration(bt_mesh_net_transmit_get()),0,packet.data);
  }
  template<class Timeout> void tick(uint32_t now,Timeout timeout) {
    sdk_now=now;
    for(auto &client:clients) {
      auto current=client.internal.queue.head;
      while(current) {
        auto node=reinterpret_cast<bt_mesh_client_node_t *>(current);current=current->next;
        if(!node->timer.active||node->timer.due>now)continue;
        const auto destination=node->ctx.addr;const auto opcode=node->opcode;
        k_delayed_work_free(&node->timer);bt_mesh_client_free_node(node);
        timeout(destination,opcode);
      }
    }
  }
  uint32_t receive(uint32_t now,uint16_t source,uint32_t status) {
    sdk_now=now;bt_mesh_msg_ctx ctx{source,0x100,status};net_buf_simple message{};
    auto &model=clients[family(status)].model;
    auto node=bt_mesh_is_client_recv_publish_msg(&model,&ctx,&message,false);
    if(!node)return status; // The SDK reports unmatched replies as publications.
    const auto opcode=node->opcode;
    k_delayed_work_free(&node->timer);bt_mesh_client_free_node(node);return opcode;
  }
};
}  // namespace idf_host
