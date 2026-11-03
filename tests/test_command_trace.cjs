const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM, VirtualConsole} = require('jsdom');
const component = path.resolve(__dirname, '../esphome/components/steinel_mesh');
const html = fs.readFileSync(path.join(component, 'steinel_page.html'), 'utf8').replace(
  '<!-- STEINEL_I18N -->', '<script>' + fs.readFileSync(path.join(component, 'steinel_i18n.js'), 'utf8') + '</script>');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

async function boot() {
  const calls = [], files = [], errors = [];
  const server = {id:0, sequence:0, running:false, elapsed:0, events:[], failure:false, invalid:false};
  server.add = (count, node=24) => {
    for(let i=0;i<count;i++) {
      const sequence=++server.sequence;
      server.events.push({sequence,at_ms:1000+sequence*10,elapsed_ms:sequence*10,node,address:node,
        event:sequence%9,sdk_event:0,opcode:0x8202,request:0x8202,kind:7,step:1,tid:sequence%256,
        queue_depth:3,attempt:0,flags:0,detail:3000,error:0,before_on:0,after_on:1,
        before_auto:1,after_auto:0,raw:'01',device_name:'PRIVATE_NAME',key:'PRIVATE_KEY'});
    }
    server.events=server.events.slice(-32);
  };
  const console = new VirtualConsole(); console.on('jsdomError', error=>errors.push(error));
  const dom = new JSDOM(html,{url:'http://gateway.test/',runScripts:'dangerously',virtualConsole:console,
    beforeParse(window) {
      window.setInterval=()=>0; window.confirm=()=>true; window.Blob=global.Blob;
      window.URL.createObjectURL=blob=>{files.push(blob);return 'blob:test';};
      window.URL.revokeObjectURL=()=>{}; window.HTMLAnchorElement.prototype.click=function(){};
      window.fetch=async(url,options={})=>{
        calls.push({url,options}); let data, status=200;
        if(url==='/steinel/status') data={configured:true,enabled:true,mesh_ready:true,extended_diagnostics:true,
          gateway_version:'2.0.1-rc.4',networks:[],local_address:'7000'};
        else if(url==='/api/nodes') data={nodes:[]};
        else if(url.startsWith('https://api.github.com/')) data={tag_name:'v2.0.0',assets:[]};
        else if(url==='/api/state-trace'&&options.method==='POST') {
          if(options.body.get('action')==='start') {
            if(server.running) {status=409;data={message:'Another diagnostic session is running'};}
            else {server.id++;server.sequence=0;server.elapsed=0;server.running=true;server.events=[];data={id:server.id};}
          } else if(Number(options.body.get('id'))!==server.id) {status=409;data={message:'Command trace session changed'};}
          else {server.running=false;data={message:'Command trace stopped'};}
        } else if(url.startsWith('/api/state-trace?')) {
          if(server.failure)throw Error('Connection interrupted');
          const parameters=new URL(url,'http://gateway.test').searchParams;
          if(Number(parameters.get('id'))!==server.id) {status=409;data={message:'Command trace session changed'};}
          else {
            const after=Number(parameters.get('after'));
            data={schema:'steinel-state-trace/2',id:server.id,gateway_version:'2.0.1-rc.4',started_at_ms:1000,
              uptime_ms:1000+server.elapsed,elapsed_ms:server.elapsed,sequence:server.sequence,running:server.running,expired:false,
              buffer_bytes:1568,buffer_events:32,events:structuredClone(server.events.filter(e=>e.sequence>after))};
            if(server.invalid&&data.events.length)data.events[0].at_ms='invalid';
          }
        } else throw Error('Unexpected request: '+url);
        return {ok:status===200,status,json:async()=>structuredClone(data)};
      };
    }});
  await pause(40);
  const read=()=>dom.window.eval('readCommandTrace()');
  const trace=()=>dom.window.eval('commandTrace');
  const start=async()=>{dom.window.document.getElementById('traceStart').click();await pause(15);};
  const save=async()=>{dom.window.document.getElementById('traceDownload').click();await pause(15);return JSON.parse(await files.at(-1).text());};
  return {dom,server,calls,files,errors,read,trace,start,save};
}

(async()=>{
  let t=await boot();assert.equal(t.server.id,0);assert.equal(t.calls.some(c=>c.url.startsWith('/api/state-trace')),false);
  await t.start();assert.equal(t.server.running,true);
  for(const node of [24,18,12]) {t.server.add(10,node);await t.read();}
  assert.equal(t.trace().events.length,30);assert.equal(t.trace().missed_events,0);
  assert.deepEqual([...new Set(t.trace().events.map(e=>e.node))],[24,18,12]);
  assert.equal(t.trace().events.some(e=>e.before_auto===1&&e.after_auto===0),true);
  t.server.add(40,12);await t.read();assert.equal(t.trace().missed_events,8);
  assert.match(t.dom.window.document.getElementById('traceStatus').textContent,/missing events/);
  t.server.add(2);t.server.invalid=true;
  const oldLength=t.trace().events.length,oldSequence=t.trace().sequence;
  await assert.rejects(t.read(),/Invalid command trace event/);
  assert.equal(t.trace().events.length,oldLength);assert.equal(t.trace().sequence,oldSequence);
  t.server.invalid=false;await t.read();
  const saved=await t.save();assert.equal(saved.sequence,72);assert.equal(saved.events.length,64);
  assert.equal(saved.missed_events,8);assert.equal(saved.collection_stop_unconfirmed,false);
  assert.equal(saved.last_read_failed,false);assert.ok(saved.gateway_buffer_bytes<=2048);
  assert.equal(saved.clock_anchors.first.gateway_uptime_ms,1000);
  assert.ok(saved.clock_anchors.last.browser_response_unix_ms>=saved.clock_anchors.last.browser_request_unix_ms);
  assert.equal(JSON.stringify(saved).includes('PRIVATE_'),false);
  assert.equal(t.calls.some(c=>c.url==='/steinel/diagnostics'||c.url==='/steinel/node'),false);
  assert.equal(t.errors.length,0);t.dom.window.close();

  t=await boot();await t.start();t.server.add(5);await t.read();
  t.server.failure=true;const partial=await t.save();
  assert.equal(partial.events.length,5);assert.equal(partial.last_read_failed,true);
  assert.equal(partial.collection_stop_unconfirmed,false);assert.equal(partial.running,false);
  assert.equal(t.server.running,false);t.dom.window.close();

  t=await boot();await t.start();t.server.add(5);await t.read();t.server.id++;
  const changed=await t.save();assert.equal(changed.collection_stop_unconfirmed,true);
  assert.equal(changed.events.length,5);assert.equal(changed.last_read_failed,true);t.dom.window.close();

  t=await boot();await t.start();
  for(let i=0;i<130;i++){t.server.add(32);await t.read();}
  const bounded=await t.save();assert.equal(bounded.sequence,4160);assert.equal(bounded.events.length,4096);
  assert.equal(bounded.browser_limit_reached,true);assert.equal(bounded.missed_events,0);
  assert.equal(t.errors.length,0);t.dom.window.close();
  console.log('Command trace browser: passive all-node streaming, loss detection, atomic validation, partial export, session isolation and bounded storage passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
