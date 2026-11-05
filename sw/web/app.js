// Live view for the gateway WebSocket server (starting point for task 128).
// Connects to ws://<same host>:8000/ws, shows nodes and alerts, reconnects automatically.
// "Listen" streams a node's audio frames and draws them as a live waveform.
// Sound playback isn't implemented yet.

const WS_URL = `ws://${location.hostname || "localhost"}:8000/ws`;
const FRAME_SECONDS = 0.02;    // each audio frame is 20 ms
const WAVE_HISTORY = 200;      // bars kept per waveform (200 x 20 ms = 4 s)
const SILENCE_MS = 500;        // no frame for this long -> "no audio"

const nodes = new Map();       // node_id -> node
const listening = new Map();   // node_id -> { frames, levels: Float32Array, head, lastFrameAt, card }
let alerts = [];
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
    } else if (msg.type === "node_update") {
      nodes.set(msg.node.node_id, msg.node);
    } else if (msg.type === "alert") {
      alerts.push(msg.alert);
      alerts = alerts.slice(-50);
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
      <strong>Node ${id}</strong>
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

  for (const [, stream] of listening) {
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

function ago(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  return s < 60 ? `${s.toFixed(0)} s ago` : `${(s / 60).toFixed(0)} min ago`;
}

function rowFor(id) {
  let row = rows.get(id);
  if (!row) {
    const tr = document.createElement("tr");
    const cells = Array.from({ length: 5 }, () => tr.appendChild(document.createElement("td")));
    const button = document.createElement("button");
    button.dataset.node = id;
    tr.appendChild(document.createElement("td")).appendChild(button);
    row = { tr, cells, button };
    rows.set(id, row);
  }
  return row;
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
      const { tr, cells, button } = rowFor(n.node_id);
      const pos = n.pos ? `${n.pos[0].toFixed(1)}, ${n.pos[1].toFixed(1)}` : "–";
      const values = [n.node_id, n.status, n.link ?? "–", pos, ago(n.last_seen)];
      values.forEach((value, i) => {
        if (cells[i].textContent !== String(value)) cells[i].textContent = value;
      });
      cells[1].className = `status-${n.status}`;
      const stream = listening.get(n.node_id);
      const label = stream ? `Stop (${seconds(stream)} s)` : "Listen";
      if (button.textContent !== label) button.textContent = label;
      button.classList.toggle("on", Boolean(stream));
      if (body.children[index] !== tr) body.insertBefore(tr, body.children[index] || null);
    });
  }

  const list = $("alerts");
  list.replaceChildren();
  if (!alerts.length) list.innerHTML = '<li class="muted">No alerts</li>';
  for (const a of [...alerts].reverse()) {
    const li = document.createElement("li");
    li.className = a.kind === "sos" ? "sos" : "";
    li.textContent = `${new Date(a.ts * 1000).toLocaleTimeString()} — ${a.message}`;
    list.appendChild(li);
  }
}

// One click handler each for the table and the waveform cards, so they survive updates.
for (const container of [$("nodes"), $("waves")]) {
  container.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-node]");
    if (button) toggleListen(Number(button.dataset.node));
  });
}

setInterval(scheduleRender, 250);  // keep "last seen" and the seconds counters fresh
connect();
