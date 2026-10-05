// Browser regression tests. jsdom is never embedded in the firmware.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM, VirtualConsole} = require('jsdom');
const directory = path.resolve(__dirname, '../esphome/components/steinel_mesh');
const html = fs.readFileSync(path.join(directory, 'steinel_page.html'), 'utf8').replace(
  '<!-- STEINEL_I18N -->', '<script>\n' + fs.readFileSync(path.join(directory, 'steinel_i18n.js'), 'utf8') + '\n</script>');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const status = {configured:true,enabled:true,mesh_ready:true,busy:false,
  gateway_version:'2.0.0',runtime_mode:'Bluetooth Mesh',network:'Motion',
  local_address:'7000',iv_index:7,iv_index_confirmed:true,networks:[],
  connected_ssid:'WiFi',factory_password:true};
const nodes = [
  {address:2,name:'Motion',model:'IS 180',company:0x0563,product:0x1DE0,
    uuid:'01020304050607080910111213141516',firmware:'1.2.3',hardware:'',
    composition_vid:0x0883,capabilities:15,functions:127,elements:3,selected:true,
    available:true,verified:true,output:1,automatic:1,motion:0,lux:2500,
    threshold:2500,run_time:30000,brightness:65535,mode:-1,
    sensors:[{element:2,address:4,property:0x8123,length:2,raw:'AABB',age_seconds:5,available:true,truncated:false},
             {element:3,address:5,property:0x8123,length:10,raw:'0102030405060708',age_seconds:200,available:false,truncated:true}]},
  {address:10,name:'Garden',model:'NightmatIQ Plus',company:0x0563,product:0x1DCE,
    uuid:'',firmware:'',hardware:'',composition_vid:0,capabilities:29,functions:173,
    elements:3,selected:true,available:true,verified:true,
    output:1,automatic:0,motion:-1,lux:100,threshold:2500,run_time:0,brightness:0,mode:1},
  {address:20,name:'<img src=x onerror=alert(1)>',model:'',company:0x0563,product:123,
    uuid:'',firmware:'',hardware:'',composition_vid:0,capabilities:8,functions:0,
    elements:1,selected:false,available:false,verified:false}
];
const diagnosticSnapshot={schema:'steinel-device-diagnostics/1',id:91,gateway_version:'2.0.0',elapsed_ms:1000,
  running:true,probe_complete:false,sequence:2,company:0x0563,product:0x1E74,firmware:'1.2.3',hardware:'1',
  elements:[{index:0,capabilities:8,bound:true}],
  composition:{length:20,valid:true,truncated:false,raw:'6305741E83080000000000000101001163052381'},
  events:[{sequence:1,elapsed_ms:0,element:0,event:0,opcode:0x8008,property:0,error:0,length:0,truncated:false,raw:''},
          {sequence:2,elapsed_ms:900,element:0,event:2,opcode:0x52,property:0,error:0,length:2,truncated:false,raw:'AABB'}],
  name:'PRIVATE_DEVICE_NAME',network:'PRIVATE_NETWORK',uuid:'PRIVATE_UUID',device_key:'PRIVATE_KEY',password:'PRIVATE_PASSWORD',address:12345};
async function boot(saved, storageBlocked=false) {
  const errors = [], calls = [], blobs=[], downloads=[];
  const console = new VirtualConsole(); console.on('jsdomError', error => errors.push(error));
  const dom = new JSDOM(html, {url:'http://gateway.test/',runScripts:'dangerously',virtualConsole:console,
    beforeParse(window) {
      if (saved) window.localStorage.setItem('steinel.mesh.language', saved);
      if (storageBlocked) Object.defineProperty(window,'localStorage',{get(){throw new Error('disabled');}});
      window.confirm = () => true; window.prompt = () => '';
      window.Blob=class{constructor(parts,options){this.parts=parts;this.options=options;}};
      window.URL.createObjectURL=blob=>{blobs.push(blob);return 'blob:report'};
      window.URL.revokeObjectURL=()=>{};
      window.HTMLAnchorElement.prototype.click=function(){downloads.push(this.download)};
      window.setInterval = () => 0;
      window.fetch = async (url, options) => {
        calls.push({url,options});
        if(url==='/steinel/diagnostics'){
          const action=options.body.get('action');diagnosticSnapshot.running=action==='start';
          return {ok:true,status:200,json:async()=>action==='start'?{id:91}:{message:'Stopped'}};
        }
        if(url.startsWith('/api/diagnostics'))return {ok:true,status:200,json:async()=>structuredClone(diagnosticSnapshot)};
        const data = url === '/steinel/status' ? status : url === '/api/nodes' ? {nodes} :
          url.startsWith('https:') ? {tag_name:'v2.0.0'} : {message:'Command queued'};
        return {ok:true,status:200,json:async()=>structuredClone(data)};
      };
    }});
  await wait(80);
  return {dom,window:dom.window,document:dom.window.document,errors,calls,blobs,downloads};
}
function choose(test, language) {
  const picker = test.document.getElementById('language'); picker.value = language;
  picker.dispatchEvent(new test.window.Event('change'));
}
const cards = test => [...test.document.querySelectorAll('#deviceList > .maintenance-section')];
(async () => {
  const test = await boot();
  assert.equal(test.document.documentElement.lang,'en');
  assert.equal(test.document.getElementById('networkName').textContent,'Motion');
  assert.equal(cards(test).length,3);
  for (const checkbox of test.document.querySelectorAll('input[type=checkbox]'))
    assert.equal(test.window.getComputedStyle(checkbox).accentColor,'var(--accent)');
  assert.equal(test.window.getComputedStyle(test.document.documentElement).getPropertyValue('--accent'),'#f39200');
  assert.equal(test.document.getElementById('modeControl'),null);
  assert.equal(test.document.getElementById('nodeName'),null);
  const setup = test.document.getElementById('setup');
  const installPanel = test.document.getElementById('installPanel');
  const localBackup = test.document.getElementById('localBackup');
  assert.ok(setup.compareDocumentPosition(installPanel) & test.window.Node.DOCUMENT_POSITION_FOLLOWING);
  assert.ok(installPanel.compareDocumentPosition(localBackup) & test.window.Node.DOCUMENT_POSITION_FOLLOWING);
  const manual = test.document.getElementById('firmware').closest('details');
  const manualSummaryStyle = test.window.getComputedStyle(manual.querySelector('summary'));
  const manualContentGap = test.window.getComputedStyle(manual.querySelector('.field')).marginTop;
  const manualSectionGap = test.window.getComputedStyle(manual.nextElementSibling).marginTop;
  for (const section of [test.document.getElementById('diagnostics'),...test.document.querySelectorAll('.device-details,.device-controls')]) {
    const summaryStyle = test.window.getComputedStyle(section.querySelector('summary'));
    for (const property of ['fontSize','lineHeight','color','cursor'])
      assert.equal(summaryStyle[property],manualSummaryStyle[property],property);
    assert.equal(summaryStyle.fontWeight || '400',manualSummaryStyle.fontWeight || '400');
    assert.equal(test.window.getComputedStyle(section).marginTop,test.window.getComputedStyle(manual).marginTop);
    assert.equal(test.window.getComputedStyle(section.querySelector('.details,.control-grid')).marginTop,manualContentGap);
    section.open=true;
    assert.equal(section.open,true);
    section.open=false;
    assert.equal(section.open,false);
    if(section.classList.contains('device-details')||section.classList.contains('device-controls'))
      assert.equal(test.window.getComputedStyle(section).marginBottom,manualSectionGap);
    else
      assert.equal(test.window.getComputedStyle(section.nextElementSibling).marginTop,manualSectionGap);
  }
  assert.equal(cards(test)[2].querySelector('img'),null); // backup names are inert text
  assert.equal(cards(test)[2].querySelector('.control-row'),null); // Sensor != motion/lux
  assert.equal(cards(test)[2].querySelector('.device-controls'),null);
  for (const card of cards(test).slice(0,2)) {
    const controls=card.querySelector('.device-controls');
    assert.equal(controls.open,false);
    assert.equal(controls.querySelector('summary').textContent,'Device controls');
    assert.equal(card.querySelectorAll(':scope > .control-row').length,0);
    assert.ok(controls.querySelectorAll('[data-command]').length>0);
  }
  assert.equal(cards(test)[1].querySelector('[data-command=mode]').value,'1');
  assert.equal(cards(test)[1].querySelector('[data-command=run_time]'),null);
  assert.ok(cards(test)[0].querySelector('[data-command=run_time]'));
  assert.ok(cards(test)[0].querySelector('details').textContent.includes('0x0004 · 0x8123 · aabb · 5s'));
  const identity = cards(test)[0].querySelector('.device-details > .details').textContent;
  assert.ok(identity.includes('Manufacturer: Steinel GmbH'));
  assert.ok(identity.includes('Company ID: 0x0563 (Steinel GmbH)'));
  assert.ok(identity.includes('Product ID: 0x1de0'));
  assert.ok(!identity.includes('UUID') && !identity.includes('VID'));
  assert.ok(cards(test)[0].querySelector('details').textContent.includes('0102030405060708… · 200s · —'));
  nodes[0].verified=false;nodes[0].controls_allowed=true;
  await test.window.poll();
  assert.ok(!cards(test)[0].querySelector('[data-command=output]').disabled);
  assert.ok(cards(test)[0].querySelector('p').textContent.includes('Models imported from backup'));
  nodes[0].controls_allowed=false;nodes[0].composition_checked=true;
  await test.window.poll();
  assert.ok(cards(test)[0].querySelector('[data-command=output]').disabled);
  assert.ok(cards(test)[0].querySelector('p').textContent.includes('composition mismatch'));
  nodes[0].verified=true;nodes[0].controls_allowed=true;
  const checkbox = cards(test)[0].querySelector('input[type=checkbox]');
  checkbox.checked=false; checkbox.dispatchEvent(new test.window.Event('change'));
  cards(test)[0].querySelector('details').open=true;
  cards(test)[0].querySelector('.device-controls').open=true;
  for (const [lang,title,output,details,controls] of [
    ['pl','Brama Steinel Mesh','Wyjście','Szczegóły urządzenia','Sterowanie urządzeniem'],
    ['de','Steinel-Mesh-Gateway','Ausgang','Gerätedetails','Gerätesteuerung'],
    ['fr','Passerelle Steinel Mesh','Sortie','Détails de l’appareil','Commandes de l’appareil'],
    ['en','Steinel Mesh Gateway','Output','Device details','Device controls']]) {
    choose(test,lang); await wait(25);
    assert.equal(test.document.documentElement.lang,lang);
    assert.equal(test.document.title,title);
    assert.equal(test.window.localStorage.getItem('steinel.mesh.language'),lang);
    assert.equal(cards(test)[0].querySelector('.control-row label').textContent,output);
    assert.equal(cards(test)[0].querySelector('summary').textContent,details);
    assert.equal(cards(test)[0].querySelector('.device-controls summary').textContent,controls);
    assert.equal(cards(test)[0].querySelector('.device-controls').open,true);
    assert.equal(cards(test)[1].querySelector('.device-controls').open,false);
    assert.ok(cards(test)[0].querySelector('label').textContent.includes('Motion'));
    assert.equal(test.document.getElementById('networkName').textContent,'Motion');
    assert.equal(cards(test)[1].querySelector('[data-command=mode]').value,'1');
    await test.window.poll(); await wait(25);
    assert.equal(cards(test)[0].querySelector('input[type=checkbox]'),checkbox);
    assert.equal(checkbox.checked,false);
    assert.equal(test.errors.length,0,test.errors.map(String).join('\n'));
  }
  // Discovery can rebuild cards while the user is editing selection.
  nodes[0].functions=63;
  await test.window.poll(); await wait(25);
  assert.equal(cards(test)[0].querySelector('input[type=checkbox]').checked,false);
  assert.equal(cards(test)[0].querySelector('details').open,true);
  assert.equal(cards(test)[0].querySelector('.device-controls').open,true);
  assert.equal(cards(test)[1].querySelector('.device-controls').open,false);
  cards(test)[0].querySelector('.device-controls').open=false;
  assert.ok(!cards(test)[0].querySelector('p').textContent.includes('Motion:'));
  nodes[0].firmware='1.2.4';
  await test.window.poll(); await wait(25);
  assert.ok(cards(test)[0].querySelector('details').textContent.includes('1.2.4'));
  assert.equal(cards(test)[0].querySelector('.device-controls').open,false);
  choose(test,'fr'); await wait(25);
  assert.equal(test.window.steinelTranslate('Motion: no'),'Mouvement : non');
  const mode=cards(test)[1].querySelector('[data-command=mode]');
  mode.value='2'; mode.dispatchEvent(new test.window.Event('change')); await wait(30);
  const command=test.calls.find(call=>call.url==='/steinel/node');
  assert.equal(command.options.body.get('command'),'mode');
  assert.equal(command.options.body.get('value'),'2');
  assert.equal(command.options.body.get('address'),'10');
  status.mesh_ready=false;
  await test.window.poll(); await wait(25);
  for (const field of test.document.querySelectorAll('#deviceList [data-command]')) assert.ok(field.disabled);
  status.mesh_ready=true;
  test.dom.window.close();
  for (const saved of ['pl','de','fr','en','invalid']) {
    const test=await boot(saved);
    assert.equal(test.document.documentElement.lang,saved==='invalid'?'en':saved);
    assert.equal(test.errors.length,0); test.dom.window.close();
  }
  const blocked=await boot(null,true); choose(blocked,'fr');
  assert.equal(blocked.document.documentElement.lang,'fr');
  assert.equal(blocked.errors.length,0); blocked.dom.window.close();
  const savedStatus=structuredClone(status),savedNodes=structuredClone(nodes);
  status.enabled=false;status.mesh_ready=false;
  const extra=(address,name,company)=>({address,name,company,product:0,
    capabilities:8,functions:0,selected:false,available:false,verified:false});
  nodes.push(extra(30,'iPhone',0x004C),extra(31,'Steinel-looking name',0x1234),
    extra(35,'Missing manufacturer',0),extra(36,'Unset manufacturer',0xFFFF),extra(37,'Absent manufacturer'));
  nodes[3].selected=true;nodes[3].capabilities=15;
  const imported=await boot();
  const importOptions=imported.document.getElementById('networkImport');
  const unidentified=imported.document.getElementById('unrecognizedDevices');
  const unidentifiedCards=()=>[...imported.document.querySelectorAll('#unrecognizedDeviceList > .maintenance-section')];
  assert.equal(importOptions.open,false);
  assert.equal(importOptions.querySelector('summary').textContent,'Change or reimport network');
  assert.equal(cards(imported).length,3);
  assert.equal(unidentifiedCards().length,3);
  assert.equal(unidentified.open,false);
  assert.ok(!imported.document.getElementById('devicesPanel').textContent.includes('iPhone'));
  assert.ok(!imported.document.getElementById('devicesPanel').textContent.includes('Steinel-looking name'));
  assert.ok(cards(imported)[2].querySelector('label').textContent.includes('0x0014'));
  assert.ok(!imported.calls.some(call=>call.options?.method==='POST'));
  importOptions.open=true;unidentified.open=true;
  await imported.window.poll();await wait(25);
  assert.equal(importOptions.open,true);assert.equal(unidentified.open,true);
  const unidentifiedCheckbox=unidentifiedCards()[0].querySelector('input[type=checkbox]');
  unidentifiedCheckbox.checked=true;unidentifiedCheckbox.dispatchEvent(new imported.window.Event('change'));
  imported.document.getElementById('saveSelection').click();await wait(25);
  const selection=imported.calls.find(call=>call.url==='/steinel/selection');
  assert.equal(selection.options.body.get('addresses'),'2,10,35');
  nodes[5].company=0x0563;
  await imported.window.poll();await wait(25);
  assert.equal(cards(imported).length,4);assert.equal(unidentifiedCards().length,2);
  assert.equal(cards(imported)[3].querySelector('input[type=checkbox]').checked,true);
  nodes[5].company=0x1234;
  await imported.window.poll();await wait(25);
  assert.equal(cards(imported).length,3);
  assert.ok(!imported.document.getElementById('devicesPanel').textContent.includes('Missing manufacturer'));
  for(const [language,importLabel,unknownLabel] of [
    ['pl','Zmień lub ponownie importuj sieć','Nierozpoznane urządzenia'],
    ['de','Netzwerk ändern oder erneut importieren','Nicht erkannte Geräte'],
    ['fr','Modifier ou réimporter le réseau','Appareils non identifiés'],
    ['en','Change or reimport network','Unrecognized devices']]){
    choose(imported,language);await wait(20);
    assert.equal(importOptions.querySelector('summary').textContent,importLabel);
    assert.equal(unidentified.querySelector('summary').textContent,unknownLabel);
  }
  status.enabled=true;status.mesh_ready=true;
  await imported.window.poll();await wait(25);
  assert.equal(importOptions.open,true);
  assert.ok(!imported.document.getElementById('importMeshHelp').classList.contains('hidden'));
  for(const id of ['setup','installPanel','localBackup'])assert.ok(imported.document.getElementById(id).classList.contains('hidden'));
  status.enabled=false;status.mesh_ready=false;status.configured=false;
  status.networks=[{name:'Home',nodes:2,last_update:'Today',id:'test-network'}];
  await imported.window.poll();await wait(25);
  assert.equal(importOptions.open,true);
  assert.equal(importOptions.querySelector('summary').textContent,'Set up a Mesh network');
  for(const id of ['setup','installPanel','localBackup'])assert.ok(!imported.document.getElementById(id).classList.contains('hidden'));
  status.configured=true;status.networks=[];
  await imported.window.poll();await wait(25);
  assert.equal(importOptions.open,false);
  nodes.splice(0,nodes.length,extra(30,'Other manufacturer',0x004C));
  await imported.window.poll();await wait(25);
  assert.equal(cards(imported).length,0);
  assert.ok(!imported.document.getElementById('noSteinelDevices').classList.contains('hidden'));
  assert.equal(imported.document.getElementById('saveSelection').disabled,true);
  assert.ok(unidentified.classList.contains('hidden'));
  assert.equal(imported.errors.length,0,imported.errors.map(String).join('\n'));
  imported.dom.window.close();Object.assign(status,savedStatus);nodes.splice(0,nodes.length,...savedNodes);
  const reportTest=await boot();
  const unknown=cards(reportTest)[2];unknown.querySelector('[data-report]').click();
  assert.ok(!reportTest.document.getElementById('deviceReportPanel').classList.contains('hidden'));
  reportTest.document.getElementById('reportStart').click();await wait(30);
  assert.ok(!reportTest.calls.some(call=>call.url==='/steinel/diagnostics'));
  reportTest.document.getElementById('reportModel').value='L 830 SC';
  reportTest.document.getElementById('reportStart').click();await wait(40);
  const start=reportTest.calls.find(call=>call.url==='/steinel/diagnostics');
  assert.equal(start.options.body.get('address'),'20');
  assert.equal(nodes[2].selected,false);
  assert.equal(reportTest.document.getElementById('reportModel').disabled,true);
  reportTest.document.getElementById('reportPhase').value='covered';reportTest.document.getElementById('reportMark').click();
  diagnosticSnapshot.sequence=10;
  diagnosticSnapshot.events=[{sequence:10,elapsed_ms:1900,element:0,event:2,opcode:0x52,property:0,error:0,length:2,truncated:false,raw:'CCDD'}];
  await reportTest.window.readDeviceReport();
  reportTest.document.getElementById('reportDownload').click();await wait(40);
  assert.equal(diagnosticSnapshot.running,false);
  assert.equal(reportTest.blobs.length,1);
  const reportText=reportTest.blobs[0].parts.join(''),report=JSON.parse(reportText);
  assert.equal(report.schema,'steinel-device-report/1');assert.equal(report.device.label_model,'L 830 SC');
  assert.equal(report.device.models.product,0x1E74);
  assert.equal(report.device.models.elements[0].sig_models[0],0x1100);
  assert.equal(report.device.models.elements[0].vendor_models[0].model,0x8123);
  assert.equal(report.missed_events,7);assert.equal(report.events.length,3);
  assert.equal(report.observations[1].condition,'covered');
  assert.ok(!reportText.includes('PRIVATE_')&&!reportText.includes('"address"')&&!reportText.includes('12345'));
  assert.ok(!reportTest.downloads[0].includes('L 830')&&!reportTest.downloads[0].includes('img'));
  assert.ok(!reportTest.calls.some(call=>call.url==='/steinel/selection'||call.url==='/steinel/node'));
  const snapshot=structuredClone(diagnosticSnapshot);
  for(let batch=0;batch<70;batch++){
    snapshot.events=Array.from({length:8},(_,index)=>({...diagnosticSnapshot.events[0],sequence:11+batch*8+index}));
    snapshot.sequence=snapshot.events[7].sequence;reportTest.window.collectReportSnapshot(snapshot);
  }
  reportTest.document.getElementById('reportDownload').click();await wait(30);
  const limited=JSON.parse(reportTest.blobs[1].parts.join(''));
  assert.equal(limited.events.length,512);assert.equal(limited.event_limit_reached,true);
  reportTest.document.getElementById('reportModel').value='L 830 C';
  unknown.querySelector('[data-report]').click();await wait(10);
  assert.equal(reportTest.document.getElementById('reportModel').value,'L 830 C');
  reportTest.document.getElementById('reportDownload').click();await wait(20);
  assert.equal(JSON.parse(reportTest.blobs[2].parts.join('')).device.label_model,'L 830 C');
  reportTest.window.confirm=()=>false;
  cards(reportTest)[0].querySelector('[data-report]').click();await wait(10);
  assert.equal(reportTest.document.getElementById('reportModel').value,'L 830 C');
  assert.equal(reportTest.document.getElementById('reportDownload').disabled,false);
  reportTest.window.confirm=()=>true;
  cards(reportTest)[0].querySelector('[data-report]').click();await wait(10);
  assert.equal(reportTest.document.getElementById('reportModel').value,'IS 180');
  assert.equal(reportTest.document.getElementById('reportDownload').disabled,true);
  reportTest.document.getElementById('reportStart').click();await wait(40);
  assert.equal(reportTest.document.getElementById('reportDownload').disabled,false);
  const fetchOnline=reportTest.window.fetch;
  reportTest.window.fetch=async(url,options)=>{
    if(url.startsWith('/api/diagnostics')||url==='/steinel/diagnostics')throw Error('Network unavailable');
    return fetchOnline(url,options);
  };
  await reportTest.window.readDeviceReport();
  reportTest.document.getElementById('reportDownload').click();await wait(30);
  const offline=JSON.parse(reportTest.blobs[3].parts.join(''));
  assert.equal(offline.device.label_model,'IS 180');
  assert.equal(offline.collection_stop_unconfirmed,true);assert.equal(offline.last_read_failed,true);
  assert.ok(offline.events.length>0);
  assert.equal(reportTest.document.getElementById('reportModel').disabled,false);
  assert.ok(reportTest.document.getElementById('reportStatus').textContent.includes('Stop not confirmed'));
  reportTest.window.fetch=async(url,options)=>{
    if(url.startsWith('/api/diagnostics'))return new Promise((resolve,reject)=>{
      options.signal.addEventListener('abort',()=>{const error=Error('Aborted');error.name='AbortError';reject(error)});
    });
    return fetchOnline(url,options);
  };
  await reportTest.window.readDeviceReport();
  assert.equal(reportTest.document.getElementById('reportStatus').textContent,'Diagnostics connection timed out');
  reportTest.window.fetch=fetchOnline;
  reportTest.window.fetch=async(url,options)=>{
    if(url==='/steinel/status')throw Error('Gateway connection timed out');
    return fetchOnline(url,options);
  };
  await reportTest.window.poll();
  assert.equal(reportTest.document.getElementById('message').textContent,'Gateway connection timed out');
  assert.equal(reportTest.document.getElementById('state').textContent,'Connection to gateway unavailable');
  assert.equal(reportTest.document.getElementById('dot').className,'dot bad');
  reportTest.window.fetch=fetchOnline;
  await reportTest.window.poll();
  assert.equal(reportTest.document.getElementById('state').textContent,'Bluetooth Mesh ready');
  for(const [language,label] of [['pl','Przygotuj raport urządzenia'],['de','Gerätebericht erstellen'],['fr','Préparer le rapport de l’appareil']]){
    choose(reportTest,language);await wait(20);assert.equal(cards(reportTest)[2].querySelector('[data-report]').textContent,label);
  }
  assert.equal(reportTest.errors.length,0,reportTest.errors.map(String).join('\n'));
  reportTest.dom.window.close();
  console.log('Web tests passed: manufacturer filtering, unidentified entries, first import/reimport states, device cards, four languages, state, reports, privacy, bounded history, target changes, offline export and request timeouts.');
})().catch(error=>{console.error(error);process.exitCode=1;});
