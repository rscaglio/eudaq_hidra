<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>EUDAQ Run Dashboard</title>
<meta name="viewport" content="width=device-width, initial-scale=1">

<style>
:root {
  --bg: #07111f;
  --panel: #101c2d;
  --panel2: #14243a;
  --text: #eaf2ff;
  --muted: #8fa6c2;
  --ok: #27d17f;
  --warn: #ffbf47;
  --bad: #ff5c7a;
  --blue: #48a6ff;
  --border: rgba(255,255,255,.09);
}

* { box-sizing: border-box; }

body {
  margin: 0;
  background: radial-gradient(circle at top, #102744, var(--bg));
  color: var(--text);
  font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}

header {
  padding: 22px 28px;
  border-bottom: 1px solid var(--border);
  display: flex;
  justify-content: space-between;
  gap: 16px;
  align-items: center;
}

h1 {
  margin: 0;
  font-size: 28px;
  letter-spacing: .5px;
}

.status-dot {
  display: inline-block;
  width: 12px;
  height: 12px;
  border-radius: 50%;
  background: var(--bad);
  box-shadow: 0 0 18px var(--bad);
  margin-right: 8px;
}

.status-dot.live {
  background: var(--ok);
  box-shadow: 0 0 18px var(--ok);
}

.status-dot.warn {
  background: var(--warn);
  box-shadow: 0 0 18px var(--warn);
}

.status-dot.bad {
  background: var(--bad);
  box-shadow: 0 0 18px var(--bad);
}

.sub {
  color: var(--muted);
  font-size: 14px;
}

main {
  padding: 24px;
  display: grid;
  gap: 20px;
}

.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 16px;
}

.card, .device {
  background: linear-gradient(180deg, var(--panel), var(--panel2));
  border: 1px solid var(--border);
  border-radius: 18px;
  padding: 18px;
  box-shadow: 0 14px 35px rgba(0,0,0,.22);
}

.metric-label {
  color: var(--muted);
  font-size: 13px;
  text-transform: uppercase;
  letter-spacing: .08em;
}

.metric-value {
  font-size: 30px;
  font-weight: 800;
  margin-top: 6px;
  overflow-wrap: anywhere;
}

.metric-value.time {
  font-size: 22px;
}

.devices {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  gap: 16px;
}

.device-head {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  align-items: start;
  margin-bottom: 14px;
}

.device-name {
  font-size: 20px;
  font-weight: 800;
}

.badge {
  padding: 5px 10px;
  border-radius: 999px;
  font-weight: 700;
  font-size: 13px;
  background: rgba(255,255,255,.08);
}

.badge.started { color: var(--ok); }
.badge.warn { color: var(--warn); }
.badge.bad { color: var(--bad); }

.tags {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
  gap: 10px;
}

.tag {
  background: rgba(255,255,255,.055);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 10px;
}

.tag-k {
  color: var(--muted);
  font-size: 12px;
}

.tag-v {
  font-size: 20px;
  font-weight: 750;
  margin-top: 3px;
  overflow-wrap: anywhere;
}

.footer {
  color: var(--muted);
  font-size: 13px;
}

.error {
  color: var(--bad);
  font-weight: 700;
}

.stale {
  color: var(--warn);
}

.stopped {
  color: var(--bad);
}
</style>
</head>

<body>
<header>
  <div>
    <h1><span id="liveDot" class="status-dot"></span>EUDAQ Control-Room Dashboard</h1>
    <div class="sub" id="subtitle">Waiting for data...</div>
  </div>
  <div class="sub" id="clock"></div>
</header>

<main>
  <section class="cards">
    <div class="card">
      <div class="metric-label">Run</div>
      <div class="metric-value" id="run">—</div>
    </div>

    <div class="card">
      <div class="metric-label">Start time</div>
      <div class="metric-value time" id="startTime">—</div>
    </div>

    <div class="card">
      <div class="metric-label">Stop time</div>
      <div class="metric-value time" id="stopTime">—</div>
    </div>

    <div class="card">
      <div class="metric-label">Devices</div>
      <div class="metric-value" id="deviceCount">—</div>
    </div>

    <div class="card">
      <div class="metric-label">Total EventN</div>
      <div class="metric-value" id="totalEvents">—</div>
    </div>

    <div class="card">
      <div class="metric-label">DAQ age</div>
      <div class="metric-value" id="age">—</div>
    </div>
  </section>

  <section class="devices" id="devices"></section>

  <div class="footer" id="footer"></div>
</main>

<script>
const API = "api.php";
const POLL_MS = 1000;
const HIDDEN_TAGS = new Set(["MonitorEventN"]);

function nsToDate(ns) {
  if (!ns) return null;
  return new Date(Number(BigInt(ns) / 1000000n));
}

function fmtAge(seconds) {
  if (!Number.isFinite(seconds)) return "—";
  if (seconds < 1) return "<1 s";
  if (seconds < 60) return `${seconds.toFixed(0)} s`;
  return `${(seconds / 60).toFixed(1)} min`;
}

function stateClass(state) {
  const s = String(state || "").toLowerCase();

  if (s.includes("started") || s.includes("running")) return "started";
  if (
    s.includes("error") ||
    s.includes("failed") ||
    s.includes("dead") ||
    s.includes("stopped")
  ) return "bad";

  if (s.includes("warn")) return "warn";

  return "";
}

function tagValue(tags, key) {
  return tags && tags[key] !== undefined ? tags[key] : null;
}

function escapeHtml(x) {
  return String(x)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function computeGlobalStatus(deviceNames, devices, now, daqAgeSec) {
  if (!Number.isFinite(daqAgeSec) || daqAgeSec > 10) {
    return "bad";
  }

  let status = "live";

  for (const name of deviceNames) {
    const dev = devices[name];
    const state = String(dev.state || "").toLowerCase();

    const lastUpdate = nsToDate(dev.last_update_unix_ns);
    const devAge = lastUpdate ? (now - lastUpdate) / 1000 : Infinity;

    if (
      state.includes("stopped") ||
      state.includes("error") ||
      state.includes("failed") ||
      state.includes("dead") ||
      devAge > 15
    ) {
      return "bad";
    }

    if (devAge > 5 || state.includes("warn")) {
      status = "warn";
    }
  }

  return status;
}

function render(data) {
  if (!data.ok) {
    throw new Error(data.error || "API error");
  }

  const entry = data.entry;

  if (!entry) {
    document.getElementById("subtitle").innerHTML =
      `<span class="error">No valid JSON line found</span>`;
    return;
  }

  const devices = entry.devices || {};
  const deviceNames = Object.keys(devices);

  const run = entry.run ?? "—";
  const daqDate = nsToDate(entry.time_unix_ns);
  const now = new Date();
  const ageSec = daqDate ? (now - daqDate) / 1000 : NaN;

  const startDate = data.run_start_unix_ns
    ? nsToDate(data.run_start_unix_ns)
    : null;

  const anyStopped = deviceNames.some(name =>
    String(devices[name].state || "").toLowerCase().includes("stopped")
  );

  const stopDate = anyStopped ? nsToDate(entry.time_unix_ns) : null;

  document.getElementById("run").textContent = run;
  document.getElementById("deviceCount").textContent = deviceNames.length;
  document.getElementById("startTime").textContent =
    startDate ? startDate.toLocaleString() : "—";

  const stopTimeEl = document.getElementById("stopTime");
  stopTimeEl.textContent = stopDate ? stopDate.toLocaleString() : "running";
  stopTimeEl.classList.toggle("stopped", Boolean(stopDate));

  const ageEl = document.getElementById("age");
  ageEl.textContent = fmtAge(ageSec);
  ageEl.className = ageSec > 10 ? "metric-value stale" : "metric-value";

  let totalEvents = 0;
  for (const name of deviceNames) {
    const ev = Number(tagValue(devices[name].tags, "EventN"));
    if (Number.isFinite(ev)) {
      totalEvents += ev;
    }
  }

  document.getElementById("totalEvents").textContent = totalEvents;

  const globalStatus = computeGlobalStatus(deviceNames, devices, now, ageSec);
  const dot = document.getElementById("liveDot");
  dot.classList.remove("live", "warn", "bad");
  dot.classList.add(globalStatus);

  document.getElementById("subtitle").textContent =
    `File: ${data.file} · Last DAQ update: ${daqDate ? daqDate.toLocaleString() : "unknown"}`;

  const container = document.getElementById("devices");
  container.innerHTML = "";

  for (const name of deviceNames.sort()) {
    const dev = devices[name];
    const tags = Object.fromEntries(
    	  Object.entries(dev.tags || {}).filter(([key, value]) => !HIDDEN_TAGS.has(key)));
	  
    const lastUpdate = nsToDate(dev.last_update_unix_ns);
    const devAge = lastUpdate ? (now - lastUpdate) / 1000 : NaN;

    const card = document.createElement("article");
    card.className = "device";

    const tagHtml = Object.entries(tags).map(([k, v]) => `
      <div class="tag">
        <div class="tag-k">${escapeHtml(k)}</div>
        <div class="tag-v">${escapeHtml(v)}</div>
      </div>
    `).join("");

    card.innerHTML = `
      <div class="device-head">
        <div>
          <div class="device-name">${escapeHtml(name)}</div>
          <div class="sub">
            last update: ${lastUpdate ? lastUpdate.toLocaleTimeString() : "unknown"}
            · age ${fmtAge(devAge)}
          </div>
        </div>
        <div class="badge ${stateClass(dev.state)}">${escapeHtml(dev.state || "Unknown")}</div>
      </div>
      <div class="tags">
        ${tagHtml || `<div class="sub">No tags</div>`}
      </div>
    `;

    container.appendChild(card);
  }

  document.getElementById("footer").textContent =
    `File size: ${data.file_size} bytes · Server time: ${new Date(data.server_time * 1000).toLocaleString()}`;
}

async function poll() {
  try {
    const res = await fetch(API, { cache: "no-store" });
    const data = await res.json();
    render(data);
  } catch (err) {
    document.getElementById("subtitle").innerHTML =
      `<span class="error">${escapeHtml(err.message)}</span>`;

    const dot = document.getElementById("liveDot");
    dot.classList.remove("live", "warn");
    dot.classList.add("bad");
  }
}

setInterval(() => {
  document.getElementById("clock").textContent = new Date().toLocaleString();
}, 500);

poll();
setInterval(poll, POLL_MS);
</script>
</body>
</html>