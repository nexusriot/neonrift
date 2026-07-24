const $ = (id) => document.getElementById(id);

// ---- token (optional auth) -------------------------------------------------
let token = localStorage.getItem("neonrift_token") || "";
$("token").value = token;
$("saveToken").addEventListener("click", () => {
  token = $("token").value.trim();
  localStorage.setItem("neonrift_token", token);
  msg("wifiMsg", token ? "token saved" : "token cleared");
});

function authHeaders(extra = {}) {
  return token ? { ...extra, Authorization: "Bearer " + token } : extra;
}

async function apiGet(path) {
  const res = await fetch(path, { cache: "no-store", headers: authHeaders() });
  return res.json();
}
async function apiSend(method, path, body) {
  const res = await fetch(path, {
    method,
    cache: "no-store",
    headers: authHeaders(body ? { "Content-Type": "application/json" } : {}),
    body: body ? JSON.stringify(body) : undefined,
  });
  return res.json();
}

function msg(id, text, isErr = false) {
  const el = $(id);
  el.textContent = text || "";
  el.style.color = isErr ? "#ff8aa0" : "";
}

// ---- live telemetry (WebSocket, fallback to polling) -----------------------
const fmtBytes = (n) =>
  n == null ? "—" : n < 1024 ? n + " B" : n < 1048576 ? (n / 1024).toFixed(1) + " KB" : (n / 1048576).toFixed(2) + " MB";
const fmtUptime = (s) => {
  if (s == null) return "—";
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return `${d ? d + "d " : ""}${h}h ${m}m ${s % 60}s`;
};

let lastInfo = null;

function render(info) {
  lastInfo = info;
  const w = info.wifi || {};
  const connected = w.status === 3 && !info.ap_mode;

  const badge = $("connBadge");
  if (info.ap_mode) {
    badge.textContent = "setup mode (AP)";
    badge.className = "badge warn";
  } else if (connected) {
    badge.textContent = `online · ${w.ssid || ""}`;
    badge.className = "badge ok";
  } else {
    badge.textContent = "offline";
    badge.className = "badge err";
  }

  const rows = [
    ["IP", w.ip || (info.ap_mode ? w.ap_ip : "") || "—"],
    ["SSID", w.ssid || (info.ap_mode ? w.ap_ssid : "") || "—"],
    ["RSSI", w.rssi != null ? w.rssi + " dBm" : "—"],
    ["MAC", w.mac || "—"],
    ["Uptime", fmtUptime(info.uptime_s)],
    ["Free heap", fmtBytes(info.heap_free)],
    ["Min heap", fmtBytes(info.heap_min_free)],
    ["PSRAM free", fmtBytes(info.psram_free)],
    ["Flash", fmtBytes(info.flash_size)],
    ["CPU", info.cpu_freq_mhz ? info.cpu_freq_mhz + " MHz" : "—"],
    ["Chip ID", info.chip_id || "—"],
    ["SDK", info.sdk || "—"],
    ["Time (UTC)", info.time_synced ? info.time_iso : "not synced"],
  ];
  $("stats").innerHTML = rows
    .map(([k, v]) => `<div class="stat"><span class="k">${k}</span><span class="v">${v}</span></div>`)
    .join("");

  setLed(info.led);
  if (!$("rawOut").classList.contains("hidden")) {
    $("rawOut").textContent = JSON.stringify(info, null, 2);
  }
}

let ws = null;
function connectWs() {
  try {
    ws = new WebSocket(`ws://${location.host}/ws`);
  } catch {
    return startPolling();
  }
  ws.onopen = () => $("liveDot").classList.add("on");
  ws.onclose = () => {
    $("liveDot").classList.remove("on");
    setTimeout(connectWs, 2000);
  };
  ws.onerror = () => ws && ws.close();
  ws.onmessage = (e) => {
    try { render(JSON.parse(e.data)); } catch {}
  };
}

let pollTimer = null;
function startPolling() {
  if (pollTimer) return;
  const tick = async () => { try { render(await apiGet("/info")); } catch {} };
  tick();
  pollTimer = setInterval(tick, 3000);
}

// ---- LED -------------------------------------------------------------------
function setLed(on) {
  const b = $("ledBtn");
  b.textContent = `LED: ${on ? "on" : "off"}`;
  b.classList.toggle("secondary", !on);
  b.dataset.on = on ? "1" : "0";
}
$("ledBtn").addEventListener("click", async () => {
  const next = $("ledBtn").dataset.on !== "1";
  try { const r = await apiSend("POST", "/api/led", { on: next }); setLed(r.on); }
  catch { msg("wifiMsg", "LED request failed", true); }
});

// ---- Wi-Fi -----------------------------------------------------------------
async function loadSaved() {
  try {
    const r = await apiGet("/api/wifi");
    $("savedList").innerHTML = (r.saved || []).length
      ? r.saved.map((s) => {
          const cur = s === r.current ? ` <span class="tag">current</span>` : "";
          return `<li><span>${s}${cur}</span><button class="link" data-forget="${encodeURIComponent(s)}">forget</button></li>`;
        }).join("")
      : `<li class="muted">none saved</li>`;
    document.querySelectorAll("[data-forget]").forEach((b) =>
      b.addEventListener("click", async () => {
        const ssid = decodeURIComponent(b.dataset.forget);
        await apiSend("DELETE", `/api/wifi?ssid=${encodeURIComponent(ssid)}`);
        loadSaved();
      })
    );
  } catch { /* device may be rebooting */ }
}

$("scanBtn").addEventListener("click", async () => {
  $("scanList").innerHTML = `<li class="muted">scanning…</li>`;
  try {
    const r = await apiGet("/api/scan");
    const nets = (r.networks || []).sort((a, b) => b.rssi - a.rssi);
    $("scanList").innerHTML = nets.length
      ? nets.map((n) => {
          const k = n.known ? ` <span class="tag">known</span>` : "";
          const lock = n.open ? "" : " 🔒";
          return `<li><button class="link" data-pick="${encodeURIComponent(n.ssid)}">${n.ssid || "(hidden)"}</button>${lock}${k}<span class="muted"> ${n.rssi} dBm</span></li>`;
        }).join("")
      : `<li class="muted">no networks found</li>`;
    document.querySelectorAll("[data-pick]").forEach((b) =>
      b.addEventListener("click", () => { $("ssid").value = decodeURIComponent(b.dataset.pick); $("pass").focus(); })
    );
  } catch { $("scanList").innerHTML = `<li class="err">scan failed</li>`; }
});

$("addBtn").addEventListener("click", async () => {
  const ssid = $("ssid").value.trim();
  if (!ssid) return msg("wifiMsg", "enter an SSID", true);
  msg("wifiMsg", "saving…");
  try {
    const r = await apiSend("POST", "/api/wifi", { ssid, pass: $("pass").value });
    if (r.ok) {
      msg("wifiMsg", `saved "${r.saved}". Rebooting to connect — reconnect to your network, then reopen this page.`);
      $("pass").value = "";
    } else {
      msg("wifiMsg", r.error || "save failed", true);
    }
  } catch { msg("wifiMsg", "request failed", true); }
});

// ---- OTA -------------------------------------------------------------------
$("otaBtn").addEventListener("click", () => {
  const f = $("fwFile").files[0];
  if (!f) return msg("otaMsg", "choose a .bin file first", true);
  const form = new FormData();
  form.append("update", f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/update");
  if (token) xhr.setRequestHeader("Authorization", "Bearer " + token);
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) $("otaBar").style.width = ((e.loaded / e.total) * 100).toFixed(0) + "%";
  };
  xhr.onload = () => {
    let ok = false;
    try { ok = JSON.parse(xhr.responseText).ok; } catch {}
    msg("otaMsg", ok ? "upload complete — device rebooting" : "update failed", !ok);
  };
  xhr.onerror = () => msg("otaMsg", "upload error", true);
  msg("otaMsg", "uploading…");
  xhr.send(form);
});

// ---- misc ------------------------------------------------------------------
$("rawToggle").addEventListener("click", () => {
  const el = $("rawOut");
  el.classList.toggle("hidden");
  $("rawToggle").textContent = el.classList.contains("hidden") ? "show" : "hide";
  if (!el.classList.contains("hidden") && lastInfo) el.textContent = JSON.stringify(lastInfo, null, 2);
});

$("hostNote").textContent = `host: ${location.host} — also try http://neonrift.local/`;

// ---- boot ------------------------------------------------------------------
connectWs();
loadSaved();
