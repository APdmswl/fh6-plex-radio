// FH6 Universal Radio dashboard. Vanilla JS, no build step. `state` holds
// the latest /api/state; `cfg` holds the latest /api/config. Render functions
// are idempotent and only touch nodes whose displayed value changed.

const $  = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

const api = {
  async get(path) {
    const r = await fetch(path);
    if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
    return r.json();
  },
  async send(path, body, method = "POST") {
    const r = await fetch(path, {
      method,
      headers: body ? { "content-type": "application/json" } : {},
      body:    body ? JSON.stringify(body) : undefined,
    });
    if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
    return r.json().catch(() => ({}));
  },
};

let state = null;
let cfg   = null;
let plexLists = { libraries: [], playlists: [], artists: [], albums: [] };

const plexSource = () => state?.sources?.available?.find(s => s.name === "plex");

const fmt = ms => {
  if (!ms || ms < 0) return "0:00";
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
};
const esc = v => String(v ?? "").replace(/[&<>"']/g, c => ({
  "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
}[c]));

const toast = (msg, isErr = false) => {
  const el = document.createElement("div");
  el.className = "toast" + (isErr ? " err" : "");
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 2400);
};

// Only write when the displayed value changes, to avoid cursor jumps in inputs.
const setText = (el, v) => { if (el && el.textContent !== String(v)) el.textContent = v; };
const setValue = (el, v) => {
  if (!el || document.activeElement === el) return;
  if (el.type === "checkbox") el.checked = !!v;
  else if (el.value !== String(v ?? "")) el.value = v ?? "";
};

function renderStatus() {
  const ok = state?.game?.attached;
  const sub = $("#status");
  sub.className = "subtitle " + (ok ? "ok" : "err");
  sub.textContent = ok ? "connected" : "bridge offline";
}

function renderNowPlaying() {
  const t = state?.track || {};
  const art = $("#np-art");
  setText($("#np-title"),  t.title  || "Nothing playing");
  setText($("#np-artist"), t.artist ? `${t.artist}${t.album ? " · " + t.album : ""}` : "");
  setText($("#np-pos"), fmt(t.position_ms));
  setText($("#np-dur"), fmt(t.duration_ms));
  if (t.artwork_url) {
    if (art.dataset.url !== t.artwork_url) {
      art.style.backgroundImage = `url(${JSON.stringify(t.artwork_url)})`;
      art.dataset.url = t.artwork_url;
    }
    art.classList.add("has-art");
  } else {
    art.style.backgroundImage = "";
    art.dataset.url = "";
    art.classList.remove("has-art");
  }
  const pct = (t.duration_ms && t.position_ms)
    ? Math.min(100, (t.position_ms / t.duration_ms) * 100)
    : 0;
  $("#np-fill").style.width = pct + "%";

  const src = plexSource();
  const playing = src?.playback_state === "playing";
  $("#t-play").textContent = playing ? "⏸" : "▶";
}

let volDirty = false;
function renderOutput() {
  const gain = state?.audio?.output_gain ?? 0;
  if (!volDirty) {
    const slider = $("#vol");
    if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
    $("#vol-out").value = Math.round(gain * 100) + "%";
  }
}

function fillSelect(el, rows, current, emptyLabel, labelFn) {
  const sig = JSON.stringify([rows.map(r => [r.key, r.title, r.artist, r.leaf_count]), current, emptyLabel]);
  if (el.dataset.sig === sig) return;
  el.dataset.sig = sig;

  let html = `<option value="">${esc(emptyLabel)}</option>`;
  let hasCurrent = !current;
  for (const row of rows) {
    if (row.key === current) hasCurrent = true;
    html += `<option value="${esc(row.key)}">${esc(labelFn(row))}</option>`;
  }
  if (!hasCurrent) html += `<option value="${esc(current)}">${esc(current)}</option>`;
  el.innerHTML = html;
  el.value = current || "";
}

function renderPlexPanel() {
  const card = $("#plex-card");
  if (!cfg?.plex) {
    card.hidden = true;
    return;
  }
  card.hidden = false;

  const formSig = JSON.stringify([cfg.plex.server_url, cfg.plex.token, cfg.plex.shuffle]);
  if (card.dataset.formSig !== formSig) {
    card.dataset.formSig = formSig;
    setValue($("#plex-server"), cfg.plex.server_url || "https://your-plex.example.com");
    setValue($("#plex-token"), cfg.plex.token || "");
    setValue($("#plex-shuffle"), cfg.plex.shuffle ?? true);
  }

  fillSelect($("#plex-library"), plexLists.libraries, cfg.plex.library_key,
    "Select library", row => `${row.title}${row.leaf_count ? ` (${row.leaf_count})` : ""}`);
  fillSelect($("#plex-artist"), plexLists.artists, cfg.plex.artist_key,
    "All artists", row => `${row.title}${row.leaf_count ? ` (${row.leaf_count})` : ""}`);
  fillSelect($("#plex-album"), plexLists.albums, cfg.plex.album_key,
    "All albums", row => `${row.title}${row.artist ? ` - ${row.artist}` : ""}${row.leaf_count ? ` (${row.leaf_count})` : ""}`);
  fillSelect($("#plex-playlist"), plexLists.playlists, cfg.plex.playlist_key,
    "No playlist", row => `${row.title}${row.leaf_count ? ` (${row.leaf_count})` : ""}`);

  const plex = plexSource();
  const status = plex
    ? `${plex.playback_state} - ${plex.auth_state.replace("_", " ")} - ${plex.details?.track_count ?? 0} tracks`
    : "disabled";
  setText($("#plex-status"), status);
}

function collectPlexConfig() {
  return {
    enabled: true,
    server_url: $("#plex-server").value.trim() || "https://your-plex.example.com",
    token: $("#plex-token").value.trim(),
    library_key: $("#plex-library").value,
    playlist_key: $("#plex-playlist").value,
    artist_key: $("#plex-artist").value,
    album_key: $("#plex-album").value,
    ffmpeg_path: cfg?.plex?.ffmpeg_path || "",
    shuffle: $("#plex-shuffle").checked,
  };
}

async function loadPlexLists() {
  const [libs, playlists] = await Promise.all([
    api.get("/api/source/plex/libraries"),
    api.get("/api/source/plex/playlists"),
  ]);
  plexLists = {
    libraries: libs.libraries || [],
    playlists: playlists.playlists || [],
    artists: plexLists.artists || [],
    albums: plexLists.albums || [],
  };
  if (cfg?.plex?.library_key) await loadPlexLibraryLists();
  else renderPlexPanel();
}

async function loadPlexLibraryLists() {
  if (!cfg?.plex?.library_key) {
    plexLists.artists = [];
    plexLists.albums = [];
    renderPlexPanel();
    return;
  }
  const [artists, albums] = await Promise.all([
    api.get("/api/source/plex/artists"),
    api.get("/api/source/plex/albums"),
  ]);
  plexLists.artists = artists.artists || [];
  plexLists.albums = albums.albums || [];
  renderPlexPanel();
}

async function loadPlexAlbums() {
  if (!cfg?.plex?.library_key && !cfg?.plex?.artist_key) {
    plexLists.albums = [];
    renderPlexPanel();
    return;
  }
  const albums = await api.get("/api/source/plex/albums");
  plexLists.albums = albums.albums || [];
  renderPlexPanel();
}

async function savePlexConfig({ play = false, load = false, loadLibrary = false, loadAlbums = false } = {}) {
  const plex = collectPlexConfig();
  const patch = { plex };
  if (play) patch.general = { default_source: "plex", fallback_source: "plex" };
  cfg = await api.send("/api/config", patch, "PUT");
  renderPlexPanel();

  if (load || play) await loadPlexLists();
  else if (loadLibrary) await loadPlexLibraryLists();
  else if (loadAlbums) await loadPlexAlbums();
  if (play) {
    await api.send("/api/source/plex/refresh");
    await api.send("/api/source/switch", { source: "plex" });
    await api.send("/api/source/plex/play");
  }
}

const SCHEMA = [
  ["general", "General", [
    ["port",                    "Port",                   "number", 1, 65535],
    ["ring_buffer_mb",          "Ring buffer (MB)",       "number", 1, 64],
    ["open_dashboard_on_start", "Open dashboard on start","checkbox"],
  ]],
  ["plex", "Plex", [
    ["enabled",      "Enabled",                "checkbox"],
    ["server_url",   "Server URL",             "text"],
    ["token",        "Token",                  "password"],
    ["library_key",  "Library key",            "text"],
    ["playlist_key", "Playlist key",           "text"],
    ["artist_key",   "Artist key",             "text"],
    ["album_key",    "Album key",              "text"],
    ["ffmpeg_path",  "ffmpeg path (optional)", "text"],
    ["shuffle",      "Shuffle",                "checkbox"],
  ]],
  ["audio", "Audio", [
    ["output_gain", "Output gain", "number", 0, 1, 0.01],
  ]],
  ["playback", "Playback", [
    ["race_start_playback", "Race start", "select", ["ignore", "next", "restart"]],
    ["quick_station_skip",  "Quick station skip", "checkbox"],
    ["force_stereo_audio",  "Force stereo audio", "checkbox"],
  ]],
];

function field(section, [key, label, type, min, max, step]) {
  const id = `f-${section}-${key}`;
  const cur = cfg?.[section]?.[key];
  if (type === "checkbox") {
    return `<div class="field checkbox">
      <input type="checkbox" id="${id}" data-section="${section}" data-key="${key}" ${cur ? "checked" : ""}>
      <label for="${id}">${label}</label>
    </div>`;
  }
  if (type === "select") {
    const options = (min || []).map(v =>
      `<option value="${esc(v)}" ${cur === v ? "selected" : ""}>${esc(v)}</option>`
    ).join("");
    return `<div class="field">
      <label for="${id}">${label}</label>
      <select id="${id}" data-section="${section}" data-key="${key}">${options}</select>
    </div>`;
  }
  const attrs = type === "number"
    ? ` min="${min ?? ''}" max="${max ?? ''}" step="${step ?? 1}"`
    : "";
  return `<div class="field">
    <label for="${id}">${label}</label>
    <input id="${id}" type="${type}" data-section="${section}" data-key="${key}"${attrs} value="${esc(cur ?? '')}">
  </div>`;
}

function renderSettings() {
  $("#settings-form").innerHTML = SCHEMA.map(([sec, title, fields]) =>
    `<fieldset><legend>${title}</legend>${fields.map(f => field(sec, f)).join("")}</fieldset>`
  ).join("");
}

function collectSettings() {
  const patch = {};
  $$("#settings-form [data-section]").forEach(el => {
    const sec = el.dataset.section;
    const key = el.dataset.key;
    (patch[sec] ??= {});
    if (el.type === "checkbox")    patch[sec][key] = el.checked;
    else if (el.type === "number") patch[sec][key] = parseFloat(el.value);
    else                           patch[sec][key] = el.value;
  });
  return patch;
}

function openDrawer() {
  $("#drawer").classList.add("open");
  $("#scrim").hidden = false;
  $("#drawer").setAttribute("aria-hidden", "false");
}
function closeDrawer() {
  $("#drawer").classList.remove("open");
  $("#scrim").hidden = true;
  $("#drawer").setAttribute("aria-hidden", "true");
}

async function transport(action) {
  const src = "plex";
  const s = plexSource();
  if (!s) {
    toast("Plex source is not registered", true);
    return;
  }
  // Centre button is a smart play/pause toggle.
  if (action === "play") {
    if (s?.playback_state === "playing") action = "pause";
  }
  try {
    if (action !== "pause") await api.send("/api/source/switch", { source: src });
    await api.send(`/api/source/${src}/${action}`);
  } catch (e) { toast(e.message, true); }
}

function wire() {
  $("#t-play").onclick = () => transport("play");
  $("#t-next").onclick = () => transport("next");
  $("#t-prev").onclick = () => transport("previous");

  const vol = $("#vol");
  vol.addEventListener("input", () => {
    volDirty = true;
    $("#vol-out").value = Math.round(parseFloat(vol.value) * 100) + "%";
  });
  vol.addEventListener("change", async () => {
    try { await api.send("/api/options", { output_gain: parseFloat(vol.value) }); }
    catch (e) { toast(e.message, true); }
    setTimeout(() => { volDirty = false; }, 400);
  });

  $("#plex-load").onclick = async () => {
    try {
      await savePlexConfig({ load: true });
      toast("Plex loaded");
    } catch (e) {
      setText($("#plex-status"), e.message);
      toast(e.message, true);
    }
  };
  $("#plex-library").onchange = async () => {
    $("#plex-playlist").value = "";
    $("#plex-artist").value = "";
    $("#plex-album").value = "";
    plexLists.artists = [];
    plexLists.albums = [];
    try { await savePlexConfig({ loadLibrary: true }); }
    catch (e) { setText($("#plex-status"), e.message); toast(e.message, true); }
  };
  $("#plex-artist").onchange = async () => {
    $("#plex-playlist").value = "";
    $("#plex-album").value = "";
    plexLists.albums = [];
    try { await savePlexConfig({ loadAlbums: true }); }
    catch (e) { setText($("#plex-status"), e.message); toast(e.message, true); }
  };
  $("#plex-album").onchange = async () => {
    $("#plex-playlist").value = "";
    try { await savePlexConfig(); }
    catch (e) { setText($("#plex-status"), e.message); toast(e.message, true); }
  };
  $("#plex-playlist").onchange = async () => {
    if ($("#plex-playlist").value) {
      $("#plex-artist").value = "";
      $("#plex-album").value = "";
    }
    try { await savePlexConfig(); }
    catch (e) { setText($("#plex-status"), e.message); toast(e.message, true); }
  };
  $("#plex-config").addEventListener("submit", async e => {
    e.preventDefault();
    try {
      await savePlexConfig();
      toast("Saved");
    } catch (err) {
      setText($("#plex-status"), err.message);
      toast(err.message, true);
    }
  });
  $("#plex-play").onclick = async () => {
    try {
      await savePlexConfig({ play: true });
      toast("Plex playing");
    } catch (e) {
      setText($("#plex-status"), e.message);
      toast(e.message, true);
    }
  };

  $("#open-settings").onclick  = async () => { cfg = await api.get("/api/config"); renderSettings(); openDrawer(); };
  $("#close-settings").onclick = closeDrawer;
  $("#scrim").onclick          = closeDrawer;
  $("#save-config").onclick    = async () => {
    try {
      cfg = await api.send("/api/config", collectSettings(), "PUT");
      renderPlexPanel();
      toast("Saved");
      closeDrawer();
    } catch (e) { toast(e.message, true); }
  };
  $("#reload-config").onclick  = async () => {
    cfg = await api.send("/api/config/reload");
    renderSettings();
    renderPlexPanel();
    toast("Reloaded from disk");
  };

  api.get("/api/config").then(c => {
    cfg = c;
    renderPlexPanel();
    if (cfg.plex?.enabled && cfg.plex?.token) loadPlexLists().catch(() => {});
  }).catch(() => {});
}

// SSE if available, polling fallback otherwise.
function connect() {
  let es;
  try {
    es = new EventSource("/api/events");
    es.onmessage = e => { state = JSON.parse(e.data); render(); };
    es.onerror   = () => { es.close(); setTimeout(poll, 1000); };
  } catch { poll(); }
}
async function poll() {
  try { state = await api.get("/api/state"); render(); }
  catch { /* keep last state */ }
  setTimeout(poll, 1000);
}

function render() {
  renderStatus();
  renderNowPlaying();
  renderOutput();
  renderPlexPanel();
}

wire();
connect();
