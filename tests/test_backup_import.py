"""Exercise the complete production importer, including per-model AppKey filtering."""
from pathlib import Path
import json
import os
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = runpy.run_path(str(ROOT / "tests/test_backup_parser.py"))
source = parser["SOURCE"]
header = (ROOT / "esphome/components/steinel_mesh/steinel_mesh.h").read_text()
constants = header[header.index("  static constexpr uint32_t CONFIG_MAGIC"):header.index("  struct StoredConfig")]
structures = header[header.index("  struct StoredConfig"):header.index("  struct StoredAddressConfirmation")]
method = source[source.index("bool SteinelMesh::parse_backup_("):source.index("bool SteinelMesh::install_network_(")]
stub = r'''
#include <new>
#define ESP_LOGI(...) ((void)0)
constexpr int ESP_MAC_WIFI_STA=0;
uint32_t esp_random(){return 1;}
int esp_read_mac(uint8_t *mac,int){std::memset(mac,1,6);return ESP_OK;}
struct CloudBody {bool use_flash;const esp_partition_t *partition;size_t length;};
class SteinelMesh {public:
''' + constants + structures + r'''
  bool parse_backup_(const CloudBody&,uint32_t,uint16_t,StoredConfig&,std::array<uint8_t,16>&,StoredAddressPolicy&,protocol::Catalog&,std::string&);
  bool load_cached_iv_index_(const std::array<uint8_t,16>&,uint32_t&){return false;}
  bool catalog_valid_=false,configured_=false,address_policy_valid_=false;
  protocol::Catalog catalog_{};StoredConfig config_{};StoredAddressPolicy address_policy_{};
};
'''
main = r'''
int main(int argc,char **argv){
  std::string input((std::istreambuf_iterator<char>(std::cin)),{});
  esp_partition_t partition{&input};CloudBody body{true,&partition,input.size()};
  SteinelMesh gateway;SteinelMesh::StoredConfig config;SteinelMesh::StoredAddressPolicy policy;
  protocol::Catalog catalog;std::array<uint8_t,16> key{};std::string error;
  const uint16_t requested=argc>1?std::stoul(argv[1]):0;
  if(!gateway.parse_backup_(body,0,requested,config,key,policy,catalog,error)){
    std::cout<<"invalid:"<<error;return 0;
  }
  if(input.size()>16384)assert(parser_yields>0);
  std::cout<<config.app_key_index<<':'<<catalog.count<<':';
  for(size_t i=0;i<catalog.count;++i){
    const auto &node=catalog.nodes[i];
    if(node.company_id==0x0563&&(node.product_id==0x1B1B||node.product_id==0x1E74)){
      assert(node.element_count==4&&!node.nightmatiq);
      assert(protocol::output_element(node)==0);
      assert(protocol::element_with(node,protocol::LC)==1);
      assert(protocol::element_with(node,protocol::SENSOR)==2);
      assert(node.elements[3].capabilities==protocol::SENSOR);
      assert(protocol::possible_functions(node)==127);
      assert(std::strcmp(protocol::product_name(node.company_id,node.product_id),
                         node.product_id==0x1B1B?"L 830 SC":"L 820 SC")==0);
    }
    for(size_t j=0;j<node.element_count;++j){const auto &e=node.elements[j];
      std::cout<<e.capabilities<<'/'<<e.app_key<<'/'<<e.sensor_group<<',';
    }
    std::cout<<';';
  }
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix="steinel-import-") as directory:
        binary = str(Path(directory) / "import")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-fsanitize=address,undefined",
                        "-g", "-I", str(ROOT / "esphome/components/steinel_mesh"), "-x", "c++", "-", "-o", binary],
                       input=parser["PREFIX"] + parser["PARSER"] + stub + method + main,
                       text=True, check=True)
        model = lambda mid, keys: {"modelId": mid, "bind": keys, "publish": {"address": "C123"}}
        node = {"name": "Lamp", "unicastAddress": "0001", "deviceKey": "ab" * 16,
                "cid": "0563", "pid": "1E79", "elements": [{"models": []}]}
        backup = {"meshUUID": "01" * 16, "meshName": "Network", "nodes": [node],
                  "appKeys": [{"index": k, "boundNetKey": 0, "key": "02" * 16} for k in [3, 4]],
                  "netKeys": [{"index": 0, "key": "03" * 16}],
                  "provisioners": [{"allocatedUnicastRange": [{"lowAddress": "0001", "highAddress": "7FFF"}]}]}

        def result(requested=0):
            return subprocess.run([binary, str(requested)], input=json.dumps(backup), text=True,
                                  capture_output=True, check=True).stdout

        models = node["elements"][0]["models"]
        models[:] = [model("1000", [3]), model("1100", [3])]
        assert result() == "3:1:9/3/49443,;"
        models[1]["bind"] = [4]
        assert result() == "3:1:1/3/0,;"
        models[0]["bind"] = [3, 4]; models[1]["bind"] = [4, 3]
        assert result() == "3:1:9/3/49443,;"
        models[0]["bind"] = [4, 3]; models[1]["bind"] = [3, 4]
        assert result() == "3:1:9/3/49443,;"
        models[0]["bind"] = [3]; models[1]["bind"] = []
        assert result() == "3:1:1/3/0,;"
        for invalid in [4096, -1, "3", None]:
            models[1]["bind"] = [3, invalid]
            assert result().startswith("invalid:")
        models[1]["bind"] = [3]
        node["elements"].append({"models": [model("130F", [4])]})
        assert result() == "3:1:9/3/49443,0/65535/0,;"
        node["elements"][1]["models"][0]["bind"] = [4, 3]
        assert result() == "3:1:9/3/49443,4/3/0,;"
        node["elements"] = [{"models": [model("1203", [3]), model("1000", [4]), model("1100", [4])]}]
        assert result() == "4:1:9/4/49443,;"
        # Unusable first nodes must not decide which network key is imported.
        from copy import deepcopy
        original_nodes = backup["nodes"]
        unavailable = deepcopy(node)
        unavailable["unicastAddress"] = "0010"
        unavailable["elements"] = [{"models": [model("1000", [99])]}]
        backup["nodes"] = [unavailable, node]
        assert result() == "4:2:0/65535/0,;9/4/49443,;"
        assert result(1) == "4:2:0/65535/0,;9/4/49443,;"
        assert result(16).startswith("invalid:")
        backup["nodes"].reverse()
        assert result() == "4:2:9/4/49443,;0/65535/0,;"
        assert result(16).startswith("invalid:")
        backup["nodes"] = original_nodes
        node["elements"][0]["models"].reverse()
        backup["appKeys"].reverse()
        assert result() == "4:1:9/4/49443,;"
        # Select a present, valid key even when the first binding has no key material.
        node["elements"][0]["models"] = [model("1000", [3, 4]), model("1100", [4, 3])]
        backup["appKeys"] = [entry for entry in backup["appKeys"] if entry["index"] == 4]
        assert result() == "4:1:9/4/49443,;"
        backup["appKeys"][0]["boundNetKey"] = 1
        assert result().startswith("invalid:")
        backup["appKeys"][0]["boundNetKey"] = 0
        # A key with more usable functions wins over a lower-index scene key.
        backup["appKeys"].append({"index": 3, "boundNetKey": 0, "key": "03" * 16})
        node["elements"][0]["models"] = [model("1000", [3, 4]), model("1100", [4])]
        assert result() == "4:1:9/4/49443,;"
        # Maximum catalog and key lists retain bounded metadata and deterministic selection.
        backup["appKeys"] = [{"index": k, "boundNetKey": 0, "key": "02" * 16} for k in range(16)]
        node["elements"] = [{"models": [model(mid, list(range(16)))
                                        for mid in ["1000", "1300", "130F", "1100"]]} for _ in range(8)]
        backup["nodes"] = [json.loads(json.dumps(node)) for _ in range(16)]
        for i, entry in enumerate(backup["nodes"]):
            entry["unicastAddress"] = f"{1 + 8 * i:04X}"
        backup["padding"] = "x" * 262144
        expected = "0:16:" + "15/0/49443," * 8 + ";"
        assert result().startswith(expected)
        # Four-element lamp layout, with synthetic keys, addresses and identifiers.
        lamp_models = (
            ("0000", "0002", "1000", "1002", "1004", "1006", "1007", "1200",
             "1201", "1203", "1204", "1206", "1207", "1300", "1301", "0563100B", "0563100D"),
            ("1000", "1001", "1205", "130F", "1310", "05631001", "05631004", "05631005", "05631006"),
            ("1100", "1101", "05631003"),
            ("1100", "1101"),
        )
        lamps = []
        for i, (pid, vid) in enumerate((("1B1B", "8883"), ("1E74", "8841"))):
            address = 0x100 * (i + 1)
            lamp = {"name": f"Lamp {i + 1}", "UUID": f"{i + 1:032X}",
                    "unicastAddress": f"{address:04X}", "deviceKey": "ab" * 16,
                    "cid": "0563", "pid": pid, "vid": vid, "elements": []}
            for index, mids in enumerate(lamp_models):
                entries = []
                for mid in mids:
                    entry = {"modelId": mid, "bind": [] if mid == "0000" else [0], "subscribe": []}
                    if mid == "1100":
                        entry["publish"] = {"address": f"{address + 1:04X}", "index": 0,
                                            "period": {"numberOfSteps": 10 if index == 3 else 0, "resolution": 1000}}
                    elif mid in ("1200", "05631005"):
                        entry["publish"] = {"address": "FFFF" if mid == "1200" else "FEFF", "index": 0}
                    entries.append(entry)
                lamp["elements"].append({"index": index, "location": "0000", "models": entries})
            lamps.append(lamp)
        backup = {"meshUUID": "01" * 16, "meshName": "Synthetic network", "nodes": [
            {"name": "Provisioner", "UUID": "03" * 16, "unicastAddress": "0001", "deviceKey": "",
             "elements": [{"models": [{"modelId": "0001", "bind": [], "publish": None}]}]}, *lamps],
            "appKeys": [{"index": k, "boundNetKey": 0, "key": "02" * 16} for k in range(3)],
            "netKeys": [{"index": 0, "key": "03" * 16}],
            "provisioners": [{"allocatedUnicastRange": [{"lowAddress": "0001", "highAddress": "7FFF"}]}]}
        lamp_catalog = "51/0/0,5/0/0,8/0/0,8/0/0,;"
        assert result() == "0:2:" + lamp_catalog * 2
        assert result(0x200) == "0:2:" + lamp_catalog * 2
        backup["nodes"].reverse()
        backup["appKeys"].reverse()
        assert result() == "0:2:" + lamp_catalog * 2
        assert result(0x200) == "0:2:" + lamp_catalog * 2
        print("Production backup import: per-model keys, reordered bindings, groups, unbound models and invalid keys passed")
        print("L 820 SC / L 830 SC: four elements, product names, output/LC routing and both Sensor elements passed")


if __name__ == "__main__":
    run()
