#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define BT_DBG(...) ((void)0)
#define BT_INFO(...) ((void)0)
#define BT_ERR(...) ((void)0)
#define K_MSEC(n) (n)
#define ARG_UNUSED(x) ((void)(x))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define BLE_MESH_ADDR_IS_UNICAST(a) ((a)>0&&(a)<0x8000)
#define BLE_MESH_ADDR_UNASSIGNED 0
#define BLE_MESH_TRANSMIT_COUNT(x) ((x)&7)
#define BLE_MESH_TRANSMIT_INT(x) ((((x)>>3)+1)*10)
#define BLE_MESH_SDU_UNSEG_MAX 11
#define BLE_MESH_APP_SEG_SDU_MAX 12
#define BLE_MESH_SEG_SZMIC_LONG 1
#define BLE_MESH_MIC_LONG 8
#define BLE_MESH_MIC_SHORT 4
#define CONFIG_BLE_MESH_CLIENT_MSG_TIMEOUT 4000
#define ADV_ITVL_MIN 20
#define HCI_TIME_FOR_START_ADV K_MSEC(5)

uint32_t sdk_now=0;
unsigned publications=0,allocated=0;
struct sys_snode_t { sys_snode_t *next{}; };
struct sys_slist_t { sys_snode_t *head{}; };
bool sys_slist_is_empty(sys_slist_t *l) { return !l->head; }
sys_snode_t *sys_slist_peek_head(sys_slist_t *l) { return l->head; }
sys_snode_t *sys_slist_peek_next(sys_snode_t *n) { return n->next; }
void sys_slist_append(sys_slist_t *l,sys_snode_t *n) {
  auto p=&l->head;while(*p)p=&(*p)->next;*p=n;n->next=nullptr;
}
void sys_slist_find_and_remove(sys_slist_t *l,sys_snode_t *n) {
  auto p=&l->head;while(*p&&*p!=n)p=&(*p)->next;if(*p)*p=n->next;
}
void bt_mesh_list_lock() {}
void bt_mesh_list_unlock() {}
struct k_work {};
using k_work_handler_t=void (*)(k_work *);
struct k_delayed_work { uint32_t due{};bool active{}; };
int k_delayed_work_init(k_delayed_work *w,k_work_handler_t) { *w={};return 0; }
int k_delayed_work_free(k_delayed_work *w) { w->active=false;return 0; }
void k_delayed_work_submit(k_delayed_work *w,uint32_t delay) { w->active=true;w->due=sdk_now+delay; }
uint32_t k_delayed_work_remaining_get(k_delayed_work *w) { return w->active&&w->due>sdk_now?w->due-sdk_now:0; }
struct bt_mesh_msg_ctx { uint16_t addr{},recv_dst{};uint32_t recv_op{},send_tag{};uint8_t send_szmic{},send_ttl=7; };
struct net_buf_simple { unsigned len{}; };
unsigned net_buf_simple_tailroom(net_buf_simple *) { return 16; }
bool bt_mesh_tag_send_segmented(uint32_t tag) { return tag&1; }
bool bt_mesh_tag_immutable_cred(uint32_t) { return false; }
uint8_t bt_mesh_net_transmit_get() { return 2|(1<<3); }
uint8_t bt_mesh_get_seg_rtx_num() { return 4; }
int32_t bt_mesh_get_seg_rtx_timeout(uint16_t,uint8_t ttl) { return 400+50*ttl; }
void *bt_mesh_calloc(size_t size) { ++allocated;return std::calloc(1,size); }
void bt_mesh_free(void *p) { assert(allocated);--allocated;std::free(p); }
struct bt_mesh_model { void *user_data{}; };
struct bt_mesh_send_cb { void (*start)(uint16_t,int,void *);void (*end)(int,void *); };
struct bt_mesh_client_op_pair_t { uint32_t cli_op,status_op; };
struct bt_mesh_client_internal_data_t { sys_slist_t queue; };
struct bt_mesh_client_user_data_t {
  void *internal_data{};const bt_mesh_client_op_pair_t *op_pair{};int op_pair_size{};
  void (*publish_status)(uint32_t,bt_mesh_model *,bt_mesh_msg_ctx *,net_buf_simple *);
};
struct bt_mesh_client_node_t {
  sys_snode_t client_node;bt_mesh_msg_ctx ctx;bt_mesh_model *model;
  uint32_t opcode,op_pending;int32_t timeout;k_delayed_work timer;
};
struct bt_mesh_client_common_param_t {
  bt_mesh_model *model;bt_mesh_msg_ctx ctx;uint32_t opcode;int32_t msg_timeout;
  const bt_mesh_send_cb *cb{};void *cb_data{};
};
struct QueuedPacket { const bt_mesh_send_cb *cb;void *data; };
std::vector<QueuedPacket> packets;
int bt_mesh_model_send(bt_mesh_model *,bt_mesh_msg_ctx *,net_buf_simple *,const bt_mesh_send_cb *cb,void *data) {
  packets.push_back({cb,data});return 0;
}
int bt_mesh_client_free_node(bt_mesh_client_node_t *);

// IDF_CLIENT_FUNCTIONS

void timeout_handler(k_work *) {}
void published(uint32_t,bt_mesh_model *,bt_mesh_msg_ctx *,net_buf_simple *) { ++publications; }
struct Client {
  bt_mesh_client_internal_data_t internal{};
  const bt_mesh_client_op_pair_t pairs[2]{{0x8201,0x8204},{0x8202,0x8204}};
  bt_mesh_client_user_data_t user{&internal,pairs,2,published};
  bt_mesh_model model{&user};
  ~Client() {
    while(internal.queue.head) {
      auto node=reinterpret_cast<bt_mesh_client_node_t *>(internal.queue.head);
      k_delayed_work_free(&node->timer);bt_mesh_client_free_node(node);
    }
  }
};

int main() {
  Client first,other;net_buf_simple message{4};
  bt_mesh_client_common_param_t p{&first.model,{0x18,0,0,0,0,7},0x8202,3000};
  assert(bt_mesh_pdu_duration(bt_mesh_net_transmit_get())==90);
  assert(client_calc_timeout(&p.ctx,&message,p.opcode,1200)==1200);
  assert(client_calc_timeout(&p.ctx,&message,p.opcode,3000)==3000);
  assert(bt_mesh_client_send_msg(&p,&message,true,timeout_handler)==0);
  auto node=client_pick_node(&first.internal.queue,0x18);
  assert(node&&!node->timer.active&&packets.size()==1);
  assert(bt_mesh_client_send_msg(&p,&message,true,timeout_handler)==-EBUSY);
  assert(bt_mesh_client_send_msg(&p,&message,false,timeout_handler)==0);
  auto second=p;second.ctx.addr=0x12;
  assert(bt_mesh_client_send_msg(&second,&message,true,timeout_handler)==0);
  second=p;second.model=&other.model;
  assert(bt_mesh_client_send_msg(&second,&message,true,timeout_handler)==0);

  bt_mesh_msg_ctx reply{0x18,0x100,0x8204};
  assert(!bt_mesh_is_client_recv_publish_msg(&first.model,&reply,&message,true));
  assert(publications==1); // Before radio start there is no running client response timer.
  sdk_now=450;packets[0].cb->start(90,0,packets[0].data);
  assert(node->timer.active&&node->timer.due==3450);
  sdk_now=3449;
  assert(bt_mesh_is_client_recv_publish_msg(&first.model,&reply,&message,true)==node);
  sdk_now=3450;
  assert(!bt_mesh_is_client_recv_publish_msg(&first.model,&reply,&message,true));
  assert(publications==2);

  for(uint32_t queue_delay:{0U,450U,750U,751U,1000U}) {
    Client client;packets.clear();sdk_now=10000;
    p.model=&client.model;
    assert(bt_mesh_client_send_msg(&p,&message,true,timeout_handler)==0);
    auto pending=client_pick_node(&client.internal.queue,0x18);
    sdk_now+=queue_delay;packets[0].cb->start(90,0,packets[0].data);
    const uint32_t watchdog=10000+3000+750;
    assert(pending->timer.due==10000+queue_delay+3000);
    assert((pending->timer.due>watchdog)==(queue_delay>750));
  }
  std::puts("ESP-IDF client source: queue admission, delayed timer start, busy destinations, separate clients and response/publication classification verified");
  std::puts("Configured ADV duration: 90ms; six buffers do not by themselves account for a 15-second delay");
}
