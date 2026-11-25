const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const page = fs.readFileSync(path.join(__dirname, '../esphome/components/steinel_mesh/steinel_page.html'), 'utf8');
const script = page.slice(page.indexOf('const groupSpecs='), page.indexOf('let statusPollPromise='));
const fields = Object.fromEntries(['groupAddress', 'groupCommand', 'groupValue', 'groupStatus', 'sendGroup']
  .map(id => [id, {value:'', disabled:false, textContent:''}]));
fields.groupCommand.value = 'output';
const calls = [];
let reject = false;
const context = {q: selector => fields[selector.slice(1)], URLSearchParams,
  lastStatus:{enabled:true, mesh_ready:true, busy:false},
  post: async (url, body) => {
    calls.push({url, body});
    if (reject) throw Error('Rejected');
    return {message:'Group command queued; delivery is not confirmed'};
  }};
vm.runInNewContext(script, context);

(async () => {
  fields.groupAddress.value = '0xC000'; fields.groupValue.value = '1';
  await fields.sendGroup.onclick();
  assert.equal(calls[0].url, '/steinel/group');
  assert.equal(calls[0].body.get('address'), '49152');
  assert.equal(calls[0].body.get('command'), 'output');
  assert.equal(calls[0].body.get('value'), '1');
  assert.match(fields.groupStatus.textContent, /not confirmed/);
  for (const address of ['0xBFFF', 'FF00', 'C00', '0xC0000', '']) {
    fields.groupAddress.value = address; await fields.sendGroup.onclick();
    assert.equal(calls.length, 1);
    assert.equal(fields.groupStatus.textContent, 'Invalid group command parameters');
  }
  fields.groupAddress.value = 'FEFF';
  for (const [command, value, wire] of [
    ['brightness','100','65535'], ['automatic','0','0'],
    ['threshold','12.34','1234'], ['run_time','12.345','12345']
  ]) {
    fields.groupCommand.value = command; fields.groupCommand.onchange();
    fields.groupValue.value = value; await fields.sendGroup.onclick();
    assert.equal(calls.at(-1).body.get('address'), '65279');
    assert.equal(calls.at(-1).body.get('value'), wire);
  }
  fields.groupCommand.value = 'automatic'; fields.groupValue.value = '0.5';
  await fields.sendGroup.onclick(); assert.equal(calls.length, 5);
  reject = true; fields.groupValue.value = '1'; await fields.sendGroup.onclick();
  assert.equal(fields.groupStatus.textContent, 'Rejected');
  context.lastStatus.mesh_ready = false; await fields.sendGroup.onclick();
  assert.equal(fields.sendGroup.disabled, true);
  console.log('Group web form validation and request encoding passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
