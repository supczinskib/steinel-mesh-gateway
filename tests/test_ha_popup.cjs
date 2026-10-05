const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM, VirtualConsole} = require('jsdom');
const source = fs.readFileSync(path.resolve(__dirname, '../home-assistant/steinel-nightmatiq-popup.js'), 'utf8');

function addDevice(hass, name, available = true) {
  const ids = {mode:`select.${name}_mode`, output:`binary_sensor.${name}_output`,
    threshold:`number.${name}_threshold`, illuminance:`sensor.${name}_lux`};
  for (const [field, id] of Object.entries(ids)) {
    hass.entities[id] = {device_id:name};
    hass.states[id] = {state:available ? (field === 'mode' ? 'Auto' : field === 'output' ? 'on' : '25') : 'unavailable',
      attributes:field === 'mode' ? {options:['Auto','Always On','Always Off']} :
        field === 'output' ? {friendly_name:`${name} Output`} :
          field === 'threshold' ? {unit_of_measurement:'lx'} : {device_class:'illuminance'}};
  }
  return ids;
}

function boot(ambiguous = false) {
  const errors = [], calls = [];
  const virtualConsole = new VirtualConsole();
  virtualConsole.on('jsdomError', error => errors.push(error));
  const dom = new JSDOM('<home-assistant></home-assistant>',
    {url:'https://ha.test/',runScripts:'outside-only',virtualConsole});
  let now = 0, timerId = 0;
  const timers = new Map();
  dom.window.setTimeout = (callback, delay) => {
    timers.set(++timerId, {at:now + delay, callback});return timerId;
  };
  dom.window.clearTimeout = id => timers.delete(id);
  const advance = milliseconds => {
    now += milliseconds;
    for (const [id, timer] of [...timers]) if (timer.at <= now) {
      timers.delete(id);timer.callback();
    }
  };
  const hass = {states:{},entities:{},devices:{},locale:{language:'en'},
    callService:async(domain,service,data,target) => calls.push({domain,service,data:{...data},entity:target.entity_id})};
  const first = addDevice(hass,'garden',!ambiguous);
  if (ambiguous) addDevice(hass,'entrance');
  dom.window.document.querySelector('home-assistant').hass = hass;
  dom.window.eval(source);
  const popup = dom.window.document.createElement('steinel-nightmatiq-popup');
  dom.window.document.body.appendChild(popup);
  assert.equal(errors.length,0);
  return {dom,hass,first,popup,calls,errors,advance};
}

(async () => {
  const test = boot();
  const {hass,first,popup,calls} = test;
  await popup.setMode('Always On');await popup.setThreshold(50);
  assert.equal(calls[0].entity,first.mode);assert.equal(calls[1].entity,first.threshold);
  addDevice(hass,'entrance');
  for (const id of Object.values(first)) hass.states[id].state = 'unavailable';
  popup.updateState();
  assert.ok([...popup.shadowRoot.querySelectorAll('.mode')].every(button=>button.disabled));
  assert.ok(popup.shadowRoot.querySelector('#thresholdRange').disabled);
  await popup.setMode('Always Off');await popup.setThreshold(70);
  assert.equal(calls.length,2);
  for (const id of Object.values(first)) {delete hass.states[id];delete hass.entities[id];}
  popup.updateState();await popup.setMode('Auto');
  assert.equal(calls.length,2);
  addDevice(hass,'garden');popup.updateState();
  assert.ok([...popup.shadowRoot.querySelectorAll('.mode')].every(button=>!button.disabled));
  await popup.setMode('Always Off');assert.equal(calls[2].entity,first.mode);
  assert.equal(test.errors.length,0);test.dom.window.close();

  const ambiguous = boot(true);
  assert.ok([...ambiguous.popup.shadowRoot.querySelectorAll('.mode')].every(button=>button.disabled));
  await ambiguous.popup.setMode('Always On');await ambiguous.popup.setThreshold(50);
  assert.equal(ambiguous.calls.length,0);
  ambiguous.dom.window.steinelNightmatiqEntities = {mode:'select.entrance_mode',output:'binary_sensor.entrance_output'};
  ambiguous.popup.updateState();
  await ambiguous.popup.setMode('Always Off');await ambiguous.popup.setThreshold(80);
  assert.equal(ambiguous.calls.length,1);assert.equal(ambiguous.calls[0].entity,'select.entrance_mode');
  assert.ok(ambiguous.popup.shadowRoot.querySelector('#thresholdRange').disabled);
  assert.equal(ambiguous.errors.length,0);ambiguous.dom.window.close();
  const delayed = boot();
  delayed.hass.states[delayed.first.mode].state = 'Always Off';
  delayed.popup.updateState();
  await delayed.popup.setMode('Auto');
  const activeMode = () => delayed.popup.shadowRoot.querySelector('.mode.active')?.dataset.mode;
  assert.equal(activeMode(), 'Auto');
  delayed.advance(6000);delayed.popup.updateState();
  assert.equal(activeMode(), 'Auto');
  delayed.hass.states[delayed.first.mode].state = 'Auto';delayed.popup.updateState();
  assert.equal(delayed.popup.optimisticMode, null);
  await delayed.popup.setMode('Always Off');
  delayed.advance(12000);delayed.popup.updateState();
  assert.equal(activeMode(), 'Always Off');
  delayed.advance(60000);delayed.popup.updateState();
  assert.equal(activeMode(), 'Auto');
  delayed.hass.callService = async () => {throw new Error('Service unavailable');};
  await delayed.popup.setMode('Always On');
  assert.equal(delayed.popup.optimisticMode, null);
  delayed.dom.window.close();
  const area = boot();
  area.hass.devices.garden = {area_id:'garden'};
  area.hass.states['binary_sensor.garden_available'] = {state:'on',attributes:{}};
  area.hass.entities['binary_sensor.garden_available'] = {device_id:'garden'};
  const originalCards = [...Object.values(area.first), 'binary_sensor.garden_available', 'sensor.other'].map(entity=>({type:'tile',entity}));
  class AreaStrategy extends area.dom.window.HTMLElement {
    static async generate() {return {sections:[{cards:originalCards}]};}
  }
  area.dom.window.customElements.define('home-area-view-strategy',AreaStrategy);
  area.advance(0);
  await new Promise(resolve=>setImmediate(resolve));
  const view = await AreaStrategy.generate({area:'garden'},area.hass);
  assert.equal(view.sections[0].cards.length,3);
  assert.deepEqual(view.sections[0].cards.filter(card=>card.type==='tile').map(card=>card.entity),['sensor.other']);
  assert.equal(view.sections[0].cards.filter(card=>card.card?.entity==='sensor.steinel_nightmatiq_sensor_state').length,2);
  assert.equal(area.hass.states[area.first.mode].state,'Auto');
  const untouched = await AreaStrategy.generate({area:'entrance'},area.hass);
  assert.equal(untouched.sections[0].cards.length,originalCards.length);
  assert.equal(area.calls.length,0);
  area.dom.window.close();
  console.log('HA popup: pinned device, offline/removed entities, ambiguous selection and service targets passed');
})().catch(error=>{console.error(error);process.exit(1);});
