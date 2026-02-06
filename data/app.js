const $ = (id) => document.getElementById(id);

const healthOut = $("healthOut");
const infoOut = $("infoOut");
const healthBadge = $("healthBadge");
const autoBtn = $("btnAuto");
const autoNote = $("autoNote");

let auto = false;
let timer = null;

function pretty(obj) {
  return JSON.stringify(obj, null, 2);
}

async function apiGet(path) {
  const res = await fetch(path, { cache: "no-store" });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch {}
  return { ok: res.ok, status: res.status, json, text };
}

async function loadHealth() {
  healthBadge.textContent = "loading...";
  const r = await apiGet("/health");
  if (r.ok && r.json) {
    healthBadge.textContent = "ok";
    healthOut.textContent = pretty(r.json);
  } else {
    healthBadge.textContent = `err ${r.status}`;
    healthOut.textContent = r.json ? pretty(r.json) : r.text;
  }
}

async function loadInfo() {
  const r = await apiGet("/info");
  infoOut.textContent = r.json ? pretty(r.json) : r.text;
}

function setAuto(on) {
  auto = on;
  autoBtn.textContent = `Auto refresh: ${auto ? "on" : "off"}`;
  autoBtn.classList.toggle("secondary", !auto);
  autoNote.textContent = auto ? "(every 2s)" : "";

  if (timer) {
    clearInterval(timer);
    timer = null;
  }
  if (auto) {
    timer = setInterval(loadInfo, 2000);
  }
}

$("btnHealth").addEventListener("click", loadHealth);
$("btnInfo").addEventListener("click", loadInfo);
autoBtn.addEventListener("click", () => setAuto(!auto));

// initial
loadHealth();
loadInfo();
