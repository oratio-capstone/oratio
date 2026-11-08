// Live view for the gateway WebSocket server (task 128). Three tabs: Live (people, relays,
// alerts), Topology (the mesh tree) and Log (saved history). The SOS banner and Listening
// panel show on every tab.
// Nodes can be renamed (✎) to the person wearing them; names are stored on the gateway and
// shared by every dashboard.
// Connects to ws://<same host>:8000/ws, shows nodes and alerts, reconnects automatically.
// "Listen" streams a node's audio frames and draws them as a live waveform.
// Sound playback of node audio isn't implemented yet; an SOS sounds an alarm (see "SOS alarm sound").

const WS_URL = `ws://${location.hostname || "localhost"}:8000/ws`;
const FRAME_SECONDS = 0.02;    // each audio frame is 20 ms
const WAVE_HISTORY = 200;      // bars kept per waveform (200 x 20 ms = 4 s)
const SILENCE_MS = 500;        // no frame for this long -> "no audio"

const nodes = new Map();       // node_id -> node
const listening = new Map();   // node_id -> { frames, levels: Float32Array, head, lastFrameAt, card }
let alerts = [];
const openSos = new Map();     // node_id -> unacknowledged SOS alert (shared by every dashboard)
const PAGE_TITLE = document.title;
let socket = null;

const $ = (id) => document.getElementById(id);

// ---- connection --------------------------------------------------------------

function connect() {
  setConn("connecting…", "");
  socket = new WebSocket(WS_URL);
  socket.binaryType = "arraybuffer";

  socket.onopen = () => {
    setConn("live", "ok");
    for (const [id, stream] of listening) {  // resume after reconnect (the gateway re-checks permission)
      if (!stream.paused) send({ type: "listen", node_id: id, emergency: stream.emergency });
    }
  };
  socket.onclose = () => {
    setConn("reconnecting…", "bad");
    setTimeout(connect, 1500);
  };
  socket.onmessage = (event) => {
    if (event.data instanceof ArrayBuffer) {
      onAudio(event.data);
      return;
    }
    const msg = JSON.parse(event.data);
    if (msg.type === "snapshot") {
      nodes.clear();
      for (const n of msg.nodes) nodes.set(n.node_id, n);
      alerts = msg.alerts;
      openSos.clear();
      for (const a of msg.open_sos || []) openSos.set(a.node_id, a);
      if (logLoaded) loadLog();  // reconnected: the gateway may have restarted
    } else if (msg.type === "node_update") {
      nodes.set(msg.node.node_id, msg.node);
      const stream = listening.get(msg.node.node_id);
      if (stream && stream.paused && msg.node.talking) {  // they pressed talk again: pick up
        stream.paused = false;
        send({ type: "listen", node_id: msg.node.node_id });
      }
    } else if (msg.type === "listen_denied") {
      endStream(msg.node_id);
      toast(msg.message);
    } else if (msg.type === "listen_ended") {
      const stream = listening.get(msg.node_id);
      if (stream && msg.reason === "talk_released") stream.paused = true;  // wait for the next press
      else { endStream(msg.node_id); toast(msg.message); }
    } else if (msg.type === "alert") {
      alerts.push(msg.alert);
      alerts = alerts.slice(-50);
      if (msg.alert.kind === "sos" && !msg.alert.acked) openSos.set(msg.alert.node_id, msg.alert);
    } else if (msg.type === "sos_update") {
      const a = msg.alert;
      if (a.acked) openSos.delete(a.node_id);
      else openSos.set(a.node_id, a);
      const i = alerts.findIndex((x) => x.id === a.id);  // keep the history entry in step
      if (i >= 0) alerts[i] = a;
      onLogUpdate(a);
    } else if (msg.type === "log_event") {
      onLogEvent(msg.event);
    } else if (msg.type === "log_result") {
      onLogResult(msg);
    } else if (msg.type === "log_stats") {
      onLogStats(msg);
    }
    scheduleRender();
  };
}

function send(obj) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(obj));
}

function setConn(text, cls) {
  $("conn").textContent = text;
  $("conn").className = `pill ${cls}`;
}

// ---- audio -------------------------------------------------------------------

// Binary frame: 2-byte big-endian node id, then 16-bit little-endian PCM samples.
function onAudio(buffer) {
  const id = new DataView(buffer).getUint16(0);
  const stream = listening.get(id);
  if (!stream) return;
  const samples = new Int16Array(buffer, 2, (buffer.byteLength - 2) >> 1);
  let sumSquares = 0;
  for (let i = 0; i < samples.length; i++) sumSquares += samples[i] * samples[i];
  const rms = Math.sqrt(sumSquares / Math.max(1, samples.length)) / 32768;
  stream.levels[stream.head] = Math.min(1, rms * 3);  // scale so normal speech fills the height
  stream.head = (stream.head + 1) % WAVE_HISTORY;
  stream.frames++;
  stream.lastFrameAt = performance.now();
}

// Listening is allowed while the worker has an open SOS (or holds a talk button, if the
// devices ever get one); the gateway enforces this. Otherwise the only way in is a
// confirmed Emergency listen.
function canListen(id) {
  return Boolean(nodes.get(id)?.talking) || openSos.has(id);
}

function toggleListen(id, emergency = false) {
  if (listening.has(id)) {
    endStream(id);
    send({ type: "stop", node_id: id });
  } else if (!emergency && !canListen(id)) {
    openEmergencyDialog(id);
    return;
  } else {
    listening.set(id, { frames: 0, levels: new Float32Array(WAVE_HISTORY), head: 0, lastFrameAt: 0,
                        emergency, paused: false, card: makeWaveCard(id, emergency) });
    send({ type: "listen", node_id: id, emergency });
    requestAnimationFrame(drawWaves);
  }
  scheduleRender();
}

function endStream(id) {
  const stream = listening.get(id);
  if (!stream) return;
  stream.card.remove();
  listening.delete(id);
  scheduleRender();
}

// ---- emergency listen ------------------------------------------------------------

let emergencyId = null;

function openEmergencyDialog(id) {
  emergencyId = id;
  $("em-name").textContent = displayName(id);
  $("emergency-dialog").showModal();
  $("em-cancel").focus();  // the safe choice is the default
}

$("em-cancel").addEventListener("click", () => $("emergency-dialog").close());
$("em-confirm").addEventListener("click", () => {
  $("emergency-dialog").close();
  if (emergencyId !== null && !listening.has(emergencyId)) toggleListen(emergencyId, true);
});

let toastTimer = null;
function toast(text) {
  const el = $("toast");
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 5000);
}

function seconds(stream) {
  return (stream.frames * FRAME_SECONDS).toFixed(1);
}

function makeWaveCard(id, emergency) {
  const card = document.createElement("div");
  card.className = emergency ? "wave-card emergency" : "wave-card";
  card.innerHTML = `
    <div class="wave-head">
      <strong class="wave-name"></strong>
      ${emergency ? '<span class="tag-emergency">Emergency listen</span>' : ""}
      <span class="wave-stat muted"></span>
      <button data-node="${id}">Stop</button>
    </div>
    <canvas class="wave"></canvas>`;
  $("waves").appendChild(card);
  $("listening-section").hidden = false;
  return card;
}

function drawWaves() {
  if (!listening.size) {
    $("listening-section").hidden = true;
    return;
  }
  const styles = getComputedStyle(document.documentElement);
  const accent = styles.getPropertyValue("--accent").trim();
  const quiet = styles.getPropertyValue("--line").trim();
  const now = performance.now();

  for (const [id, stream] of listening) {
    const canvas = stream.card.querySelector("canvas");
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const mid = h / 2;
    ctx.fillStyle = quiet;
    ctx.fillRect(0, mid - 0.5, w, 1);  // centre line

    const step = 4, barWidth = 2.5;  // newest bar on the right, scrolling left
    const count = Math.min(WAVE_HISTORY, Math.floor(w / step));
    const silent = now - stream.lastFrameAt > SILENCE_MS;
    ctx.fillStyle = silent ? quiet : accent;
    for (let i = 0; i < count; i++) {
      const level = stream.levels[(stream.head - 1 - i + WAVE_HISTORY) % WAVE_HISTORY];
      const half = Math.max(1, level * (mid - 2));
      ctx.fillRect(w - (i + 1) * step, mid - half, barWidth, half * 2);
    }

    const title = displayName(id);
    const nameEl = stream.card.querySelector(".wave-name");
    if (nameEl.textContent !== title) nameEl.textContent = title;
    stream.card.querySelector(".wave-stat").textContent =
      stream.paused ? `${seconds(stream)} s · waiting for ${title} to press talk`
        : silent && stream.frames ? `${seconds(stream)} s · no audio`
        : silent ? "waiting for audio…" : `${seconds(stream)} s received`;
  }
  requestAnimationFrame(drawWaves);
}

// ---- tables and alerts ---------------------------------------------------------

// Rows are created once per node and then updated in place. Rebuilding the table on every
// update replaced the buttons several times a second, so clicks between mouse-down and
// mouse-up were lost.
const tables = {
  people: { body: $("people"), rows: new Map(), cols: 6, empty: "Waiting for nodes…" },
  relays: { body: $("relays"), rows: new Map(), cols: 5, empty: "No relays yet" },
};
let renderQueued = false;

function scheduleRender() {
  if (!renderQueued) {
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }
}

// Signal strength (RSSI to the node's parent) -> label and colour class.
function linkQuality(dbm) {
  if (dbm === null || dbm === undefined) return { text: "–", cls: "", level: "none" };
  if (dbm >= -50) return { text: `${dbm} · excellent`, cls: "link-good", level: "good" };
  if (dbm >= -67) return { text: `${dbm} · good`, cls: "link-good", level: "good" };
  if (dbm >= -80) return { text: `${dbm} · weak`, cls: "link-weak", level: "weak" };
  return { text: `${dbm} · poor`, cls: "link-poor", level: "poor" };
}

function ago(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  return s < 60 ? `${s.toFixed(0)} s ago` : `${(s / 60).toFixed(0)} min ago`;
}

const isRelay = (n) => n?.role === "relay";

// Name shown for a node: the name given on the dashboard (a person for a worn device, a
// place for a relay), otherwise "Node 3" / "Relay 101".
function displayName(id) {
  const n = nodes.get(id);
  return n?.name || (isRelay(n) ? `Relay ${id}` : `Node ${id}`);
}

// The next hop towards the Pi: 0 is the Pi itself.
function hopName(id) {
  if (id === 0) return "Pi";
  return id === null || id === undefined ? "–" : displayName(id);
}

// Ids of a node's parents up to the root (the Pi itself excluded).
function ancestors(id) {
  const chain = [];
  let parent = nodes.get(id)?.parent;
  while (parent && nodes.has(parent) && parent !== id && !chain.includes(parent)) {
    chain.push(parent);
    parent = nodes.get(parent).parent;
  }
  return chain;
}

// People whose connection to the Pi currently runs through this relay.
function carriedBy(relayId) {
  return [...nodes.values()]
    .filter((n) => !isRelay(n) && n.status === "active" && ancestors(n.node_id).includes(relayId))
    .map((n) => n.node_id);
}

function rowFor(table, id) {
  let row = table.rows.get(id);
  if (!row) {
    const tr = document.createElement("tr");
    const cells = Array.from({ length: table.cols }, () => tr.appendChild(document.createElement("td")));
    row = { tr, cells, button: null, nameEl: null, idEl: null };
    if (table === tables.people) {
      row.button = document.createElement("button");
      row.button.dataset.node = id;
      row.emergency = document.createElement("button");
      row.emergency.className = "emergency-btn";
      row.emergency.dataset.emergency = id;
      row.emergency.textContent = "Emergency…";
      row.emergency.title = "Listen without their request. Recorded in the log and shown on their device.";
      cells[table.cols - 1].append(row.button, row.emergency);
    }
    table.rows.set(id, row);
    buildNameCell(row, id);
  }
  return row;
}

// First cell: "Alex · node 3" (or "Node 3") and a rename button.
function buildNameCell(row, id) {
  const nameEl = document.createElement("span");
  nameEl.className = "name";
  const idEl = document.createElement("span");
  idEl.className = "muted";
  const rename = document.createElement("button");
  rename.className = "rename";
  rename.dataset.rename = id;
  rename.title = "Rename";
  rename.setAttribute("aria-label", `Rename ${displayName(id)}`);
  rename.textContent = "✎";
  row.cells[0].replaceChildren(nameEl, idEl, rename);
  row.nameEl = nameEl;
  row.idEl = idEl;
}

function setCell(cell, text, cls = "") {
  if (cell.textContent !== text) cell.textContent = text;
  if (cell.className !== cls) cell.className = cls;
}

// ---- renaming --------------------------------------------------------------------

let editingId = null;

function startRename(id) {
  if (editingId !== null) return;
  const row = tables.people.rows.get(id) || tables.relays.rows.get(id);
  if (!row) return;
  editingId = id;
  const input = document.createElement("input");
  input.className = "name-input";
  input.maxLength = 32;
  input.placeholder = isRelay(nodes.get(id)) ? "Place, e.g. Tunnel B 200 m" : "Name, e.g. Alex";
  input.value = nodes.get(id)?.name || "";
  input.setAttribute("aria-label", `Name for ${displayName(id)}`);
  let done = false;
  const finish = (save) => {
    if (done) return;
    done = true;
    if (save) send({ type: "rename", node_id: id, name: input.value.trim() });
    editingId = null;
    buildNameCell(row, id);
    scheduleRender();
  };
  input.addEventListener("keydown", (event) => {
    if (event.key === "Enter") finish(true);
    if (event.key === "Escape") finish(false);
  });
  input.addEventListener("blur", () => finish(true));
  row.cells[0].replaceChildren(input);
  input.focus();
  input.select();
}

// Puts each node of `list` in a row of `table`, in order, updating rows in place.
function fillTable(table, list, update) {
  const ids = new Set(list.map((n) => n.node_id));
  for (const [id, row] of table.rows) {
    if (!ids.has(id)) { row.tr.remove(); table.rows.delete(id); }  // e.g. its role became known
  }
  if (!list.length) {
    if (!table.body.querySelector(".empty")) {
      table.body.innerHTML = `<tr class="empty"><td colspan="${table.cols}" class="muted">${table.empty}</td></tr>`;
    }
    return;
  }
  table.body.querySelector(".empty")?.remove();
  list.forEach((n, index) => {
    const row = rowFor(table, n.node_id);
    if (editingId !== n.node_id) {
      const name = displayName(n.node_id);
      const suffix = n.name ? ` · ${isRelay(n) ? "relay" : "node"} ${n.node_id}` : "";
      if (row.nameEl.textContent !== name) row.nameEl.textContent = name;
      if (row.idEl.textContent !== suffix) row.idEl.textContent = suffix;
    }
    update(n, row);
    if (table.body.children[index] !== row.tr) table.body.insertBefore(row.tr, table.body.children[index] || null);
  });
}

function countText(list) {
  const active = list.filter((n) => n.status === "active").length;
  return list.length ? `${active} active / ${list.length}` : "";
}

function render() {
  const all = [...nodes.values()].sort((a, b) => a.node_id - b.node_id);
  const people = all.filter((n) => !isRelay(n));
  const relays = all.filter(isRelay);
  $("people-count").textContent = countText(people);
  $("relay-count").textContent = countText(relays);

  fillTable(tables.people, people, (n, row) => {
    // A dropped node's last reading is stale, so it is shown greyed out.
    const stale = n.status === "dropped";
    const link = linkQuality(n.link);
    const talking = n.talking && !stale;
    setCell(row.cells[1], talking ? "talking" : n.status, talking ? "status-talking" : `status-${n.status}`);
    setCell(row.cells[2], n.near ? displayName(n.near) : n.parent === 0 ? "Pi" : "–", stale ? "muted" : "");
    setCell(row.cells[3], link.text, stale ? "muted" : link.cls);
    setCell(row.cells[4], ago(n.last_seen));
    const stream = listening.get(n.node_id);
    const allowed = canListen(n.node_id);
    const label = stream ? `Stop (${seconds(stream)} s)` : "Listen";
    if (row.button.textContent !== label) row.button.textContent = label;
    row.button.classList.toggle("on", Boolean(stream));
    row.button.disabled = !stream && !allowed;
    row.button.title = stream || allowed ? "" : `Available during an SOS from ${displayName(n.node_id)}`;
    row.emergency.hidden = Boolean(stream) || allowed;
  });

  fillTable(tables.relays, relays, (n, row) => {
    const stale = n.status === "dropped";
    const link = linkQuality(n.link);
    const carried = stale ? [] : carriedBy(n.node_id);
    setCell(row.cells[1], n.status, `status-${n.status}`);
    setCell(row.cells[2], hopName(n.parent), stale ? "muted" : "");
    setCell(row.cells[3], link.text, stale ? "muted" : link.cls);
    setCell(row.cells[4], stale ? "–" : carried.length === 1 ? "1 person" : `${carried.length} people`,
            stale ? "muted" : "");
    row.cells[4].title = carried.map(displayName).join(", ");
  });

  renderSos();
  renderAlerts();
  updateLogNodeOptions();
  if (currentTab() === "topology") renderTopology();
}

function renderAlerts() {
  const list = $("alerts");
  list.replaceChildren();
  if (!alerts.length) list.innerHTML = '<li class="muted">No alerts</li>';
  for (const a of [...alerts].reverse()) {
    const li = document.createElement("li");
    li.className = a.kind === "sos" ? "sos" : a.kind === "relay_dropped" ? "relay-down" : "";
    const repeats = a.count > 1 ? ` (sent ${a.count}×)` : "";
    li.textContent = `${new Date(a.ts * 1000).toLocaleTimeString()} — ${a.message}${repeats}`;
    if (a.kind === "sos" && a.acked) {
      const ack = document.createElement("span");
      ack.className = "acked";
      const after = a.acked_ts ? Math.max(0, a.acked_ts - a.ts) : null;
      const delay = after === null ? "" : after < 60 ? ` after ${after.toFixed(0)} s` : ` after ${(after / 60).toFixed(0)} min`;
      ack.textContent = ` · ✓ acknowledged${delay}`;
      li.appendChild(ack);
    }
    list.appendChild(li);
  }
}

// ---- tabs ------------------------------------------------------------------------
// The tab lives in the address (#live, #topology), so a refresh or a shared link keeps it.

const TABS = ["live", "topology", "log"];

function currentTab() {
  const tab = location.hash.slice(1);
  return TABS.includes(tab) ? tab : "live";
}

function showTab() {
  const tab = currentTab();
  for (const t of TABS) {
    $(`panel-${t}`).hidden = t !== tab;
    $(`tab-${t}`).setAttribute("aria-selected", String(t === tab));
  }
  topoKey = "";  // redraw when coming back to Topology
  topoCentered = false;
  if (tab === "log" && !logLoaded) loadLog();
  scheduleRender();
}

window.addEventListener("hashchange", showTab);

// ---- topology ----------------------------------------------------------------------
// A tree from the Pi down: relays as squares, people as circles, lines coloured by the
// child's signal to its parent. Redrawn only when something visible changes, so a click
// is never lost to a redraw.

const SVG_NS = "http://www.w3.org/2000/svg";
const TOPO_COL = 84;    // horizontal space per leaf
const TOPO_ROW = 96;    // vertical space per mesh level
const TOPO_PAD = 44;
let topoKey = "";
let topoCentered = false;  // centre the Pi once when the tab opens, then leave scrolling to the user

function svgEl(tag, attrs = {}, parent = null) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
  if (parent) parent.appendChild(el);
  return el;
}

function shortName(text, max = 13) {
  return text.length > max ? `${text.slice(0, max - 1)}…` : text;
}

function renderTopology() {
  const all = [...nodes.values()];
  const key = JSON.stringify(all.map((n) => [n.node_id, n.parent, n.role, n.status, n.name, n.talking,
    linkQuality(n.link).level, listening.has(n.node_id), openSos.has(n.node_id)]).sort());
  if (key === topoKey) return;
  topoKey = key;

  // Children of each node; 0 is the Pi.
  const kids = new Map();
  for (const n of all) {
    if (n.parent === null || n.parent === undefined || (n.parent !== 0 && !nodes.has(n.parent))) continue;
    if (!kids.has(n.parent)) kids.set(n.parent, []);
    kids.get(n.parent).push(n);
  }
  const order = (a, b) => (isRelay(b) - isRelay(a)) || a.node_id - b.node_id;  // relays first: the backbone

  // Leaves get consecutive columns; a parent sits centred over its children.
  const pos = new Map();
  let leaves = 0, depthMax = 0;
  const place = (id, depth) => {
    depthMax = Math.max(depthMax, depth);
    const children = (kids.get(id) || []).filter((c) => !pos.has(c.node_id) && c.node_id !== id).sort(order);
    pos.set(id, null);  // mark visited, so a loop in the parents can't recurse forever
    if (!children.length) { pos.set(id, { x: leaves++, depth }); return; }
    for (const c of children) place(c.node_id, depth + 1);
    const xs = children.map((c) => pos.get(c.node_id).x);
    pos.set(id, { x: (xs[0] + xs[xs.length - 1]) / 2, depth });
  };
  place(0, 0);

  const width = Math.max(320, Math.max(leaves, 1) * TOPO_COL + TOPO_PAD);
  const height = depthMax * TOPO_ROW + TOPO_PAD * 2 + 20;
  const at = (id) => {
    const p = pos.get(id);
    return { x: TOPO_PAD / 2 + (p.x + 0.5) * TOPO_COL, y: TOPO_PAD + p.depth * TOPO_ROW };
  };

  const svg = $("topo");
  svg.replaceChildren();
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.setAttribute("width", width);
  svg.setAttribute("height", height);
  // Shrink to fit the box, but never below 75% (text would get too small): past that, scroll.
  svg.style.minWidth = `${Math.round(width * 0.75)}px`;
  const edges = svgEl("g", {}, svg);
  const marks = svgEl("g", {}, svg);

  // The Pi
  const pi = at(0);
  const piG = svgEl("g", { transform: `translate(${pi.x},${pi.y})` }, marks);
  svgEl("rect", { x: -24, y: -15, width: 48, height: 30, rx: 7, class: "t-pi" }, piG);
  svgEl("text", { y: 5, class: "t-pi-text" }, piG).textContent = "Pi";
  if (!kids.has(0)) svgEl("text", { y: 38, class: "t-sub" }, piG).textContent = "Waiting for nodes…";

  for (const n of all) {
    if (!pos.get(n.node_id) || n.node_id === 0) continue;
    const id = n.node_id;
    const { x, y } = at(id);
    const from = at(n.parent);
    // Lines leave below the parent's label so they never cross it; the Pi has no label below.
    const parentNode = nodes.get(n.parent);
    const fromY = from.y + (n.parent === 0 ? 15 : parentNode?.status === "dropped" ? 52 : 38);
    const stale = n.status === "dropped";
    const link = linkQuality(n.link);
    const midY = (fromY + y) / 2;
    const edge = svgEl("path", {
      d: `M${from.x},${fromY} C${from.x},${midY} ${x},${midY} ${x},${y - 15}`,
      class: `edge ${stale ? "edge-dropped" : `edge-${link.level}`}`,
    }, edges);
    svgEl("title", {}, edge).textContent = stale ? "Not responding" : `Signal ${link.text} dBm`;

    const relay = isRelay(n);
    const sos = openSos.has(id);
    const talking = n.talking && !stale;
    const classes = ["t-node", relay ? "relay" : "worker", stale && "dropped", sos && "sos",
                     listening.has(id) && "listening", talking && "talking"].filter(Boolean).join(" ");
    const g = svgEl("g", { transform: `translate(${x},${y})`, class: classes }, marks);
    if (!relay) {
      g.dataset.node = id;
      g.setAttribute("tabindex", "0");
      g.setAttribute("role", "button");
      g.setAttribute("aria-label", `${displayName(id)}: ${listening.has(id) ? "stop listening"
        : canListen(id) ? "listen" : "emergency listen"}`);
    }
    svgEl("rect", { x: -TOPO_COL / 2 + 2, y: -20, width: TOPO_COL - 4, height: 70, class: "t-hit" }, g);  // name is clickable too
    if (relay) svgEl("rect", { x: -14, y: -14, width: 28, height: 28, rx: 5, class: "t-relay" }, g);
    else svgEl("circle", { r: 14, class: "t-worker" }, g);
    svgEl("text", { y: 4, class: relay ? "t-id-inv" : "t-id" }, g).textContent = id;
    svgEl("text", { y: 31, class: "t-label" }, g).textContent = shortName(displayName(id));
    const sub = sos ? "SOS" : stale ? "not responding" : listening.has(id) ? "listening" : talking ? "talking" : "";
    if (sub) svgEl("text", { y: 45, class: `t-sub ${sos ? "t-sub-sos" : ""}` }, g).textContent = sub;
    const where = relay ? `connects to ${hopName(n.parent)}` : n.near ? `near ${displayName(n.near)}` : "";
    svgEl("title", {}, g).textContent =
      [displayName(id), relay ? `relay ${id}` : `node ${id}`, where, stale ? "not responding" : `${link.text} dBm`]
        .filter(Boolean).join(" · ");
  }

  const wrap = svg.parentElement;
  if (!topoCentered && wrap.clientWidth && kids.has(0)) {
    const scale = svg.getBoundingClientRect().width / width;
    wrap.scrollLeft = Math.max(0, pi.x * scale - wrap.clientWidth / 2);
    topoCentered = true;
  }

  const unplaced = all.filter((n) => !pos.get(n.node_id));
  const note = $("topo-unplaced");
  note.hidden = !unplaced.length;
  note.textContent = unplaced.length
    ? `Not in the tree yet (their parent hasn't been heard): ${unplaced.map((n) => displayName(n.node_id)).join(", ")}`
    : "";
}

function topoActivate(event) {
  const target = event.target.closest("[data-node]");
  if (target) toggleListen(Number(target.dataset.node));
}
$("topo").addEventListener("click", topoActivate);
$("topo").addEventListener("keydown", (event) => {
  if (event.key === "Enter" || event.key === " ") { event.preventDefault(); topoActivate(event); }
});

// ---- SOS banner ------------------------------------------------------------------
// Every unacknowledged SOS stays pinned under the header on every dashboard until
// someone presses Acknowledge. Repeats from the same node only bump its count.

const sosRows = new Map();     // node_id -> { el, text, listen }

function renderSos() {
  const banner = $("sos-banner");
  for (const [id, row] of sosRows) {
    if (!openSos.has(id)) { row.el.remove(); sosRows.delete(id); }
  }
  const open = [...openSos.values()].sort((a, b) => a.ts - b.ts);
  for (const a of open) {
    let row = sosRows.get(a.node_id);
    if (!row) {
      const el = document.createElement("div");
      el.className = "sos-row";
      const text = document.createElement("span");
      text.className = "sos-text";
      const listen = document.createElement("button");
      listen.dataset.node = a.node_id;
      const ack = document.createElement("button");
      ack.className = "ack";
      ack.textContent = "Acknowledge";
      el.append(text, listen, ack);
      row = { el, text, listen, ack };
      sosRows.set(a.node_id, row);
      banner.appendChild(el);
    }
    row.ack.dataset.ack = a.id;
    const repeats = a.count > 1 ? ` · sent ${a.count}×` : "";
    const text = `SOS — ${displayName(a.node_id)} · ${ago(a.ts)}${repeats}`;
    if (row.text.textContent !== text) row.text.textContent = text;
    const label = listening.has(a.node_id) ? "Stop" : "Listen";
    if (row.listen.textContent !== label) row.listen.textContent = label;
  }
  banner.hidden = open.length === 0;
  if (open.length && !alarmTimer) startAlarm();
  else if (!open.length && alarmTimer) stopAlarm();
  const title = open.length ? `(${open.length}) SOS · ${PAGE_TITLE}` : PAGE_TITLE;
  if (document.title !== title) document.title = title;  // visible even in another tab
}

$("sos-banner").addEventListener("click", (event) => {
  const ack = event.target.closest("button[data-ack]");
  if (ack) {
    send({ type: "ack", alert_id: Number(ack.dataset.ack) });
    ack.disabled = true;  // the server's sos_update removes the row for everyone
    return;
  }
  const button = event.target.closest("button[data-node]");
  if (button) toggleListen(Number(button.dataset.node));
});

// One click handler each for the tables and the waveform cards, so they survive updates.
for (const container of [$("people"), $("relays"), $("waves")]) {
  container.addEventListener("click", (event) => {
    const rename = event.target.closest("button[data-rename]");
    if (rename) {
      startRename(Number(rename.dataset.rename));
      return;
    }
    const emergency = event.target.closest("button[data-emergency]");
    if (emergency) {
      openEmergencyDialog(Number(emergency.dataset.emergency));
      return;
    }
    const button = event.target.closest("button[data-node]");
    if (button) toggleListen(Number(button.dataset.node));
  });
}

// ---- log -------------------------------------------------------------------------------
// History saved on the Pi. Loaded in pages of 200 when the tab opens or a filter changes,
// then kept current by the gateway's "log_event" messages. Summary numbers come from the
// gateway (log_stats) so they cover the whole period, not just the rows on screen.

const LOG_PAGE = 200;
const KIND_LABELS = {
  sos: "SOS", node_dropped: "Not responding", relay_dropped: "Relay down", node_rejoined: "Back",
  node_seen: "Connected", renamed: "Renamed", gateway_started: "Gateway started",
  emergency_listen: "Emergency listen", emergency_listen_ended: "Emergency ended",
};
const KIND_GROUPS = {
  sos: ["sos"], lost: ["node_dropped", "relay_dropped"], back: ["node_seen", "node_rejoined"],
  renamed: ["renamed"], gateway: ["gateway_started"],
  emergency: ["emergency_listen", "emergency_listen_ended"],
};
let logEvents = [];
let logMore = false;
let logLoaded = false;
let logReq = 0;          // id of the newest request; older answers are ignored
let olderReq = null;     // the request that fetches the next page of older events
let exportReq = null;
let statsTimer = null;

function logFilters() {
  const node = $("log-node").value;
  const range = $("log-range").value;
  return {
    node_id: node === "" ? null : Number(node),
    kinds: KIND_GROUPS[$("log-kind").value] || null,
    since: range === "" ? null : Date.now() / 1000 - Number(range),
  };
}

function loadLog() {
  logLoaded = true;
  logReq++;
  const f = logFilters();
  send({ type: "log_query", req: logReq, ...f, limit: LOG_PAGE });
  send({ type: "log_stats", req: logReq, node_id: f.node_id, since: f.since });
}

function loadOlder() {
  if (!logEvents.length) return;
  logReq++;
  olderReq = logReq;
  send({ type: "log_query", req: logReq, ...logFilters(), limit: LOG_PAGE,
         before_id: logEvents[logEvents.length - 1].id });
  $("log-more").disabled = true;
}

function onLogResult(msg) {
  if (msg.req === exportReq) { exportReq = null; downloadCsv(msg.events); return; }
  if (msg.req !== logReq) return;  // a newer filter was picked meanwhile
  logEvents = msg.req === olderReq ? logEvents.concat(msg.events) : msg.events;
  logMore = msg.more;
  renderLog();
}

function onLogStats(msg) {
  const st = msg.stats;
  $("st-sos").textContent = st.sos;
  // Only shown when it matters: an SOS nobody has acknowledged.
  $("st-sos-note").hidden = !st.sos_open;
  $("st-sos-note").textContent = `${st.sos_open} not acknowledged`;
  $("st-ack").textContent = st.ack_median_s === null ? "–" : duration(st.ack_median_s);
  $("st-ack-tile").title = st.ack_median_s === null ? "No acknowledged SOS in this period"
    : `Average ${duration(st.ack_avg_s)} · slowest ${duration(st.ack_max_s)}`;
  $("st-lost").textContent = st.counts.node_dropped || 0;
  $("st-relay").textContent = st.counts.relay_dropped || 0;
}

function refreshStatsSoon() {
  clearTimeout(statsTimer);
  statsTimer = setTimeout(() => {
    const f = logFilters();
    send({ type: "log_stats", req: logReq, node_id: f.node_id, since: f.since });
  }, 1000);
}

function matchesFilters(event) {
  const f = logFilters();
  return (f.node_id === null || event.node_id === f.node_id)
    && (!f.kinds || f.kinds.includes(event.kind))
    && (f.since === null || event.ts >= f.since);
}

function onLogEvent(event) {
  if (!logLoaded) return;
  if (matchesFilters(event) && !logEvents.some((e) => e.id === event.id)) {
    logEvents.unshift(event);
    if (currentTab() === "log") renderLog();
  }
  refreshStatsSoon();
}

function onLogUpdate(event) {  // an SOS was repeated or acknowledged
  const i = logEvents.findIndex((e) => e.id === event.id);
  if (i >= 0) { logEvents[i] = { ...logEvents[i], ...event }; if (currentTab() === "log") renderLog(); }
  if (logLoaded) refreshStatsSoon();
}

function duration(seconds) {
  if (seconds === null || seconds === undefined) return "–";
  if (seconds < 60) return `${Math.round(seconds)} s`;
  const m = Math.floor(seconds / 60), s = Math.round(seconds % 60);
  return m < 60 ? `${m} min ${s} s` : `${Math.floor(m / 60)} h ${m % 60} min`;
}

function when(ts) {
  const d = new Date(ts * 1000);
  const today = d.toDateString() === new Date().toDateString();
  return today ? d.toLocaleTimeString()
    : d.toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function whoName(event) {
  return event.node_id === null || event.node_id === undefined ? "Gateway" : displayName(event.node_id);
}

// SOS outcome as text: acknowledged after N, or still open.
function ackText(event) {
  if (event.kind !== "sos") return "";
  if (event.acked) return event.acked_ts ? `acknowledged after ${duration(event.acked_ts - event.ts)}` : "acknowledged";
  return "not acknowledged";
}

function renderLog() {
  const body = $("log-rows");
  body.replaceChildren();
  if (!logEvents.length) {
    body.innerHTML = '<tr><td colspan="4" class="muted">No events match these filters.</td></tr>';
  }
  for (const e of logEvents) {
    const tr = document.createElement("tr");
    const time = document.createElement("td");
    time.textContent = when(e.ts);
    time.className = "log-time";
    const type = document.createElement("td");
    const badge = document.createElement("span");
    badge.className = `kind kind-${e.kind}`;
    badge.textContent = KIND_LABELS[e.kind] || e.kind;
    type.appendChild(badge);
    const who = document.createElement("td");
    who.textContent = whoName(e);
    const details = document.createElement("td");
    details.textContent = e.message + (e.count > 1 ? ` (sent ${e.count}×)` : "");
    const ack = ackText(e);
    if (ack) {
      const span = document.createElement("span");
      span.className = e.acked ? "acked" : "not-acked";
      span.textContent = ` · ${e.acked ? "✓ " : ""}${ack}`;
      details.appendChild(span);
    }
    tr.append(time, type, who, details);
    body.appendChild(tr);
  }
  $("log-shown").textContent = logEvents.length
    ? `Showing ${logEvents.length} event${logEvents.length === 1 ? "" : "s"}${logMore ? " (newest first)" : ""}.` : "";
  $("log-more").hidden = !logMore;
  $("log-more").disabled = false;
}

// The "Who" list: everyone the dashboard knows, people first, then relays.
let logNodeKey = "";
function updateLogNodeOptions() {
  const list = [...nodes.values()].sort((a, b) => (isRelay(a) - isRelay(b)) || a.node_id - b.node_id);
  const key = JSON.stringify(list.map((n) => [n.node_id, displayName(n.node_id)]));
  if (key === logNodeKey) return;
  logNodeKey = key;
  const select = $("log-node");
  const chosen = select.value;
  select.replaceChildren(new Option("Everyone", ""));
  for (const n of list) {
    select.appendChild(new Option(`${displayName(n.node_id)}${n.name ? ` (${isRelay(n) ? "relay" : "node"} ${n.node_id})` : ""}`,
                                  String(n.node_id)));
  }
  if (chosen && !list.some((n) => String(n.node_id) === chosen)) {
    select.appendChild(new Option(displayName(Number(chosen)), chosen));  // keep a filter on someone who left
  }
  select.value = chosen;
}

// ---- CSV export: asks the gateway for every matching event, then saves a file ----

function exportLog() {
  exportReq = `export-${Date.now()}`;
  send({ type: "log_query", req: exportReq, ...logFilters(), limit: 50000 });
  $("log-export").disabled = true;
  $("log-export").textContent = "Preparing…";
}

function csvCell(value) {
  const text = value === null || value === undefined ? "" : String(value);
  return /[",\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

function downloadCsv(events) {
  const header = ["id", "time_utc", "time_local", "type", "node_id", "who", "details",
                  "acknowledged", "seconds_to_acknowledge", "times_sent"];
  const rows = events.slice().reverse().map((e) => [
    e.id, new Date(e.ts * 1000).toISOString(), new Date(e.ts * 1000).toLocaleString(),
    KIND_LABELS[e.kind] || e.kind, e.node_id ?? "", whoName(e), e.message,
    e.kind === "sos" ? (e.acked ? "yes" : "no") : "",
    e.kind === "sos" && e.acked && e.acked_ts ? (e.acked_ts - e.ts).toFixed(1) : "",
    e.kind === "sos" ? (e.count || 1) : "",
  ]);
  // The BOM makes Excel read names with accents correctly.
  const csv = "﻿" + [header, ...rows].map((r) => r.map(csvCell).join(",")).join("\r\n");
  const url = URL.createObjectURL(new Blob([csv], { type: "text/csv;charset=utf-8" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = `oratio-log-${new Date().toISOString().slice(0, 10)}.csv`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  $("log-export").disabled = false;
  $("log-export").textContent = "Export CSV";
}

for (const id of ["log-node", "log-kind", "log-range"]) $(id).addEventListener("change", loadLog);
$("log-more").addEventListener("click", loadOlder);
$("log-export").addEventListener("click", exportLog);

// ---- theme ---------------------------------------------------------------------
// Cycles auto -> light -> dark. "auto" follows the computer's setting. Saved per device.

const THEMES = ["auto", "light", "dark"];

function currentTheme() {
  return document.documentElement.dataset.theme || "auto";
}

function applyTheme(theme) {
  if (theme === "auto") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = theme;
  $("theme").textContent = `Theme: ${theme}`;
  try {
    if (theme === "auto") localStorage.removeItem("oratio-theme");
    else localStorage.setItem("oratio-theme", theme);
  } catch (e) { /* storage unavailable (private window): the choice lasts until reload */ }
}

$("theme").addEventListener("click", () => {
  applyTheme(THEMES[(THEMES.indexOf(currentTheme()) + 1) % THEMES.length]);
});
applyTheme(currentTheme());

// ---- SOS alarm sound ---------------------------------------------------------------
// Beeps every few seconds while any SOS is unacknowledged. The tone is generated with
// Web Audio, so there is no sound file to ship. Browsers only allow a page to make
// sound after someone has clicked or typed on it, so until then the Sound button
// says "click to enable"; any click on the page unlocks it. On/off is saved per device.

const ALARM_EVERY_MS = 3000;
let audioCtx = null;
let alarmTimer = null;
let lockedAtPress = false;     // was sound still blocked when the current click started?
let soundOn = true;
try { soundOn = localStorage.getItem("oratio-sound") !== "off"; } catch (e) { /* default: on */ }

function audioReady() {
  return audioCtx && audioCtx.state === "running";
}

function unlockAudio() {
  if (!audioCtx) {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    audioCtx = new Ctx();
    audioCtx.onstatechange = () => {
      updateSoundButton();
      if (audioReady() && alarmTimer) beep();  // SOS already open: sound it now, not on the next cycle
    };
  }
  if (audioCtx.state === "suspended") audioCtx.resume().catch(() => {});
  updateSoundButton();
}

function beep() {
  if (!soundOn || !audioReady()) return;
  const t0 = audioCtx.currentTime;
  // Three short two-tone beeps (high-low-high), about 0.6 s in total.
  [[0, 988], [0.2, 740], [0.4, 988]].forEach(([at, freq]) => {
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = "square";
    osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.0001, t0 + at);
    gain.gain.exponentialRampToValueAtTime(0.15, t0 + at + 0.01);   // short fade in/out: no clicks
    gain.gain.exponentialRampToValueAtTime(0.0001, t0 + at + 0.16);
    osc.connect(gain).connect(audioCtx.destination);
    osc.start(t0 + at);
    osc.stop(t0 + at + 0.17);
  });
}

function startAlarm() {
  beep();
  alarmTimer = setInterval(beep, ALARM_EVERY_MS);
}

function stopAlarm() {
  clearInterval(alarmTimer);
  alarmTimer = null;
}

function updateSoundButton() {
  const button = $("sound");
  const locked = soundOn && !audioReady();
  const label = !soundOn ? "Sound: off" : locked ? "Sound: click to enable" : "Sound: on";
  if (button.textContent !== label) button.textContent = label;
  button.classList.toggle("locked", locked);
}

$("sound").addEventListener("click", () => {
  if (lockedAtPress) return;  // this click only unlocked sound; it shouldn't also turn it off
  soundOn = !soundOn;
  try { localStorage.setItem("oratio-sound", soundOn ? "on" : "off"); } catch (e) { /* this session only */ }
  updateSoundButton();
  if (soundOn) beep();  // let the user hear what the alarm sounds like
});
// Any click or key press anywhere on the page counts as permission to play sound.
for (const type of ["pointerdown", "keydown"]) {
  document.addEventListener(type, () => {
    lockedAtPress = soundOn && !audioReady();
    unlockAudio();
  }, { capture: true });
}
updateSoundButton();

showTab();
setInterval(scheduleRender, 250);  // keep "last seen" and the seconds counters fresh
connect();
