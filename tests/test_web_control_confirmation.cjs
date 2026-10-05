const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM, VirtualConsole} = require('jsdom');
const root = path.resolve(__dirname, '../esphome/components/steinel_mesh');
const html = fs.readFileSync(path.join(root, 'steinel_page.html'), 'utf8').replace(
  '<!-- STEINEL_I18N -->', '<script>' + fs.readFileSync(path.join(root, 'steinel_i18n.js'), 'utf8') + '</script>');
const flush = async () => { for (let i = 0; i < 12; ++i) await Promise.resolve(); };

async function boot() {
  let now = 100000, serial = 0, activeReads = 0, maxReads = 0;
  const timers = new Map(), calls = [], errors = [];
  const status = {configured:true, enabled:true, mesh_ready:true, busy:false,
    gateway_version:'1.9.1', networks:[], network:'Home', local_address:'7000'};
  const nodes = [
    {address:2, name:'Sensor', model:'NightmatIQ Plus', company:0x0563, product:0x1DCE,
      capabilities:29, functions:173, selected:true, available:true, verified:true,
      mode:1, output:1, automatic:0, lux:37, threshold:500, run_time:0},
    {address:10, name:'Lamp', model:'L 820 SC', company:0x0563, product:0x1E74,
      capabilities:15, functions:127, selected:true, available:true, verified:true,
      mode:-1, output:1, automatic:1, brightness:65535, lux:100, threshold:2000, run_time:30000}
  ];
  const behavior = {postDelay:0, postStatus:200, readDelay:0, readFailure:false, postHang:false};
  const console = new VirtualConsole(); console.on('jsdomError', error => errors.push(error));
  let window;
  const dom = new JSDOM(html, {url:'http://gateway.test/', runScripts:'dangerously', virtualConsole:console,
    beforeParse(w) {
      window = w;
      w.Date.now = () => now;
      w.setInterval = () => 0;
      w.setTimeout = (fn, delay = 0) => { const id = ++serial; timers.set(id, {at:now + delay, fn}); return id; };
      w.clearTimeout = id => timers.delete(id);
      w.confirm = () => true;
      w.fetch = (url, options = {}) => {
        calls.push({url, at:now, options});
        let delay = 0, data, code = 200, read = false;
        if (url === '/steinel/status') data = structuredClone(status);
        else if (url === '/api/nodes') { data = {nodes:structuredClone(nodes)}; delay = behavior.readDelay; read = true; }
        else if (url === '/steinel/node') { data = {message:behavior.postStatus === 200 ? "Command queued; waiting for the device's confirmed state" : 'Command rejected'}; delay = behavior.postDelay; code = behavior.postStatus; }
        else data = {tag_name:'v1.1.1'};
        if (read) { ++activeReads; maxReads = Math.max(maxReads, activeReads); }
        return new Promise((resolve, reject) => {
          let settled = false, timer = 0;
          const finish = error => {
            if (settled) return;
            settled = true; if (timer) w.clearTimeout(timer);
            if (read) --activeReads;
            if (error) reject(error);
            else resolve({ok:code === 200, status:code, json:async () => data});
          };
          if (options.signal) options.signal.addEventListener('abort', () => {
            const error = Error('Aborted'); error.name = 'AbortError'; finish(error);
          });
          if (url === '/steinel/node' && behavior.postHang) return;
          const complete = () => finish(read && behavior.readFailure ? Error('Offline') : null);
          if (delay) timer = w.setTimeout(complete, delay); else complete();
        });
      };
    }});
  await flush();
  await window.poll();
  await flush();
  const advance = async ms => {
    await flush();
    const end = now + ms;
    for (let count = 0; count < 1000; ++count) {
      const next = [...timers].filter(([,timer]) => timer.at <= end).sort((a,b) => a[1].at - b[1].at)[0];
      if (!next) { now = end; await flush(); return; }
      now = next[1].at; timers.delete(next[0]); next[1].fn(); await flush();
    }
    throw Error('Unbounded timer loop');
  };
  const field = (index, command) => dom.window.document.querySelectorAll('#deviceList > .maintenance-section')[index].querySelector(`[data-command="${command}"]`);
  const change = async (index, command, value) => {
    const control = field(index, command); control.value = String(value);
    control.dispatchEvent(new window.Event('change')); await flush(); return control;
  };
  return {dom, window, status, nodes, behavior, calls, errors, advance, field, change, maxReads:() => maxReads};
}

const watchdog = setTimeout(() => {console.error('Web control test did not complete'); process.exit(1);}, 10000);
(async () => {
  let t = await boot();
  let mode = await t.change(0, 'mode', 2);
  await t.window.poll(); await flush();
  assert.equal(mode.value, '2', 'a stale poll must not undo the requested mode');
  assert.equal(mode.disabled, false, 'confirmation must not block the next command');
  assert.equal(mode.getAttribute('aria-busy'), 'true');
  assert.match(t.dom.window.document.getElementById('message').textContent, /waiting for the device/);
  const reads = t.calls.filter(c => c.url === '/api/nodes').length;
  await t.advance(3000);
  assert.ok(t.calls.filter(c => c.url === '/api/nodes').length >= reads + 3, 'poll while a write awaits confirmation');
  assert.equal(mode.value, '2'); assert.equal(mode.disabled, false);
  assert.equal(mode.getAttribute('aria-busy'), 'true');
  assert.match(t.dom.window.document.querySelector('#deviceList .notice').textContent, /Output: on/);
  mode.focus(); t.nodes[0].mode = 2;
  await t.advance(1000);
  assert.equal(mode.disabled, false); assert.equal(mode.value, '2');
  assert.equal(mode.hasAttribute('aria-busy'), false);
  const settledReads = t.calls.filter(c => c.url === '/api/nodes').length;
  await t.advance(5000);
  assert.equal(t.calls.filter(c => c.url === '/api/nodes').length, settledReads, 'fast polling stops after confirmation');
  t.behavior.readDelay = 400;
  const first = t.window.poll(); const second = t.window.poll(); const third = t.window.poll();
  await t.advance(500); await Promise.all([first, second, third]);
  assert.equal(t.maxReads(), 1, 'device reads must not overlap');
  t.dom.window.close();

  t = await boot(); mode = await t.change(0, 'mode', 2);
  t.status.message = 'Mesh client ready; polling selected devices'; t.behavior.readDelay = 400;
  const pendingStatusPoll = t.window.poll(); await flush();
  assert.match(t.dom.window.document.getElementById('message').textContent, /waiting for the device/, 'normal status polling must retain the pending message');
  await t.advance(500); await pendingStatusPoll;
  t.dom.window.close();

  t = await boot(); t.behavior.postDelay = 3000;
  mode = await t.change(0, 'mode', 0); t.nodes[0].mode = 0;
  await t.advance(2000); assert.equal(mode.getAttribute('aria-busy'), 'true', 'wait for command admission before settling');
  await t.advance(2000); assert.equal(mode.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  t = await boot();
  const cases = [['output',0,0], ['automatic',0,0], ['brightness',25,16384], ['threshold',5,500], ['run_time',120,120000]];
  for (const [command, value, raw] of cases) {
    const control = await t.change(1, command, value);
    await t.window.poll(); await flush(); assert.equal(control.value, String(value)); assert.equal(control.disabled, false);
    t.nodes[1][command] = raw;
  }
  await t.advance(1000);
  for (const [command, value] of cases) { assert.equal(t.field(1, command).value, String(value)); assert.equal(t.field(1, command).disabled, false); }
  assert.equal(t.calls.filter(c => c.url === '/steinel/node').length, cases.length, 'polling must never resend a write');
  t.dom.window.close();

  t = await boot();
  const threshold = await t.change(1, 'threshold', 20.2);
  await t.advance(1000); assert.equal(threshold.getAttribute('aria-busy'), 'true', 'display rounding must not confirm a different raw threshold');
  t.nodes[1].threshold = 2020; await t.advance(1000); assert.equal(threshold.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  t = await boot();
  const template = structuredClone(t.nodes[0]);
  t.nodes.splice(0, t.nodes.length, ...Array.from({length:16}, (_,index) => ({...template,address:2+index*3,name:`Node ${index}`})));
  await t.window.poll(); await flush();
  for (let index = 0; index < 16; ++index) await t.change(index, 'mode', 2);
  const beforeBatch = t.calls.filter(c => c.url === '/api/nodes').length;
  await t.advance(1000);
  assert.equal(t.calls.filter(c => c.url === '/api/nodes').length, beforeBatch+1, 'one shared read for sixteen pending devices');
  for (const node of t.nodes) node.mode = 2;
  await t.advance(1000);
  for (let index = 0; index < 16; ++index) assert.equal(t.field(index, 'mode').disabled, false);
  assert.equal(t.maxReads(), 1);
  t.dom.window.close();

  t = await boot(); mode = await t.change(0, 'mode', 2);
  t.nodes[0].firmware = '1.1.0'; t.nodes[1].functions = 63;
  await t.window.poll(); await flush(); mode = t.field(0, 'mode');
  assert.equal(mode.value, '2'); assert.equal(mode.getAttribute('aria-busy'), 'true', 'card rebuilding must retain pending commands');
  const language = t.dom.window.document.getElementById('language'); language.value = 'fr'; language.dispatchEvent(new t.window.Event('change'));
  await flush(); assert.equal(mode.value, '2');
  t.nodes[0].mode = 2; await t.advance(1000); assert.equal(mode.disabled, false);
  t.dom.window.close();

  t = await boot(); mode = await t.change(0, 'mode', 2);
  await t.advance(60000);
  assert.equal(mode.value, '1'); assert.equal(mode.disabled, false);
  assert.match(t.dom.window.document.getElementById('message').textContent, /did not confirm/);
  const timeoutReads = t.calls.filter(c => c.url === '/api/nodes').length;
  await t.advance(10000); assert.equal(t.calls.filter(c => c.url === '/api/nodes').length, timeoutReads);
  t.dom.window.close();

  t = await boot(); t.behavior.postStatus = 409; mode = await t.change(0, 'mode', 2);
  assert.equal(mode.value, '1'); assert.equal(mode.disabled, false);
  assert.equal(mode.hasAttribute('aria-busy'), false);
  await t.advance(3000); assert.equal(t.calls.filter(c => c.url === '/steinel/node').length, 1);
  t.dom.window.close();

  t = await boot(); t.behavior.postHang = true; mode = await t.change(0, 'mode', 2);
  await t.advance(6500); assert.equal(mode.disabled, false); assert.equal(mode.value, '1');
  assert.match(t.dom.window.document.getElementById('message').textContent, /timed out/);
  t.dom.window.close();

  t = await boot(); mode = await t.change(0, 'mode', 2);
  t.behavior.readDelay = 10000;
  await t.advance(7000);
  assert.equal(t.maxReads(), 1); assert.equal(mode.disabled, false); assert.equal(mode.getAttribute('aria-busy'), 'true');
  await t.advance(53000); assert.equal(mode.disabled, false); assert.equal(mode.value, '1');
  assert.equal(t.calls.filter(c => c.url === '/steinel/node').length, 1);
  t.dom.window.close();

  t = await boot(); mode = await t.change(0, 'mode', 2); t.nodes[0].selected = false;
  await t.advance(1000);
  mode = t.field(0, 'mode');
  assert.equal(mode.disabled, true); assert.equal(mode.hasAttribute('aria-busy'), false);
  assert.match(t.dom.window.document.getElementById('message').textContent, /no longer available/);
  t.dom.window.close();

  t = await boot(); t.behavior.postDelay = 2000;
  mode = await t.change(0, 'mode', 2);
  await t.change(0, 'mode', 0); await t.change(0, 'mode', 1);
  assert.equal(mode.value, '1'); assert.equal(mode.disabled, false);
  assert.equal(mode.getAttribute('aria-busy'), 'true');
  assert.equal(t.calls.filter(c => c.url === '/steinel/node').length, 1, 'write admission is serialized');
  await t.advance(2500);
  assert.equal(mode.value, '1'); assert.equal(mode.getAttribute('aria-busy'), 'true', 'an old acknowledgement cannot settle the latest target');
  const writes = t.calls.filter(c => c.url === '/steinel/node');
  assert.deepEqual(writes.map(c => c.options.body.get('value')), ['2','1'], 'skip superseded unsent commands');
  await t.advance(2500);
  assert.equal(mode.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  t = await boot(); t.behavior.postDelay = 2000; t.behavior.postStatus = 409;
  mode = await t.change(0, 'mode', 2);
  t.behavior.postStatus = 200; await t.change(0, 'mode', 0);
  await t.advance(2500);
  assert.equal(mode.value, '0'); assert.equal(mode.getAttribute('aria-busy'), 'true', 'old errors cannot roll back newer intent');
  t.nodes[0].mode = 0; await t.advance(2500);
  assert.equal(mode.value, '0'); assert.equal(mode.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  t = await boot(); t.nodes[0].pending_controls = 32; t.nodes[0].pending_values = [0,0,0,0,0,2];
  await t.window.poll(); await flush(); mode = t.field(0, 'mode');
  assert.equal(mode.value, '2'); assert.equal(mode.getAttribute('aria-busy'), 'true', 'another client or a page reload retains server intent');
  assert.equal(mode.disabled, false);
  assert.match(t.dom.window.document.querySelector('#deviceList .notice').textContent, /Output: on/, 'actual output is not optimistic');
  mode = await t.change(0, 'mode', 1); // Already confirmed old mode, but still queued at the server.
  t.nodes[0].pending_values[5] = 1;
  await t.advance(1000); assert.equal(mode.getAttribute('aria-busy'), 'true', 'a cached matching value is not confirmation');
  t.nodes[0].pending_controls = 0;
  await t.advance(1000); assert.equal(mode.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  t = await boot(); t.behavior.readDelay = 2000;
  const oldRead = t.window.poll(); await flush();
  mode = await t.change(0, 'mode', 1);
  await t.advance(2000); await oldRead;
  assert.equal(mode.getAttribute('aria-busy'), 'true', 'a read started before admission cannot confirm a write');
  await t.advance(3000); assert.equal(mode.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  t = await boot();
  const draft = await t.change(1, 'threshold', 25);
  draft.focus(); draft.value = '30'; draft.dispatchEvent(new t.window.Event('input'));
  await t.advance(1000); assert.equal(draft.value, '30', 'polling cannot erase a new numeric draft');
  t.nodes[1].threshold = 2500;
  await t.advance(1000); assert.equal(draft.value, '30', 'confirmation cannot erase a numeric draft either');
  draft.dispatchEvent(new t.window.Event('change')); await flush();
  assert.equal(t.calls.filter(c => c.url === '/steinel/node').at(-1).options.body.get('value'), '3000');
  t.nodes[1].threshold = 3000; await t.advance(1000);
  assert.equal(draft.value, '30'); assert.equal(draft.hasAttribute('aria-busy'), false);
  t.dom.window.close();

  console.log('Web control confirmation: delayed replies, all controls, serialized polling, focused fields, rebuilds, language, rejection and bounded timeouts passed.');
  clearTimeout(watchdog);
})().catch(error => {console.error(error); process.exit(1);});
