'use strict';

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const {URL, URLSearchParams} = require('node:url');

const root = path.resolve(__dirname, '..');
const component = path.join(root, 'esphome/components/steinel_mesh');
const fixture = JSON.parse(fs.readFileSync(path.join(__dirname, 'gateway-fixture.json'), 'utf8'));
const initial = () => structuredClone(fixture);
let state = initial();
const port = Number(process.env.STEINEL_DEMO_PORT || 8765);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw Error('Invalid demo port');

const demoScript = `<script>
(() => {
  const realFetch = window.fetch.bind(window);
  window.fetch = (input, options) => {
    const url = new URL(typeof input === 'string' ? input : input.url, location.href);
    if (url.origin !== location.origin) {
      if (url.href === 'https://api.github.com/repos/supczinskib/steinel-mesh-gateway/releases/latest')
        return realFetch('/demo/release', {cache: 'no-store'});
      return Promise.reject(Error('External connections are disabled in this local demo.'));
    }
    return realFetch(input, options);
  };
})();
</script>`;

function page() {
  const html = fs.readFileSync(path.join(component, 'steinel_page.html'), 'utf8');
  const translations = fs.readFileSync(path.join(component, 'steinel_i18n.js'), 'utf8');
  return html.replace('<!-- STEINEL_I18N -->', demoScript + '<script>\n' + translations + '\n</script>');
}

function respond(response, status, value, type = 'application/json; charset=utf-8') {
  response.writeHead(status, {
    'Content-Type': type,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    'Content-Security-Policy': "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:; form-action 'none'; frame-ancestors 'none'; base-uri 'none'"
  });
  response.end(type.startsWith('application/json') ? JSON.stringify(value) : value);
}

async function readBody(request) {
  let body = '';
  for await (const chunk of request) {
    body += chunk.toString('utf8');
    if (body.length > 8192) throw Error('Request too large');
  }
  return new URLSearchParams(body);
}

const commands = {mode: [128, 2], output: [1, 1], brightness: [2, 65535],
  automatic: [4, 1], threshold: [8, 16777214], run_time: [16, 16777214]};

const server = http.createServer(async (request, response) => {
  try {
    if (request.headers.host !== `127.0.0.1:${port}` && request.headers.host !== `localhost:${port}`)
      return respond(response, 403, {message: 'Loopback access only'});
    if (request.headers.origin && ![`http://127.0.0.1:${port}`, `http://localhost:${port}`].includes(request.headers.origin))
      return respond(response, 403, {message: 'Cross-origin request rejected'});
    const url = new URL(request.url, `http://127.0.0.1:${port}`);
    if (request.method === 'GET') {
      if (url.pathname === '/') return respond(response, 200, page(), 'text/html; charset=utf-8');
      if (url.pathname === '/steinel/status') return respond(response, 200, state.status);
      if (url.pathname === '/api/nodes') return respond(response, 200, {nodes: state.nodes});
      if (url.pathname === '/demo/release') return respond(response, 200, {tag_name: 'v2.0.0'});
      if (url.pathname === '/api/diagnostics') return respond(response, 409, {message: 'Device reports require real hardware and are not collected in this demo.'});
      return respond(response, 404, {message: 'Not found'});
    }
    if (request.method !== 'POST') return respond(response, 405, {message: 'Method not allowed'});
    if (url.pathname === '/demo/reset') {
      state = initial();
      return respond(response, 200, {message: 'Demo reset'});
    }
    const params = await readBody(request);
    if (url.pathname === '/steinel/node') {
      const address = Number(params.get('address')), command = params.get('command');
      const value = params.get('value') === null ? NaN : Number(params.get('value'));
      const node = state.nodes.find(item => item.address === address);
      const spec = commands[command];
      if (!node || !spec || !Number.isInteger(value) || value < 0 || value > spec[1] || !(node.functions & spec[0]))
        return respond(response, 400, {message: 'Invalid demo command'});
      if (!node.selected || !state.status.enabled) return respond(response, 409, {message: 'Select and enable the device first'});
      node[command] = value;
      if (command === 'mode') {
        node.automatic = value === 0 ? 1 : 0;
        node.output = value === 1 ? 1 : value === 2 ? 0 : node.output;
      }
      if (command === 'brightness') node.output = value > 0 ? 1 : 0;
      return respond(response, 200, {message: 'Device command applied'});
    }
    if (url.pathname === '/steinel/selection') {
      const selected = new Set((params.get('addresses') || '').split(',').filter(Boolean).map(Number));
      if ([...selected].some(address => !state.nodes.some(node => node.address === address)))
        return respond(response, 400, {message: 'Unknown demo address'});
      state.nodes.forEach(node => node.selected = selected.has(node.address));
      state.status.enabled = selected.size > 0;
      state.status.mesh_ready = state.status.enabled;
      return respond(response, 200, {message: 'Device selection saved'});
    }
    if (url.pathname === '/steinel/enable' || url.pathname === '/steinel/disable') {
      state.status.enabled = url.pathname.endsWith('/enable');
      state.status.mesh_ready = state.status.enabled;
      return respond(response, 200, {message: state.status.enabled ? 'Mesh enabled' : 'Mesh disabled'});
    }
    if (url.pathname === '/steinel/refresh') return respond(response, 200, {message: 'Device readings refreshed'});
    return respond(response, 403, {message: 'Import, device reports, passwords, Wi-Fi, reset and firmware operations require real hardware. Disabled in demo.'});
  } catch (error) {
    if (!response.headersSent) respond(response, 400, {message: error.message});
  }
});
server.listen(port, '127.0.0.1', () => console.log(`Steinel Mesh demo: http://127.0.0.1:${port}/`));
