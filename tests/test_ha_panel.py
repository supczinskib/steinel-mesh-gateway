"""Check the optional panel against legacy and per-device HA entity registries."""
from pathlib import Path
from types import SimpleNamespace
import subprocess

import jinja2
import yaml

ROOT = Path(__file__).resolve().parents[1]
package = yaml.safe_load((ROOT / "home-assistant/steinel-nightmatiq-package.yaml").read_text())
templates = package["template"][0]["sensor"]
legacy_mode = "select.steinel_nightmatiq_plus_nightmatiq_mode"
legacy_output = "binary_sensor.steinel_nightmatiq_plus_nightmatiq_actual_light_output"
options = ["Auto", "Always On", "Always Off"]
registry = {}
devices = {}
attributes = {}
device_models = {}


class States:
    @property
    def select(self):
        return [SimpleNamespace(entity_id=key, state=value["state"], attributes=value["attributes"])
                for key, value in registry.items() if key.startswith("select.")]

    def __call__(self, entity):
        return registry.get(entity, {}).get("state", "unavailable")


environment = jinja2.Environment()
environment.globals.update(
    states=States(),
    is_state=lambda entity, state: registry.get(entity, {}).get("state") == state,
    state_attr=lambda entity, attr: registry.get(entity, {}).get("attributes", {}).get(attr),
    device_id=lambda entity: devices.get(entity),
    device_attr=lambda device, attr: device_models.get(device) if attr == "model" else None,
    device_entities=lambda device: [key for key in registry if device and devices.get(key) == device],
)


def check(pl, en, icon, available):
    for template, expected in zip(templates, (pl, en)):
        prior = attributes.setdefault(template["unique_id"], {})
        environment.globals["this"] = SimpleNamespace(attributes=prior)
        assert environment.from_string(template["state"]).render().strip() == expected
        assert environment.from_string(template["icon"]).render().strip() == icon
        assert environment.from_string(template["availability"]).render().strip() == str(available)
        prior["target_entities"] = yaml.safe_load(environment.from_string(
            template["attributes"]["target_entities"]).render().strip())


registry.update({legacy_mode: {"state": "Auto", "attributes": {"options": options}},
                 legacy_output: {"state": "on", "attributes": {}}})
check("włączony · Auto", "on · Auto", "mdi:lightbulb", True)
registry[legacy_mode]["state"] = "unavailable"
registry[legacy_output]["state"] = "unavailable"
attributes.clear()
mode = "select.garden_mode"
output = "binary_sensor.garden_output"
registry[mode] = {"state": "Always Off", "attributes": {"options": options}}
registry[output] = {"state": "off", "attributes": {"friendly_name": "Garden Output"}}
devices.update({mode: "nightmatiq-2", output: "nightmatiq-2"})
check("wyłączony", "off", "mdi:lightbulb-outline", True)
# A user-renamed entity still belongs to the same device.
renamed = "binary_sensor.my_garden"
registry[renamed] = registry.pop(output)
devices[renamed] = devices.pop(output)
registry[renamed]["state"] = "on"
registry[mode]["state"] = "Auto"
check("włączony · Auto", "on · Auto", "mdi:lightbulb", True)
registry[renamed]["state"] = "unavailable"
check("wyłączony · Auto", "off · Auto", "mdi:lightbulb-outline", False)
registry[renamed]["state"] = "on"
second_mode, second_output = "select.second_mode", "binary_sensor.second_output"
registry[second_mode] = {"state": "Auto", "attributes": {"options": options}}
registry[second_output] = {"state": "off", "attributes": {"friendly_name": "Second Output"}}
devices.update({second_mode: "second", second_output: "second"})
registry[mode]["state"] = "unavailable"
check("włączony", "on", "mdi:lightbulb", False)
registry.pop(mode)
check("włączony", "on", "mdi:lightbulb", False)
registry[mode] = {"state": "Auto", "attributes": {"options": options}}
check("włączony · Auto", "on · Auto", "mdi:lightbulb", True)
attributes.clear()
check("wyłączony", "off", "mdi:lightbulb-outline", False)
device_models["second"] = "ar01v3_esp_rc01_gateway"
device_models["nightmatiq-2"] = "steinel_mesh_gateway"
attributes.clear()
check("włączony · Auto", "on · Auto", "mdi:lightbulb", True)

popup = (ROOT / "home-assistant/steinel-nightmatiq-popup.js").read_text()
start = popup.index("  const LEGACY_ENTITIES")
end = popup.index("  const ENTITIES", start)
script = "const assert=require('node:assert/strict'); const window={};\n" + popup[start:end]
script += r'''
const modes=['Auto','Always On','Always Off'];
assert.deepEqual(resolveEntities(null),{});
const hass={states:{
  'select.old':{state:'unavailable',attributes:{options:modes}},
  'select.garden_mode':{state:'Auto',attributes:{options:modes}},
  'binary_sensor.renamed':{state:'on',attributes:{friendly_name:'Garden Output'}},
  'sensor.garden_lux':{state:'25',attributes:{device_class:'illuminance'}},
  'number.garden_threshold':{state:'10',attributes:{unit_of_measurement:'lx'}},
  'sensor.other_lux':{state:'999',attributes:{device_class:'illuminance'}}
},entities:{
  'select.garden_mode':{device_id:'garden'},'binary_sensor.renamed':{device_id:'garden'},
  'sensor.garden_lux':{device_id:'garden'},'number.garden_threshold':{device_id:'garden'},
  'sensor.other_lux':{device_id:'other'}
}};
assert.deepEqual(resolveEntities(hass),{mode:'select.garden_mode',output:'binary_sensor.renamed',
  illuminance:'sensor.garden_lux',threshold:'number.garden_threshold'});
hass.states['select.second_mode']={state:'Auto',attributes:{options:modes}};
hass.states['binary_sensor.second_output']={state:'off',attributes:{friendly_name:'Second Output'}};
hass.entities['select.second_mode']={device_id:'second'};
hass.entities['binary_sensor.second_output']={device_id:'second'};
resolvedDevice=null;resolvedEntities=null;
hass.devices={garden:{model:'steinel_mesh_gateway'},second:{model:'ar01v3_esp_rc01_gateway'}};
assert.equal(resolveEntities(hass).mode,'select.garden_mode');
delete hass.devices;
hass.states['select.garden_mode'].state='unavailable';
assert.equal(resolveEntities(hass).mode,'select.garden_mode');
delete hass.states['select.garden_mode'];
assert.equal(resolveEntities(hass).mode,'select.garden_mode');
resolvedDevice=null;resolvedEntities=null;
hass.states['select.garden_mode']={state:'unavailable',attributes:{options:modes}};
assert.deepEqual(resolveEntities(hass),{});
delete hass.states['select.second_mode'];delete hass.states['binary_sensor.second_output'];
assert.equal(resolveEntities(hass).mode,'select.garden_mode');
resolvedDevice=null;resolvedEntities=null;
const legacy={states:{[LEGACY_ENTITIES.mode]:{state:'Auto',attributes:{options:modes}},
  [LEGACY_ENTITIES.output]:{state:'on',attributes:{}}}};
assert.deepEqual(resolveEntities(legacy),LEGACY_ENTITIES);
assert.deepEqual(resolveEntities(hass),LEGACY_ENTITIES);
window.steinelNightmatiqEntities={mode:'select.second',output:'binary_sensor.second'};
assert.equal(resolveEntities(hass).mode,'select.second');
assert.equal(resolveEntities(hass).output,'binary_sensor.second');
assert.equal(resolveEntities(hass).threshold,undefined);
'''
subprocess.run(["node", "-e", script], check=True)
print("HA panel: legacy IDs, per-device discovery, renamed entities, availability and explicit selection passed")
