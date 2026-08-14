// OSCRouter web interface.
//
// Dependency free and served from the daemon's compiled-in resources. Every URL
// is relative, so the page works unchanged whether it is reached directly or
// through a Home Assistant ingress prefix.
//
// The desktop application presents routes as a twenty column table in which the
// meaning of most cells depends on a protocol chosen two cells away: "Port" is
// a UDP port, an sACN universe, an OTP system number or a MIDI port, and "Path"
// is a filter on one side and a template on the other. This interface shows one
// card per route instead, labels each field for the protocol actually selected,
// and hides the fields that protocol does not use.

'use strict';

// ------------------------------------------------------- protocol knowledge

// Everything that differs between protocols, in one place. The hints are the
// same facts the desktop application keeps in tooltips, which are hard to find
// and easy to miss.
var PROTOCOLS = [
  {
    name: 'OSC',
    hasIP: true,
    portLabel: 'Port',
    portPlaceholder: '8000',
    portRequired: true,
    srcPath: true,
    dstPath: true,
    srcPathLabel: 'Address filter',
    dstPathLabel: 'Outgoing address'
  },
  {
    name: 'PSN',
    hasIP: true,
    portLabel: 'Port',
    portPlaceholder: '56565',
    portRequired: true,
    srcPath: true,
    dstPath: true,
    srcPathLabel: 'Address filter',
    dstPathLabel: 'Outgoing address'
  },
  {
    name: 'sACN',
    hasIP: true,
    portLabel: 'Universe',
    portPlaceholder: '1',
    portRequired: true,
    portMin: 1,
    portMax: 63999,
    // sACN carries a whole universe of levels, not an addressed message, so
    // there is nothing to filter on the way in.
    srcPath: false,
    dstPath: true,
    dstPathLabel: 'Levels to send'
  },
  {
    name: 'Art-Net',
    hasIP: true,
    portLabel: 'Universe',
    portPlaceholder: '0',
    portRequired: false,
    portMin: 0,
    portMax: 32767,
    srcPath: false,
    dstPath: true,
    dstPathLabel: 'Levels to send'
  },
  {
    name: 'MIDI',
    hasIP: false,
    portLabel: 'MIDI port',
    portPlaceholder: '0',
    portRequired: false,
    srcPath: true,
    dstPath: true,
    srcPathLabel: 'Address filter',
    dstPathLabel: 'Outgoing address'
  },
  {
    name: 'OTP',
    hasIP: false,
    portLabel: 'System number',
    portPlaceholder: '1',
    portRequired: true,
    portMin: 0,
    portMax: 200,
    srcPath: true,
    dstPath: true,
    srcPathLabel: 'Address filter',
    dstPathLabel: 'Outgoing address'
  }
];

var FRAME_MODES = ['1.0 (size header)', '1.1 (SLIP)'];
var OTP_MODULES = ['Position', 'Position Velocity/Acceleration', 'Rotation', 'Rotation Velocity/Acceleration', 'Scale', 'Reference Frame'];

function protocolInfo(id) {
  return PROTOCOLS[id] || PROTOCOLS[0];
}

// Hint text for the port field, which is the field whose meaning changes most.
function portHint(id, isSrc) {
  switch (id) {
    case 2:
      return isSrc ? 'The sACN universe to listen to. Universes start at 1.' : 'The sACN universe to send on. Blank sends on the universe it arrived on.';
    case 3:
      return isSrc ? 'The Art-Net universe to listen to.' : 'The Art-Net universe to send on. Blank sends on the universe it arrived on.';
    case 4:
      return 'The MIDI port index. Available ports are listed in the routing log at startup.';
    case 5:
      return 'The OTP system number, 0 to 200.';
    default:
      return isSrc ? 'The UDP port to listen on.' : 'The port to send to. Blank sends back to the port it arrived on.';
  }
}

function ipHint(isSrc) {
  return isSrc
    ? 'Only accept packets sent from this address. Blank accepts them from anywhere.'
    : 'Where to send. Blank replies to whoever sent the packet.';
}

function pathHint(id, isSrc) {
  if (isSrc)
    return 'Only route messages matching this address. <code>*</code> matches anything, as in <code>/eos/out/event/*</code>. Blank routes everything.';

  if (id === 2 || id === 3)
    return 'Which levels to set, as <code>/sacn=%2,%3,%4</code> or <code>/sacn/offset/10=%2,%3</code>. See the examples below.';

  return 'The address to send. <code>%1</code>, <code>%2</code>… insert parts of the incoming message. See the examples below.';
}

// ------------------------------------------------------------------- state

var config = {routes: [], connections: [], settings: {otpModules: []}, muteAllIncoming: false, muteAllOutgoing: false};
var itemStates = [];
var issues = [];
// Not "status": a top level var of that name in a classic script aliases
// window.status, which silently coerces whatever it is given to a string.
var runStatus = {};
var dirty = false;
var filterText = '';
var nextUid = 1;
var openRoutes = {};  // uid -> true
var cardsByUid = {};

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

function postJson(body) {
  return {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)};
}

function toast(message, isError) {
  var node = el('toast');
  node.textContent = message;
  node.className = 'toast show' + (isError ? ' error' : '');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(function () { node.className = 'toast'; }, 2600);
}

function make(tag, className, text) {
  var node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

function makeInput(value, onChange, options) {
  options = options || {};
  var input = document.createElement('input');
  input.type = options.type || 'text';
  if (options.placeholder) input.placeholder = options.placeholder;
  if (options.className) input.className = options.className;
  input.value = value === undefined || value === null ? '' : value;
  input.addEventListener('input', function () { onChange(input.value); });
  return input;
}

function makeSelect(labels, selectedIndex, onChange) {
  var select = document.createElement('select');
  labels.forEach(function (label, index) {
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

// A labelled field with optional hint text below it. The hint may contain
// <code> markup, which is why it is assigned as HTML; every string passed here
// is a literal in this file, never anything from the configuration.
function field(labelText, control, hint, required) {
  var wrap = make('div', 'field');
  var label = make('label');
  label.appendChild(document.createTextNode(labelText));
  if (required) label.appendChild(make('span', 'required', ' *'));
  wrap.appendChild(label);
  wrap.appendChild(control);
  if (hint) {
    var p = make('p', 'field-hint');
    p.innerHTML = hint;
    wrap.appendChild(p);
  }
  return wrap;
}

function relativeTime(epochSeconds) {
  var s = Math.floor(Date.now() / 1000 - epochSeconds);
  if (s < 2) return 'just now';
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.floor(s / 60) + 'm ago';
  if (s < 86400) return Math.floor(s / 3600) + 'h ago';
  return Math.floor(s / 86400) + 'd ago';
}

// -------------------------------------------------------------- route model

function emptyRoute() {
  return {
    label: '', notes: '', enable: true, mute: false,
    src: {ip: '', port: 0, path: '', protocol: 0, multicastInterfaceIP: ''},
    dst: {ip: '', port: 0, path: '', protocol: 0, multicastInterfaceIP: '', script: false, scriptText: ''},
    inMin: {value: ''}, inMax: {value: ''}, outMin: {value: ''}, outMax: {value: ''}
  };
}

function adoptRoutes() {
  config.routes.forEach(function (route) {
    if (!route._uid) route._uid = nextUid++;
    if (route.notes === undefined) route.notes = '';
  });
}

function routeIndex(route) {
  return config.routes.indexOf(route);
}

// Mirrors MainWindow::refreshTcpBadges: an OSC endpoint whose address matches a
// defined connection travels over TCP rather than UDP. Nothing on the route
// itself says so, which makes it one of the easier things to forget.
function usesTcp(endpoint) {
  if (endpoint.protocol !== 0) return null;
  for (var i = 0; i < config.connections.length; i++) {
    var c = config.connections[i];
    if (c.ip === endpoint.ip && Number(c.port) === Number(endpoint.port))
      return c;
  }
  return null;
}

// The route in words, for the collapsed card.
function endpointSummary(endpoint, isSrc) {
  var info = protocolInfo(endpoint.protocol);
  var port = Number(endpoint.port) || 0;
  var bits = [];

  if (endpoint.protocol === 2 || endpoint.protocol === 3)
    bits.push(port || info.portRequired ? 'universe ' + port : 'universe 0');
  else if (endpoint.protocol === 4)
    bits.push('port ' + port);
  else if (endpoint.protocol === 5)
    bits.push('system ' + port);
  else if (isSrc)
    bits.push((endpoint.ip ? endpoint.ip : '') + ':' + port);
  else
    bits.push(endpoint.ip ? endpoint.ip + ':' + port : ':' + port);

  var usePath = isSrc ? info.srcPath : info.dstPath;
  if (usePath) {
    if (!isSrc && endpoint.script)
      bits.push('script');
    else if (endpoint.path)
      bits.push(endpoint.path);
  }

  return bits.join('  ');
}

function isEndpointIncomplete(endpoint, isSrc) {
  var info = protocolInfo(endpoint.protocol);
  if (!isSrc) return false;  // an outgoing endpoint may legitimately inherit everything
  if (!info.portRequired) return false;
  var port = Number(endpoint.port) || 0;
  if (info.portMin !== undefined) return port < info.portMin || port > info.portMax;
  return port === 0;
}

// -------------------------------------------------------------- route cards

function renderRoutes() {
  var list = el('routeList');
  list.textContent = '';
  cardsByUid = {};

  var query = filterText.trim().toLowerCase();
  var shown = 0;

  config.routes.forEach(function (route) {
    if (query && !matchesQuery(route, query)) return;
    var card = buildCard(route);
    cardsByUid[route._uid] = card;
    list.appendChild(card);
    shown++;
  });

  el('routesEmpty').hidden = config.routes.length !== 0;
  el('routesNoMatch').hidden = !(config.routes.length !== 0 && shown === 0);

  var enabled = config.routes.filter(function (r) { return r.enable; }).length;
  el('routeCount').textContent = config.routes.length === 0
    ? ''
    : config.routes.length + (config.routes.length === 1 ? ' route' : ' routes') +
      ', ' + enabled + ' enabled' + (query ? ' · ' + shown + ' shown' : '');

  var anyClosed = config.routes.some(function (r) { return !openRoutes[r._uid]; });
  el('expandAll').textContent = anyClosed ? 'Expand all' : 'Collapse all';
  el('expandAll').hidden = config.routes.length === 0;

  // The status arrives over the event stream, usually before the configuration
  // has finished loading, so the banner is rebuilt here as well: only now can
  // it name the route each message is about.
  applyIssues(issues);
  applyItemStates();
}

function matchesQuery(route, query) {
  var haystack = [
    route.label, route.notes,
    protocolInfo(route.src.protocol).name, protocolInfo(route.dst.protocol).name,
    route.src.ip, route.src.path, String(route.src.port),
    route.dst.ip, route.dst.path, String(route.dst.port)
  ].join(' ').toLowerCase();
  return haystack.indexOf(query) >= 0;
}

function buildCard(route) {
  var card = make('article', 'route');
  card.dataset.uid = route._uid;

  card.appendChild(buildHead(route));

  var body = make('div', 'route-body');
  body.hidden = !openRoutes[route._uid];
  if (openRoutes[route._uid]) {
    body.appendChild(buildEditor(route));
    card.classList.add('open');
  }
  card.appendChild(body);

  updateCardClasses(card, route);
  return card;
}

function updateCardClasses(card, route) {
  card.classList.toggle('disabled', !route.enable);
  var idx = routeIndex(route);
  card.classList.remove('has-error', 'has-warning');
  issues.forEach(function (issue) {
    if (issue.routeIndex === idx)
      card.classList.add(issue.level === 'error' ? 'has-error' : 'has-warning');
  });
}

function buildHead(route) {
  var head = make('div', 'route-head');
  head.addEventListener('click', function (e) {
    if (e.target.closest('.no-toggle')) return;
    toggleRoute(route);
  });

  var toggle = make('label', 'no-toggle');
  toggle.title = route.enable ? 'Enabled — click to disable' : 'Disabled — click to enable';
  toggle.appendChild(makeCheckbox(route.enable, function (value) {
    route.enable = value;
    setRouteFlag(route, 'enable', value);
  }));
  head.appendChild(toggle);

  var title = make('div', 'route-title');
  var name = make('span', 'route-name' + (route.label ? '' : ' unnamed'), route.label || 'Untitled route');
  title.appendChild(name);
  head.appendChild(title);

  var actions = make('div', 'route-actions no-toggle');
  actions.appendChild(activityChip(route));

  var muteBtn = make('button', 'btn small' + (route.mute ? ' primary' : ''), route.mute ? 'Muted' : 'Mute');
  muteBtn.title = 'Stop this route sending, without disabling it';
  muteBtn.addEventListener('click', function () {
    route.mute = !route.mute;
    setRouteFlag(route, 'mute', route.mute);
  });
  actions.appendChild(muteBtn);

  actions.appendChild(buildMenu(route));
  head.appendChild(actions);

  head.appendChild(buildFlow(route));

  if (route.notes)
    head.appendChild(make('p', 'route-note', route.notes));

  return head;
}

function buildFlow(route) {
  var flow = make('div', 'route-flow');
  flow.appendChild(buildEndpointSpan(route, true));
  flow.appendChild(make('span', 'arrow', '→'));
  flow.appendChild(buildEndpointSpan(route, false));
  return flow;
}

function buildEndpointSpan(route, isSrc) {
  var endpoint = isSrc ? route.src : route.dst;
  var span = make('span', 'endpoint');

  var indicator = make('span', 'row-indicator');
  indicator.dataset.role = isSrc ? 'src' : 'dst';
  span.appendChild(indicator);

  span.appendChild(make('span', 'proto', protocolInfo(endpoint.protocol).name));

  if (isEndpointIncomplete(endpoint, isSrc)) {
    span.appendChild(make('span', 'unset', 'not set'));
  } else {
    span.appendChild(make('span', 'detail', endpointSummary(endpoint, isSrc)));
  }

  var connection = usesTcp(endpoint);
  if (connection) {
    var chip = make('span', 'chip tcp', 'TCP');
    chip.title = 'Travels over the TCP connection "' + (connection.label || 'unnamed') + '"';
    span.appendChild(chip);
  }

  return span;
}

// "Is this route actually doing anything?" — the single most useful thing a
// card can answer, and the one the blinking indicator alone cannot.
function activityChip(route) {
  var chip = make('span', 'chip');
  chip.dataset.role = 'activity';
  paintActivityChip(chip, route);
  return chip;
}

function paintActivityChip(chip, route) {
  var state = itemStates[routeIndex(route)];
  // Only the timestamps, never the activity flags: those belong to the moment
  // the last event was sent, and this runs again on a timer long after. Reading
  // them here would leave a route reading "live" for as long as the page is
  // open, which is worse than not saying anything.
  var seen = state ? Math.max(state.srcLastActivity || 0, state.dstLastActivity || 0) : 0;

  if (!route.enable) {
    chip.className = 'chip off';
    chip.textContent = 'off';
    chip.title = 'This route is disabled and is not built when the router starts.';
    return;
  }

  if (!runStatus.running) {
    chip.className = 'chip idle';
    chip.textContent = 'stopped';
    chip.title = 'The router is not running.';
    return;
  }

  if (!seen) {
    chip.className = 'chip silent';
    chip.textContent = 'no traffic';
    chip.title = 'Nothing has passed through this route since the router started.';
    return;
  }

  var age = Math.floor(Date.now() / 1000) - seen;
  if (age <= 3) {
    chip.className = 'chip live';
    chip.textContent = 'live';
  } else {
    chip.className = 'chip idle';
    chip.textContent = relativeTime(seen);
  }
  chip.title = 'Last carried traffic ' + relativeTime(seen) + '.';
}

function buildMenu(route) {
  var wrap = make('div', 'menu-wrap');
  var button = make('button', 'btn small', '⋯');
  button.title = 'More';
  wrap.appendChild(button);

  button.addEventListener('click', function () {
    var existing = wrap.querySelector('.menu-pop');
    closeMenus();
    if (existing) return;

    var pop = make('div', 'menu-pop');
    var index = routeIndex(route);

    function item(label, onClick, disabled, danger) {
      var b = make('button', danger ? 'danger' : '', label);
      b.disabled = !!disabled;
      b.addEventListener('click', function () {
        closeMenus();
        onClick();
      });
      pop.appendChild(b);
    }

    item('Duplicate', function () {
      var copy = JSON.parse(JSON.stringify(route));
      delete copy._uid;
      copy.label = (route.label || 'Untitled route') + ' copy';
      config.routes.splice(index + 1, 0, copy);
      adoptRoutes();
      openRoutes[copy._uid] = true;
      markDirty();
      renderRoutes();
    });

    item('Move up', function () { moveRoute(index, -1); }, index === 0);
    item('Move down', function () { moveRoute(index, 1); }, index === config.routes.length - 1);

    pop.appendChild(make('hr'));

    item('Delete', function () {
      config.routes.splice(index, 1);
      markDirty();
      renderRoutes();
    }, false, true);

    wrap.appendChild(pop);
  });

  return wrap;
}

function closeMenus() {
  Array.prototype.forEach.call(document.querySelectorAll('.menu-pop'), function (pop) {
    pop.parentNode.removeChild(pop);
  });
}

function moveRoute(index, delta) {
  var target = index + delta;
  if (target < 0 || target >= config.routes.length) return;
  var moved = config.routes.splice(index, 1)[0];
  config.routes.splice(target, 0, moved);
  markDirty();
  renderRoutes();
}

function toggleRoute(route) {
  if (openRoutes[route._uid]) delete openRoutes[route._uid];
  else openRoutes[route._uid] = true;
  refreshCard(route);
}

// Rebuilds one card in place, so editing one route never disturbs the others.
function refreshCard(route) {
  var card = cardsByUid[route._uid];
  if (!card) return;
  var fresh = buildCard(route);
  card.parentNode.replaceChild(fresh, card);
  cardsByUid[route._uid] = fresh;
  applyItemStates();
}

// Updates only the summary, leaving the editor and its focus alone.
function refreshHead(route) {
  var card = cardsByUid[route._uid];
  if (!card) return;
  card.replaceChild(buildHead(route), card.querySelector('.route-head'));
  updateCardClasses(card, route);
  applyItemStates();
}

// ------------------------------------------------------------ route editor

function buildEditor(route) {
  var wrap = make('div');

  var top = make('div', 'field');
  var nameLabel = make('label', null, 'Name');
  top.appendChild(nameLabel);
  top.appendChild(makeInput(route.label, function (v) {
    route.label = v;
    markDirty();
    var name = cardsByUid[route._uid].querySelector('.route-name');
    name.textContent = v || 'Untitled route';
    name.className = 'route-name' + (v ? '' : ' unnamed');
  }, {placeholder: 'What this route is for, in a few words'}));
  wrap.appendChild(top);

  var endpoints = make('div', 'endpoints');
  endpoints.appendChild(buildSide(route, true));
  endpoints.appendChild(make('div', 'flow-arrow', '➜'));
  endpoints.appendChild(buildSide(route, false));
  wrap.appendChild(endpoints);

  // The answer to "what was I thinking". Nothing else in the configuration can
  // record intent, and intent is what is missing when a show is revisited.
  var notes = make('textarea');
  notes.rows = 2;
  notes.value = route.notes || '';
  notes.placeholder = 'Why this route exists, what is plugged in at each end, anything the next person needs to know.';
  notes.addEventListener('input', function () {
    route.notes = notes.value;
    markDirty();
  });
  notes.addEventListener('change', function () { refreshHead(route); });
  wrap.appendChild(field('Notes', notes, 'Saved with the configuration. The desktop application does not keep these.'));

  wrap.appendChild(buildTransforms(route));
  wrap.appendChild(buildHelp(route));

  return wrap;
}

function buildSide(route, isSrc) {
  var endpoint = isSrc ? route.src : route.dst;
  var side = make('div', 'side');

  var heading = make('h3');
  heading.appendChild(document.createTextNode(isSrc ? 'Incoming' : 'Outgoing'));
  side.appendChild(heading);

  side.appendChild(field('Protocol', makeSelect(PROTOCOLS.map(function (p) { return p.name; }), endpoint.protocol, function (v) {
    endpoint.protocol = v;
    markDirty();
    // Which fields exist depends on this, so the whole card is rebuilt.
    refreshCard(route);
  })));

  var info = protocolInfo(endpoint.protocol);

  var portInput = makeInput(Number(endpoint.port) || endpoint.port === 0 ? endpoint.port : '', function (v) {
    endpoint.port = parseInt(v, 10) || 0;
    markDirty();
    refreshHead(route);
  }, {type: 'number', placeholder: info.portPlaceholder});
  if (info.portMin !== undefined) {
    portInput.min = info.portMin;
    portInput.max = info.portMax;
  }
  side.appendChild(field(info.portLabel, portInput, portHint(endpoint.protocol, isSrc), isSrc && info.portRequired));

  if (info.hasIP) {
    side.appendChild(field(isSrc ? 'From IP' : 'To IP', makeInput(endpoint.ip, function (v) {
      endpoint.ip = v;
      markDirty();
      refreshHead(route);
    }, {placeholder: isSrc ? 'any address' : 'reply to sender'}), ipHint(isSrc)));

    // The desktop application hides this inside the IP field as "group,iface",
    // which is undiscoverable. It is its own field here.
    var mcast = makeInput(endpoint.multicastInterfaceIP, function (v) {
      endpoint.multicastInterfaceIP = v;
      markDirty();
    }, {placeholder: 'default'});
    mcast.setAttribute('list', 'interfaces');
    side.appendChild(field('Multicast interface', mcast, 'Only used when the address above is a multicast group.'));
  } else {
    var note = make('p', 'field-hint', protocolInfo(endpoint.protocol).name + ' does not use an IP address.');
    side.appendChild(note);
  }

  var usePath = isSrc ? info.srcPath : info.dstPath;
  if (usePath) {
    if (!isSrc && endpoint.script) {
      var script = make('textarea');
      script.rows = 4;
      script.value = endpoint.scriptText || '';
      script.spellcheck = false;
      script.addEventListener('input', function () {
        endpoint.scriptText = script.value;
        endpoint.script = true;
        markDirty();
      });
      side.appendChild(field('Script', script, 'JavaScript run for each message. The incoming address is in <code>OSC</code>; assign to it to rewrite.'));
    } else {
      side.appendChild(field(isSrc ? info.srcPathLabel : info.dstPathLabel, makeInput(endpoint.path, function (v) {
        endpoint.path = v;
        markDirty();
        refreshHead(route);
      }, {placeholder: isSrc ? 'any message' : '/cue/%6/start'}), pathHint(endpoint.protocol, isSrc)));
    }

    if (!isSrc) {
      var swap = make('button', 'btn small', endpoint.script ? 'Use an address instead' : 'Use a script instead');
      swap.addEventListener('click', function () {
        endpoint.script = !endpoint.script;
        markDirty();
        refreshCard(route);
      });
      side.appendChild(swap);
    }
  } else if (isSrc) {
    side.appendChild(make('p', 'field-hint',
      info.name + ' carries a whole universe of levels rather than addressed messages, so there is nothing to filter here. ' +
      'Refer to the levels as %1 to %512 in the outgoing address.'));
  }

  return side;
}

function buildTransforms(route) {
  var details = make('details', 'more');
  var any = ['inMin', 'inMax', 'outMin', 'outMax'].some(function (k) { return route[k] && route[k].value !== '' && route[k].value !== undefined; });
  details.open = any;

  details.appendChild(make('summary', null, 'Value range' + (any ? '' : ' — not used')));

  var body = make('div', 'more-body');
  body.appendChild(make('p', 'field-hint',
    'Fill in all four to rescale the first value in each message from one range to another. ' +
    'Fill in only some to clip it. Leave them all blank to pass values through untouched.'));

  var grid = make('div', 'transform-grid');
  [['inMin', 'Incoming min'], ['inMax', 'Incoming max'], ['outMin', 'Outgoing min'], ['outMax', 'Outgoing max']].forEach(function (pair) {
    var key = pair[0];
    if (!route[key]) route[key] = {value: ''};
    grid.appendChild(field(pair[1], makeInput(route[key].value, function (v) {
      route[key].value = v;
      markDirty();
    }, {placeholder: '—'})));
  });
  body.appendChild(grid);

  details.appendChild(body);
  return details;
}

// The path syntax is the hardest part of OSCRouter and the desktop application
// buries it in a tooltip. Shown here for the protocols this route actually
// uses, so it is short enough to read.
function buildHelp(route) {
  var details = make('details', 'more help');
  details.appendChild(make('summary', null, 'How addresses and levels are written'));

  var body = make('div', 'more-body');
  var src = route.src.protocol;
  var dst = route.dst.protocol;

  function section(title, lines) {
    body.appendChild(make('h4', null, title));
    body.appendChild(make('pre', null, lines.join('\n')));
  }

  section('Insert parts of the incoming message with %1, %2, …', [
    'Incoming:  /eos/out/event/cue/1/25/fire',
    'Outgoing:  /cue/%6/start',
    'Sends:     /cue/25/start',
    '',
    'Incoming:  /cue/25/start',
    'Outgoing:  /eos/cue/fire=%2',
    'Sends:     /eos/cue/fire with the value 25'
  ]);

  if (src === 2 || src === 3) {
    section('Incoming ' + protocolInfo(src).name + ' levels', [
      '%1 to %512 are the levels in the universe.',
      '',
      'Outgoing:  /eos/chan/1/param/red/green/blue=%10,%11,%12',
      'Sends:     the levels of channels 10, 11 and 12'
    ]);
  }

  if (dst === 2 || dst === 3) {
    var word = dst === 2 ? 'sacn' : 'artnet';
    var lines = [
      '/' + word + '=1,2,3,…              set the universe from channel 1',
      '/' + word + '/offset/<n>=1,2,3,…   set it from channel <n>'
    ];
    if (dst === 2) {
      lines.push('/sacn/priority/<n>=…          set the priority');
      lines.push('/sacn/perChannelPriority/<n>=… per channel priority');
    }
    lines.push('');
    lines.push('Incoming:  /rgb/255/0/127');
    lines.push('Outgoing:  /' + word + '/offset/10=%2,%3,%4');
    lines.push('Sends:     channel 10=255, 11=0, 12=127');
    section('Outgoing ' + protocolInfo(dst).name, lines);
  }

  if (src === 1 || dst === 1) {
    section('PSN', [
      '/psn/<id>/pos=x,y,z',
      '/psn/<id>/speed=x,y,z',
      '/psn/<id>/orientation=x,y,z',
      '/psn/<id>/acceleration=x,y,z',
      '/psn/<id>/target=x,y,z',
      '/psn/<id>/status=status',
      '/psn/<id>/timestamp=timestamp'
    ]);
  }

  if (src === 4 || dst === 4) {
    section('MIDI', [
      'Raw:                /midi=a,b,c…',
      'MIDI Show Control:  /msc/<device ID>/<command format>/<command>',
      '',
      'Incoming:  /msc/2/1/go, 3, 4',
      'Outgoing:  /eos/cue/%6/%5/fire=',
      'Sends:     /eos/cue/4/3/fire'
    ]);
  }

  if (src === 5 || dst === 5) {
    section('OTP', [
      'The port field is the OTP system number, 0 to 200.',
      '',
      '/otp/<group>/<point>/<priority>/pos=x,y,z',
      '/otp/<group>/<point>/<priority>/posVelAccel=vx,vy,vz,ax,ay,az',
      '/otp/<group>/<point>/<priority>/rot=x,y,z',
      '/otp/<group>/<point>/<priority>/rotVelAccel=vx,vy,vz,ax,ay,az',
      '/otp/<group>/<point>/<priority>/scale=x,y,z',
      '/otp/<group>/<point>/<priority>/frame=x,y,z'
    ]);
  }

  details.appendChild(body);
  return details;
}

// ---------------------------------------------------------------- presets

// A first route is much easier to understand as a worked example than as an
// empty form with fifteen fields.
var PRESETS = [
  {
    label: 'OSC to OSC',
    describe: 'Forward OSC from one device to another',
    apply: function (r) {
      r.label = 'OSC to OSC';
      r.src.protocol = 0; r.src.port = 8000;
      r.dst.protocol = 0; r.dst.port = 8001;
    }
  },
  {
    label: 'OSC to sACN',
    describe: 'Drive lighting levels from OSC messages',
    apply: function (r) {
      r.label = 'OSC to sACN';
      r.src.protocol = 0; r.src.port = 8000; r.src.path = '/level/%2';
      r.dst.protocol = 2; r.dst.port = 1; r.dst.path = '/sacn/offset/1=%2';
    }
  },
  {
    label: 'sACN to OSC',
    describe: 'Turn lighting levels into OSC messages',
    apply: function (r) {
      r.label = 'sACN to OSC';
      r.src.protocol = 2; r.src.port = 1;
      r.dst.protocol = 0; r.dst.port = 8000; r.dst.path = '/level=%1';
    }
  },
  {
    label: 'Empty route',
    describe: 'Start from nothing',
    apply: function () {}
  }
];

function renderPresets() {
  var container = el('presets');
  container.textContent = '';
  PRESETS.forEach(function (preset) {
    var button = make('button', 'btn small', preset.label);
    button.title = preset.describe;
    button.addEventListener('click', function () { addRoute(preset); });
    container.appendChild(button);
  });
}

function addRoute(preset) {
  var route = emptyRoute();
  if (preset) preset.apply(route);
  config.routes.push(route);
  adoptRoutes();
  openRoutes[route._uid] = true;
  markDirty();
  renderRoutes();
  var card = cardsByUid[route._uid];
  if (card) card.scrollIntoView({block: 'nearest'});
}

// ----------------------------------------------------------- dirty tracking

function markDirty() {
  dirty = true;
  el('dirtyBar').hidden = false;
}

function markClean() {
  dirty = false;
  el('dirtyBar').hidden = true;
}

// Enable and mute normally take effect at once, which is what you want during a
// show. While there are unsaved edits they are held back instead, so that one
// click cannot write a half-finished configuration to disk.
function setRouteFlag(route, flag, value) {
  if (dirty) {
    refreshHead(route);
    updateCardClasses(cardsByUid[route._uid], route);
    return;
  }

  var index = routeIndex(route);
  api('api/routes/' + index + '/' + flag, postJson({value: value}))
    .then(function () { renderRoutes(); })
    .catch(function (e) {
      // The daemon rejected it, so the card should not go on claiming otherwise.
      route[flag] = !value;
      renderRoutes();
      toast(e.message, true);
    });
}

// -------------------------------------------------------------------- tcp

function renderConnections() {
  var body = el('tcpBody');
  body.textContent = '';

  config.connections.forEach(function (connection, index) {
    var row = document.createElement('tr');

    function cell(child) {
      var td = document.createElement('td');
      if (child) td.appendChild(child);
      row.appendChild(td);
      return td;
    }

    cell(makeInput(connection.label, function (v) { connection.label = v; markDirty(); }, {className: 'label'}));
    cell(makeSelect(['Server', 'Client'], connection.server ? 0 : 1, function (v) { connection.server = (v === 0); markDirty(); }));
    cell(makeSelect(FRAME_MODES, connection.frameMode, function (v) { connection.frameMode = v; markDirty(); }));
    cell(makeInput(connection.ip, function (v) { connection.ip = v; markDirty(); }, {className: 'ip'}));
    cell(makeInput(connection.port, function (v) { connection.port = parseInt(v, 10) || 0; markDirty(); }, {type: 'number', className: 'port'}));

    var removeBtn = make('button', 'btn small danger', '✕');
    removeBtn.title = 'Remove this connection';
    removeBtn.addEventListener('click', function () {
      config.connections.splice(index, 1);
      markDirty();
      renderConnections();
      renderRoutes();
    });
    cell(removeBtn);

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
    var label = make('label');
    var input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = !!(settings.otpModules && settings.otpModules[index]);
    input.addEventListener('change', function () {
      if (!settings.otpModules) settings.otpModules = [];
      settings.otpModules[index] = input.checked;
      markDirty();
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
  config.routes.forEach(function (route, index) {
    var card = cardsByUid[route._uid];
    if (!card) return;
    var state = itemStates[index];

    var src = card.querySelector('.route-head [data-role="src"]');
    if (src) src.className = 'row-indicator ' + ((state && state.srcState) || '') + (state && state.srcActivity ? ' activity' : '');

    var dst = card.querySelector('.route-head [data-role="dst"]');
    if (dst) dst.className = 'row-indicator ' + ((state && state.dstState) || '') + (state && state.dstActivity ? ' activity' : '');

    var chip = card.querySelector('[data-role="activity"]');
    if (chip) paintActivityChip(chip, route);
  });
}

function refreshActivityChips() {
  config.routes.forEach(function (route) {
    var card = cardsByUid[route._uid];
    if (!card) return;
    var chip = card.querySelector('[data-role="activity"]');
    if (chip) paintActivityChip(chip, route);
  });
}

function applyStatus(next) {
  runStatus = next;
  el('runIndicator').className = 'indicator ' + (runStatus.running ? 'connected' : 'notConnected');
  el('runLabel').textContent = runStatus.running ? 'Running' : 'Stopped';
  el('startBtn').disabled = !!runStatus.running;
  el('stopBtn').disabled = !runStatus.running;

  if (runStatus.version)
    el('runLabel').title = 'OSCRouter ' + runStatus.version;

  applyIssues(runStatus.issues || []);
  refreshActivityChips();
}

// A route can be well formed and still never carry anything. Rather than
// leaving that to one line in the log, say so above the list and mark the card.
function applyIssues(list) {
  issues = list;

  var container = el('issues');
  container.textContent = '';
  container.hidden = issues.length === 0;

  issues.forEach(function (issue) {
    var row = make('div', 'issue ' + issue.level);
    row.appendChild(make('span', 'badge', issue.level));

    var text = make('span');
    text.textContent = issue.message;
    row.appendChild(text);

    // Named routes are easier to recognise than numbered ones, and the button
    // opens the one being complained about.
    var route = config.routes[issue.routeIndex];
    if (route) {
      var jump = make('button', 'jump', route.label || 'route ' + (issue.routeIndex + 1));
      jump.addEventListener('click', function () {
        openRoutes[route._uid] = true;
        filterText = '';
        el('routeSearch').value = '';
        renderRoutes();
        var card = cardsByUid[route._uid];
        if (card) card.scrollIntoView({block: 'center'});
      });
      row.appendChild(jump);
    }

    container.appendChild(row);
  });

  config.routes.forEach(function (route) {
    var card = cardsByUid[route._uid];
    if (card) updateCardClasses(card, route);
  });
}

var MAX_LOG_LINES = 500;

function appendLog(message) {
  var body = el('logBody');

  var line = make('div', 'log-line log-' + message.type);
  line.appendChild(make('span', 'time', new Date(message.timestamp * 1000).toLocaleTimeString()));
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

function loadConfig() {
  return api('api/config').then(function (data) {
    config = data;
    adoptRoutes();
    markClean();
    renderRoutes();
    renderConnections();
    renderSettings();
  });
}

function saveAndApply() {
  collectSettings();
  config.muteAllIncoming = el('muteAllIncoming').checked;
  config.muteAllOutgoing = el('muteAllOutgoing').checked;

  api('api/config', {method: 'PUT', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(config)})
    .then(function () { return api('api/config/apply', postJson({})); })
    .then(function () {
      markClean();
      toast('Saved and restarted');
      return loadConfig();
    })
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
  el('addRoute').addEventListener('click', function () { addRoute(null); });

  el('expandAll').addEventListener('click', function () {
    var anyClosed = config.routes.some(function (r) { return !openRoutes[r._uid]; });
    openRoutes = {};
    if (anyClosed)
      config.routes.forEach(function (r) { openRoutes[r._uid] = true; });
    renderRoutes();
  });

  el('routeSearch').addEventListener('input', function () {
    filterText = el('routeSearch').value;
    renderRoutes();
  });

  el('addTcp').addEventListener('click', function () {
    config.connections.push({label: '', server: true, frameMode: 1, ip: '', port: 0});
    markDirty();
    renderConnections();
  });

  el('applyBtn').addEventListener('click', saveAndApply);

  el('discardBtn').addEventListener('click', function () {
    loadConfig().then(function () { toast('Reloaded from disk'); });
  });

  el('startBtn').addEventListener('click', function () {
    api('api/start', postJson({})).then(function () { toast('Started'); }).catch(function (e) { toast(e.message, true); });
  });

  el('stopBtn').addEventListener('click', function () {
    api('api/stop', postJson({})).then(function () { toast('Stopped'); }).catch(function (e) { toast(e.message, true); });
  });

  function muteAll() {
    api('api/mute-all', postJson({incoming: el('muteAllIncoming').checked, outgoing: el('muteAllOutgoing').checked}))
      .catch(function (e) { toast(e.message, true); });
  }
  el('muteAllIncoming').addEventListener('change', muteAll);
  el('muteAllOutgoing').addEventListener('change', muteAll);

  el('clearLog').addEventListener('click', function () { el('logBody').textContent = ''; });
  el('logToggle').addEventListener('click', function () { el('logPane').classList.toggle('collapsed'); });

  el('saveFile').addEventListener('click', function () {
    api('api/file', {method: 'PUT', headers: {'Content-Type': 'text/plain'}, body: el('rawFile').value})
      .then(function () { toast('File saved'); return loadConfig(); })
      .catch(function (e) { toast(e.message, true); });
  });

  document.addEventListener('click', function (e) {
    if (!e.target.closest('.menu-wrap')) closeMenus();
  });

  window.addEventListener('beforeunload', function (e) {
    if (!dirty) return;
    e.preventDefault();
    e.returnValue = '';
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
renderPresets();
loadConfig().catch(function (e) { toast(e.message, true); });
connectEvents();
setInterval(refreshActivityChips, 5000);
