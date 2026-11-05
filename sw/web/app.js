// Minimal live view for the gateway WebSocket server (starting point for task 128).
// Connects to ws://<same host>:8000/ws, shows nodes and alerts, reconnects automatically.
// Audio playback isn't implemented yet: "Listen" only counts the frames received.

const WS_URL = `ws://${location.hostname || "localhost"}:8000/ws`;
const nodes = new Map();      // node_id -> node
const frames = new Map();     // node_id -> audio frames received
const listening = new Set();  // node ids this page listens to
let alerts = [];
let socket = null;

const $ = (id) => document.getElementById(id);

function connect() {
  setConn("connecting…", "");
  socket = new WebSocket(WS_URL);
  socket.binaryType = "arraybuffer";

  socket.onopen = () => {
    setConn("live", "ok");
    for (const id of listening) send({ type: "listen", node_id: id });  // resume after reconnect
  };
  socket.onclose = () => {
    setConn("reconnecting…", "bad");
    setTimeout(connect, 1500);
  };
  socket.onmessage = (event) => {
    if (event.data instanceof ArrayBuffer) {
      const id = new DataView(event.data).getUint16(0);  // 2-byte big-endian node id, then PCM
      frames.set(id, (frames.get(id) || 0) + 1);
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

function toggleListen(id) {
  if (listening.has(id)) {
    listening.delete(id);
    send({ type: "stop", node_id: id });
  } else {
    listening.add(id);
    send({ type: "listen", node_id: id });
  }
  scheduleRender();
}

function setConn(text, cls) {
  $("conn").textContent = text;
  $("conn").className = `pill ${cls}`;
}

function ago(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  return s < 60 ? `${s.toFixed(0)} s ago` : `${(s / 60).toFixed(0)} min ago`;
}

// Rows are created once per node and then updated in place. Rebuilding the table on every
// update replaced the buttons several times a second, so clicks landing between mouse-down and
// mouse-up were lost.
const rows = new Map();  // node_id -> { tr, cells: [td...], button }
let renderQueued = false;

function scheduleRender() {
  if (!renderQueued) {
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }
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
      const label = listening.has(n.node_id) ? `Stop (${frames.get(n.node_id) || 0})` : "Listen";
      if (button.textContent !== label) button.textContent = label;
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

// One click handler for the whole table, so it survives row updates.
$("nodes").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-node]");
  if (button) toggleListen(Number(button.dataset.node));
});

setInterval(scheduleRender, 1000);  // keep "last seen" fresh
connect();
