// Live view for the gateway WebSocket server (starting point for task 128).
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
    for (const id of listening.keys()) send({ type: "listen", node_id: id });  // resume after reconnect
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
    } else if (msg.type === "node_update") {
      nodes.set(msg.node.node_id, msg.node);
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

function toggleListen(id) {
  if (listening.has(id)) {
    listening.get(id).card.remove();
    listening.delete(id);
    send({ type: "stop", node_id: id });
  } else {
    listening.set(id, { frames: 0, levels: new Float32Array(WAVE_HISTORY), head: 0,
                        lastFrameAt: 0, card: makeWaveCard(id) });
    send({ type: "listen", node_id: id });
    requestAnimationFrame(drawWaves);
  }
  scheduleRender();
}

function seconds(stream) {
  return (stream.frames * FRAME_SECONDS).toFixed(1);
}

function makeWaveCard(id) {
  const card = document.createElement("div");
  card.className = "wave-card";
  card.innerHTML = `
    <div class="wave-head">
      <strong class="wave-name"></strong>
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
      silent && stream.frames ? `${seconds(stream)} s · no audio`
        : silent ? "waiting for audio…" : `${seconds(stream)} s received`;
  }
  requestAnimationFrame(drawWaves);
}

// ---- table and alerts ----------------------------------------------------------

// Rows are created once per node and then updated in place. Rebuilding the table on every
// update replaced the buttons several times a second, so clicks between mouse-down and
// mouse-up were lost.
const rows = new Map();  // node_id -> { tr, cells: [td...], button }
let renderQueued = false;

function scheduleRender() {
  if (!renderQueued) {
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }
}

// Signal strength (RSSI to the node's parent) -> label and colour class.
function linkQuality(dbm) {
  if (dbm === null || dbm === undefined) return { text: "–", cls: "" };
  if (dbm >= -50) return { text: `${dbm} · excellent`, cls: "link-good" };
  if (dbm >= -67) return { text: `${dbm} · good`, cls: "link-good" };
  if (dbm >= -80) return { text: `${dbm} · weak`, cls: "link-weak" };
  return { text: `${dbm} · poor`, cls: "link-poor" };
}

function ago(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  return s < 60 ? `${s.toFixed(0)} s ago` : `${(s / 60).toFixed(0)} min ago`;
}

// Name shown for a node: the person's name if one was given, otherwise "Node 3".
function displayName(id) {
  return nodes.get(id)?.name || `Node ${id}`;
}

function rowFor(id) {
  let row = rows.get(id);
  if (!row) {
    const tr = document.createElement("tr");
    const cells = Array.from({ length: 5 }, () => tr.appendChild(document.createElement("td")));
    const button = document.createElement("button");
    button.dataset.node = id;
    tr.appendChild(document.createElement("td")).appendChild(button);
    row = { tr, cells, button, nameEl: null, idEl: null };
    rows.set(id, row);
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
  rename.setAttribute("aria-label", `Rename node ${id}`);
  rename.textContent = "✎";
  row.cells[0].replaceChildren(nameEl, idEl, rename);
  row.nameEl = nameEl;
  row.idEl = idEl;
}

// ---- renaming --------------------------------------------------------------------

let editingId = null;

function startRename(id) {
  if (editingId !== null) return;
  editingId = id;
  const row = rows.get(id);
  const input = document.createElement("input");
  input.className = "name-input";
  input.maxLength = 32;
  input.placeholder = "Name, e.g. Alex";
  input.value = nodes.get(id)?.name || "";
  input.setAttribute("aria-label", `Name for node ${id}`);
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

function render() {
  const sorted = [...nodes.values()].sort((a, b) => a.node_id - b.node_id);
  const active = sorted.filter((n) => n.status === "active").length;
  $("counts").textContent = sorted.length ? `${active} active / ${sorted.length}` : "";

  const body = $("nodes");
  if (!sorted.length) {
    rows.clear();
    body.innerHTML = '<tr><td colspan="6" class="muted">Waiting for nodes…</td></tr>';
  } else {
    if (!rows.size) body.replaceChildren();  // remove the "waiting" row once
    sorted.forEach((n, index) => {
      const row = rowFor(n.node_id);
      const { tr, cells, button } = row;
      const pos = n.pos ? `${n.pos[0].toFixed(1)}, ${n.pos[1].toFixed(1)}` : "–";
      const link = linkQuality(n.link);
      if (editingId !== n.node_id) {
        const name = displayName(n.node_id);
        const suffix = n.name ? ` · node ${n.node_id}` : "";
        if (row.nameEl.textContent !== name) row.nameEl.textContent = name;
        if (row.idEl.textContent !== suffix) row.idEl.textContent = suffix;
      }
      const values = [null, n.status, link.text, pos, ago(n.last_seen)];
      values.forEach((value, i) => {
        if (i > 0 && cells[i].textContent !== String(value)) cells[i].textContent = value;
      });
      cells[1].className = `status-${n.status}`;
      // A dropped node's last reading is stale, so it is shown greyed out.
      cells[2].className = n.status === "dropped" ? "muted" : link.cls;
      const stream = listening.get(n.node_id);
      const label = stream ? `Stop (${seconds(stream)} s)` : "Listen";
      if (button.textContent !== label) button.textContent = label;
      button.classList.toggle("on", Boolean(stream));
      if (body.children[index] !== tr) body.insertBefore(tr, body.children[index] || null);
    });
  }

  renderSos();

  const list = $("alerts");
  list.replaceChildren();
  if (!alerts.length) list.innerHTML = '<li class="muted">No alerts</li>';
  for (const a of [...alerts].reverse()) {
    const li = document.createElement("li");
    li.className = a.kind === "sos" ? "sos" : "";
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

// One click handler each for the table and the waveform cards, so they survive updates.
for (const container of [$("nodes"), $("waves")]) {
  container.addEventListener("click", (event) => {
    const rename = event.target.closest("button[data-rename]");
    if (rename) {
      startRename(Number(rename.dataset.rename));
      return;
    }
    const button = event.target.closest("button[data-node]");
    if (button) toggleListen(Number(button.dataset.node));
  });
}

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

setInterval(scheduleRender, 250);  // keep "last seen" and the seconds counters fresh
connect();
