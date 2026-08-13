// OSCRouter web interface.
//
// Dependency free and served from the daemon's compiled-in resources. Every URL
// is relative, so the page works unchanged whether it is reached directly or
// through a Home Assistant ingress prefix.

'use strict';

var PROTOCOLS = ['OSC', 'PSN', 'sACN', 'Art-Net', 'MIDI', 'OTP'];
var FRAME_MODES = ['1.0 (size header)', '1.1 (SLIP)'];
var OTP_MODULES = ['Position', 'Position Velocity/Acceleration', 'Rotation', 'Rotation Velocity/Acceleration', 'Scale', 'Reference Frame'];

var config = { routes: [], connections: [], settings: { otpModules: [] }, muteAllIncoming: false, muteAllOutgoing: false };
var itemStates = [];

// ------------------------------------------------------------------ helpers

function el(id) { return document.getElementById(id); }

function api(path, options) {
  // Relative to the directory the page was served from, which is what makes
  // ingress work without knowing the prefix.
  return fetch(path.replace(/^\//, ''), options).then(function (response) {
    if (!response.ok) {
      return response.json().catch(function () { return {}; }).then(function (body) {
        throw new Error(body.error || (response.status + ' ' + response.statusText));
      });
    }
    var type = response.headers.get('content-type') || '';
    return type.indexOf('application/json') === 0 ? response.json() : response.text();
  });
}

function toast(message, isError) {
  var node = el('toast');
  node.textContent = message;
  node.className = 'toast show' + (isError ? ' error' : '');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(function () { node.className = 'toast'; }, 2600);
}

function makeInput(className, value, onChange, type) {
  var input = document.createElement('input');
  input.type = type || 'text';
  input.className = className;
  input.value = value === undefined || value === null ? '' : value;
  input.addEventListener('change', function () { onChange(input.value); });
  return input;
}

function makeSelect(options, selectedIndex, onChange) {
  var select = document.createElement('select');
  options.forEach(function (label, index) {
    var option = document.createElement('option');
    option.value = index;
    option.textContent = label;
    select.appendChild(option);
  });
  select.value = selectedIndex;
  select.addEventListener('change', function () { onChange(parseInt(select.value, 10)); });
  return select;
}

function makeCheckbox(checked, onChange) {
  var input = document.createElement('input');
  input.type = 'checkbox';
  input.checked = !!checked;
  input.addEventListener('change', function () { onChange(input.checked); });
  return input;
}

function cell(row, child) {
  var td = document.createElement('td');
  if (child) td.appendChild(child);
  row.appendChild(td);
  return td;
}

// ------------------------------------------------------------------ routing

function renderRoutes() {
  var body = el('routesBody');
  body.textContent = '';

  config.routes.forEach(function (route, index) {
    var row = document.createElement('tr');
    row.dataset.index = index;

    cell(row, makeCheckbox(route.enable, function (value) {
      route.enable = value;
      // Changing which routes exist requires the engine to be rebuilt, so this
      // is applied immediately rather than waiting for Save & Apply.
      api('api/routes/' + index + '/enable', postJson({ value: value }))
        .then(function () { toast(value ? 'Route enabled' : 'Route disabled'); })
        .catch(function (e) { toast(e.message, true); });
    }));

    cell(row, makeCheckbox(route.mute, function (value) {
      route.mute = value;
      // Muting takes effect without a restart.
      api('api/routes/' + index + '/mute', postJson({ value: value }))
        .catch(function (e) { toast(e.message, true); });
    }));

    cell(row, makeInput('label', route.label, function (v) { route.label = v; }));

    // Incoming: live indicator, then the fields.
    var srcIndicator = document.createElement('span');
    srcIndicator.className = 'row-indicator';
    srcIndicator.dataset.role = 'src';
    var srcCell = cell(row);
    srcCell.appendChild(srcIndicator);
    srcCell.colSpan = 2;

    cell(row, makeInput('ip', route.src.ip, function (v) { route.src.ip = v; }));
    cell(row, makeInput('port', route.src.port, function (v) { route.src.port = parseInt(v, 10) || 0; }, 'number'));
    cell(row, makeInput('path', route.src.path, function (v) { route.src.path = v; }));
    cell(row, makeSelect(PROTOCOLS, route.src.protocol, function (v) { route.src.protocol = v; }));
    cell(row, makeInput('transform', route.inMin.value, function (v) { route.inMin.value = v; }));
    cell(row, makeInput('transform', route.inMax.value, function (v) { route.inMax.value = v; }));

    var dstIndicator = document.createElement('span');
    dstIndicator.className = 'row-indicator';
    dstIndicator.dataset.role = 'dst';
    var dstCell = cell(row);
    dstCell.appendChild(dstIndicator);
    dstCell.colSpan = 2;

    cell(row, makeInput('ip', route.dst.ip, function (v) { route.dst.ip = v; }));
    cell(row, makeInput('port', route.dst.port, function (v) { route.dst.port = parseInt(v, 10) || 0; }, 'number'));
    cell(row, makeInput('path', route.dst.path, function (v) { route.dst.path = v; }));
    cell(row, makeSelect(PROTOCOLS, route.dst.protocol, function (v) { route.dst.protocol = v; }));
    cell(row, makeInput('transform', route.outMin.value, function (v) { route.outMin.value = v; }));
    cell(row, makeInput('transform', route.outMax.value, function (v) { route.outMax.value = v; }));

    var scriptBtn = document.createElement('button');
    scriptBtn.className = 'btn small script-btn' + (route.dst.scriptText ? ' has-script' : '');
    scriptBtn.textContent = route.dst.scriptText ? 'Script ✓' : 'Script';
    scriptBtn.addEventListener('click', function () {
      var text = prompt('JavaScript for this route.\nThe incoming OSC address is in OSC; assign to it to rewrite.', route.dst.scriptText || '');
      if (text === null) return;
      route.dst.scriptText = text;
      route.dst.script = text.length > 0;
      renderRoutes();
    });
    cell(row, scriptBtn);

    var removeBtn = document.createElement('button');
    removeBtn.className = 'btn small danger';
    removeBtn.textContent = '✕';
    removeBtn.title = 'Remove route';
    removeBtn.addEventListener('click', function () {
      config.routes.splice(index, 1);
      renderRoutes();
    });
    cell(row, removeBtn);

    body.appendChild(row);
  });

  applyItemStates();
}

function emptyRoute() {
  return {
    label: '', enable: true, mute: false,
    src: { ip: '', port: 0, path: '', protocol: 0, multicastInterfaceIP: '' },
    dst: { ip: '', port: 0, path: '', protocol: 0, multicastInterfaceIP: '', script: false, scriptText: '' },
    inMin: { value: '' }, inMax: { value: '' }, outMin: { value: '' }, outMax: { value: '' }
  };
}

// -------------------------------------------------------------------- tcp

function renderConnections() {
  var body = el('tcpBody');
  body.textContent = '';

  config.connections.forEach(function (connection, index) {
    var row = document.createElement('tr');

    cell(row, makeInput('label', connection.label, function (v) { connection.label = v; }));
    cell(row, makeSelect(['Server', 'Client'], connection.server ? 0 : 1, function (v) { connection.server = (v === 0); }));
    cell(row, makeSelect(FRAME_MODES, connection.frameMode, function (v) { connection.frameMode = v; }));
    cell(row, makeInput('ip', connection.ip, function (v) { connection.ip = v; }));
    cell(row, makeInput('port', connection.port, function (v) { connection.port = parseInt(v, 10) || 0; }, 'number'));

    var removeBtn = document.createElement('button');
    removeBtn.className = 'btn small danger';
    removeBtn.textContent = '✕';
    removeBtn.addEventListener('click', function () {
      config.connections.splice(index, 1);
      renderConnections();
    });
    cell(row, removeBtn);

    body.appendChild(row);
  });
}

// --------------------------------------------------------------- settings

function renderSettings() {
  var settings = config.settings || {};
  el('sACNIP').value = settings.sACNIP || '';
  el('artNetIP').value = settings.artNetIP || '';
  el('otpIP').value = settings.otpIP || '';
  el('levelChangesOnly').checked = !!settings.levelChangesOnly;
  el('globalScript').value = settings.script || '';

  var container = el('otpModules');
  container.textContent = '';
  OTP_MODULES.forEach(function (name, index) {
    var label = document.createElement('label');
    var input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = !!(settings.otpModules && settings.otpModules[index]);
    input.addEventListener('change', function () {
      if (!settings.otpModules) settings.otpModules = [];
      settings.otpModules[index] = input.checked;
    });
    label.appendChild(input);
    label.appendChild(document.createTextNode(' ' + name));
    container.appendChild(label);
  });

  el('muteAllIncoming').checked = !!config.muteAllIncoming;
  el('muteAllOutgoing').checked = !!config.muteAllOutgoing;
}

function collectSettings() {
  var settings = config.settings || (config.settings = {});
  settings.sACNIP = el('sACNIP').value;
  settings.artNetIP = el('artNetIP').value;
  settings.otpIP = el('otpIP').value;
  settings.levelChangesOnly = el('levelChangesOnly').checked;
  settings.script = el('globalScript').value;
}

// ----------------------------------------------------------- live updates

function applyItemStates() {
  var rows = el('routesBody').children;
  itemStates.forEach(function (state) {
    var row = rows[state.index];
    if (!row) return;

    var src = row.querySelector('[data-role="src"]');
    if (src) src.className = 'row-indicator ' + (state.srcState || '') + (state.srcActivity ? ' activity' : '');

    var dst = row.querySelector('[data-role="dst"]');
    if (dst) dst.className = 'row-indicator ' + (state.dstState || '') + (state.dstActivity ? ' activity' : '');
  });
}

function applyStatus(status) {
  el('runIndicator').className = 'indicator ' + (status.running ? 'connected' : 'notConnected');
  el('runLabel').textContent = status.running ? 'Running' : 'Stopped';
  el('startBtn').disabled = status.running;
  el('stopBtn').disabled = !status.running;
}

var MAX_LOG_LINES = 500;

function appendLog(message) {
  var body = el('logBody');

  var line = document.createElement('div');
  line.className = 'log-line log-' + message.type;

  var time = document.createElement('span');
  time.className = 'time';
  time.textContent = new Date(message.timestamp * 1000).toLocaleTimeString();
  line.appendChild(time);
  line.appendChild(document.createTextNode(message.text));

  body.appendChild(line);

  while (body.children.length > MAX_LOG_LINES)
    body.removeChild(body.firstChild);

  if (el('autoScroll').checked)
    body.scrollTop = body.scrollHeight;
}

function connectEvents() {
  var source = new EventSource('api/events');

  source.addEventListener('log', function (e) { appendLog(JSON.parse(e.data)); });
  source.addEventListener('itemStates', function (e) {
    itemStates = JSON.parse(e.data);
    applyItemStates();
  });
  source.addEventListener('status', function (e) { applyStatus(JSON.parse(e.data)); });

  // EventSource reconnects on its own; just reflect the outage in the header.
  source.onerror = function () {
    el('runIndicator').className = 'indicator';
    el('runLabel').textContent = 'disconnected';
  };
}

// ---------------------------------------------------------------- actions

function postJson(body) {
  return { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) };
}

function loadConfig() {
  return api('api/config').then(function (data) {
    config = data;
    renderRoutes();
    renderConnections();
    renderSettings();
  });
}

function saveAndApply() {
  collectSettings();
  config.muteAllIncoming = el('muteAllIncoming').checked;
  config.muteAllOutgoing = el('muteAllOutgoing').checked;

  api('api/config', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(config) })
    .then(function () { return api('api/config/apply', postJson({})); })
    .then(function () { toast('Saved and restarted'); return loadConfig(); })
    .catch(function (e) { toast(e.message, true); });
}

// ------------------------------------------------------------------- init

function initTabs() {
  var tabs = document.querySelectorAll('.tab');
  Array.prototype.forEach.call(tabs, function (tab) {
    tab.addEventListener('click', function () {
      Array.prototype.forEach.call(tabs, function (t) { t.classList.remove('active'); });
      tab.classList.add('active');

      Array.prototype.forEach.call(document.querySelectorAll('.panel'), function (panel) {
        panel.classList.remove('active');
      });
      el('panel-' + tab.dataset.panel).classList.add('active');

      if (tab.dataset.panel === 'file')
        api('api/file').then(function (text) { el('rawFile').value = text; });
    });
  });
}

function initActions() {
  el('addRoute').addEventListener('click', function () {
    config.routes.push(emptyRoute());
    renderRoutes();
  });

  el('addTcp').addEventListener('click', function () {
    config.connections.push({ label: '', server: true, frameMode: 1, ip: '', port: 0 });
    renderConnections();
  });

  el('applyBtn').addEventListener('click', saveAndApply);

  el('startBtn').addEventListener('click', function () {
    api('api/start', postJson({})).then(function () { toast('Started'); }).catch(function (e) { toast(e.message, true); });
  });

  el('stopBtn').addEventListener('click', function () {
    api('api/stop', postJson({})).then(function () { toast('Stopped'); }).catch(function (e) { toast(e.message, true); });
  });

  function muteAll() {
    api('api/mute-all', postJson({ incoming: el('muteAllIncoming').checked, outgoing: el('muteAllOutgoing').checked }))
      .catch(function (e) { toast(e.message, true); });
  }
  el('muteAllIncoming').addEventListener('change', muteAll);
  el('muteAllOutgoing').addEventListener('change', muteAll);

  el('clearLog').addEventListener('click', function () { el('logBody').textContent = ''; });

  el('saveFile').addEventListener('click', function () {
    api('api/file', { method: 'PUT', headers: { 'Content-Type': 'text/plain' }, body: el('rawFile').value })
      .then(function () { toast('File saved'); return loadConfig(); })
      .catch(function (e) { toast(e.message, true); });
  });
}

function initInterfaces() {
  api('api/interfaces').then(function (interfaces) {
    var list = el('interfaces');
    list.textContent = '';
    interfaces.forEach(function (iface) {
      var option = document.createElement('option');
      option.value = iface.ip;
      option.textContent = iface.ip + ' (' + iface.name + ')';
      list.appendChild(option);
    });
  }).catch(function () { /* the pickers still accept free text */ });
}

initTabs();
initActions();
initInterfaces();
loadConfig().catch(function (e) { toast(e.message, true); });
connectEvents();
