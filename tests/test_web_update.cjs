const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM, VirtualConsole} = require('jsdom');

const root = path.resolve(__dirname, '..');
const component = path.join(root, 'esphome/components/steinel_mesh');
const html = fs.readFileSync(path.join(component, 'steinel_page.html'), 'utf8').replace(
  '<!-- STEINEL_I18N -->', '<script>' + fs.readFileSync(path.join(component, 'steinel_i18n.js'), 'utf8') + '</script>');
const web = fs.readFileSync(path.join(component, 'steinel_web.cpp'), 'utf8');
const packager = fs.readFileSync(path.join(root, 'scripts/10_prepare_release.sh'), 'utf8');
const repository = 'supczinskib/steinel-mesh-gateway';
const api = `https://api.github.com/repos/${repository}/releases/latest`;
const downloadPrefix = web.match(/RELEASE_DOWNLOAD_PREFIX\s*=\s*"([^"]+)"/)[1];
const assetPrefix = web.match(/RELEASE_ASSET_PREFIX\s*=\s*"([^"]+)"/)[1];
assert.equal(downloadPrefix, `https://github.com/${repository}/releases/download/v`);
assert.equal(assetPrefix, 'steinel-mesh-esp32-c3-gateway-v');
assert.ok(packager.includes('BASE_NAME="' + assetPrefix + '$VERSION"'));
assert.ok(!packager.includes('LEGACY_NAME'));
assert.equal((packager.match(/^cp /gm) || []).length, 2);
for (const file of ['README.md', 'README_PL.md', 'README_DE.md', 'README_FR.md']) {
  const text = fs.readFileSync(path.join(root, file), 'utf8');
  assert.ok(text.includes(`https://github.com/${repository}/releases/latest`), file);
}
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

async function boot(release) {
  const errors = [], calls = [];
  const console = new VirtualConsole();
  console.on('jsdomError', error => errors.push(error));
  const dom = new JSDOM(html, {url:'http://gateway.test/', runScripts:'dangerously', virtualConsole:console,
    beforeParse(window) {
      window.setInterval = () => 0;
      window.confirm = () => true;
      window.fetch = async (url, options = {}) => {
        calls.push({url, options});
        let data;
        if (url === '/steinel/status') data = {configured:true, enabled:true, mesh_ready:true,
          gateway_version:'1.9.2', networks:[], local_address:'7000'};
        else if (url === '/api/nodes') data = {nodes:[]};
        else if (url === api) data = release;
        else if (url === '/steinel/update') data = {message:'Update scheduled'};
        else throw Error('Unexpected request: ' + url);
        return {ok:true, status:200, json:async () => structuredClone(data)};
      };
    }});
  await wait(60);
  return {dom, calls, errors, document:dom.window.document};
}

(async () => {
  const version = '2.0.0';
  const asset = {name:`${assetPrefix}${version}-ota.bin`, size:1555000,
    digest:'sha256:' + 'ab'.repeat(32),
    browser_download_url:`${downloadPrefix}${version}/${assetPrefix}${version}-ota.bin`};
  const release = {tag_name:`v${version}`, assets:[asset]};
  let t = await boot(release);
  assert.ok(t.calls.some(call => call.url === api));
  const install = t.document.getElementById('installAvailableUpdate');
  assert.equal(install.classList.contains('hidden'), false);
  assert.equal(t.document.getElementById('latestGatewayVersion').textContent, version);
  install.click();
  await wait(20);
  const request = t.calls.find(call => call.url === '/steinel/update');
  assert.ok(request);
  assert.equal(request.options.body.get('version'), version);
  assert.equal(request.options.body.get('url'), asset.browser_download_url);
  assert.equal(request.options.body.get('sha256'), asset.digest.slice(7));
  assert.equal(request.options.body.get('size'), String(asset.size));
  assert.equal(t.errors.length, 0);
  t.dom.window.close();

  for (const invalid of [
    {...asset, digest:''},
    {...asset, size:0},
    {...asset, name:`${assetPrefix}${version}-factory.bin`},
  ]) {
    t = await boot({...release, assets:[invalid]});
    assert.equal(t.document.getElementById('installAvailableUpdate').classList.contains('hidden'), true);
    assert.equal(t.calls.some(call => call.url === '/steinel/update'), false);
    assert.equal(t.errors.length, 0);
    t.dom.window.close();
  }
  t = await boot({...release, tag_name:'v1.9.2'});
  assert.equal(t.document.getElementById('installAvailableUpdate').classList.contains('hidden'), true);
  assert.match(t.document.getElementById('otaStatus').textContent, /up to date/);
  t.dom.window.close();
  console.log('Firmware updates: new repository, packaging contract, 1.9.2 → 2.0.0 request and invalid-asset rejection passed.');
})().catch(error => {console.error(error); process.exitCode = 1;});
