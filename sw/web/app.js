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
    render();
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
  render();
}

function setConn(text, cls) {
  $("conn").textContent = text;
  $("conn").className = `pill ${cls}`;
}

function ago(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  return s < 60 ? `${s.toFixed(0)} s ago` : `${(s / 60).toFixed(0)} min ago`;
}

function render() {
  const rows = [...nodes.values()].sort((a, b) => a.node_id - b.node_id);
  const active = rows.filter((n) => n.status === "active").length;
  $("counts").textContent = rows.length ? `${active} active / ${rows.length}` : "";
  const body = $("nodes");
  body.replaceChildren();
  if (!rows.length) {
    body.innerHTML = '<tr><td colspan="6" class="muted">Waiting for nodes…</td></tr>';
  }
  for (const n of rows) {
    const tr = document.createElement("tr");
    const pos = n.pos ? `${n.pos[0].toFixed(1)}, ${n.pos[1].toFixed(1)}` : "–";
    const cells = [n.node_id, n.status, n.link ?? "–", pos, ago(n.last_seen)];
    cells.forEach((value, i) => {
      const td = document.createElement("td");
      td.textContent = value;
      if (i === 1) td.className = `status-${n.status}`;
      tr.appendChild(td);
    });
    const td = document.createElement("td");
    const button = document.createElement("button");
    button.textContent = listening.has(n.node_id) ? `Stop (${frames.get(n.node_id) || 0})` : "Listen";
    button.onclick = () => toggleListen(n.node_id);
    td.appendChild(button);
    tr.appendChild(td);
    body.appendChild(tr);
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

setInterval(render, 1000);  // keep "last seen" fresh
connect();
