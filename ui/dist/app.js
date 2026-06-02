const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

const api = {
  async get(path) {
    const response = await fetch(path);
    if (!response.ok) throw new Error((await response.json().catch(() => ({}))).error || response.statusText);
    return response.json();
  },
  async send(path, body, method = "POST") {
    const response = await fetch(path, {
      method,
      headers: body ? { "content-type": "application/json" } : {},
      body: body ? JSON.stringify(body) : undefined,
    });
    if (!response.ok) throw new Error((await response.json().catch(() => ({}))).error || response.statusText);
    return response.json().catch(() => ({}));
  },
};

const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

const SCHEMA = [
  ["general", "General", [
    ["port", "Port", "number", 1, 65535],
    ["ring_buffer_mb", "Ring buffer (MB)", "number", 1, 64],
    ["open_dashboard_on_start", "Open dashboard on start", "checkbox"],
    ["default_source", "Default source", "source-select"],
    ["fallback_source", "Fallback source", "source-select"],
    ["ffmpeg_path", "ffmpeg path", "text"],
  ]],
  ["plex", "Plex", [
    ["enabled", "Enabled", "checkbox"],
    ["server_url", "Server URL", "text"],
    ["token", "Token", "password"],
    ["library_key", "Library key", "text"],
    ["playlist_key", "Playlist key", "text"],
    ["artist_key", "Artist key", "text"],
    ["album_key", "Album key", "text"],
    ["ffmpeg_path", "ffmpeg path override", "text"],
    ["shuffle", "Shuffle", "checkbox"],
  ]],
  ["audio", "Audio", [
    ["output_gain", "Output gain", "number", 0, 1, 0.01],
  ]],
  ["playback", "Playback", [
    ["race_start_playback", "Race start", "select", ["ignore", "next", "restart"]],
    ["quick_station_skip", "Quick station skip", "checkbox"],
    ["volume_normalization", "Normalize loudness", "checkbox"],
    ["equalizer_enabled", "Equalizer", "checkbox"],
    ["equalizer_bands", "Equalizer bands", "bands"],
    ["force_stereo_audio", "Force stereo audio", "checkbox"],
  ]],
];

let state = null;
let cfg = null;
let plexLists = { libraries: [], playlists: [], artists: [], albums: [] };
let volDirty = false;

const esc = value => String(value ?? "").replace(/[&<>"']/g, char => ({
  "&": "&amp;",
  "<": "&lt;",
  ">": "&gt;",
  "\"": "&quot;",
  "'": "&#39;",
}[char]));

const fmt = ms => {
  if (!ms || ms < 0) return "0:00";
  const seconds = Math.floor(ms / 1000);
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
};

function toast(message, isError = false) {
  const el = document.createElement("div");
  el.className = `toast${isError ? " err" : ""}`;
  el.textContent = message;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 2600);
}

function setText(el, value) {
  if (el && el.textContent !== String(value)) el.textContent = value;
}

function setValue(el, value) {
  if (!el || document.activeElement === el) return;
  if (el.type === "checkbox") el.checked = Boolean(value);
  else if (el.value !== String(value ?? "")) el.value = value ?? "";
}

const sources = () => state?.sources?.available || [];
const activeSourceName = () => state?.sources?.active || "";
const plexSource = () => sources().find(source => source.name === "plex");

function sourceOptions(current = "") {
  const rows = sources().map(source => [source.name, source.display_name || source.name]);
  if (cfg?.plex && !rows.some(([name]) => name === "plex")) rows.push(["plex", "Plex"]);
  if (current && !rows.some(([name]) => name === current)) rows.push([current, current]);
  return rows.length ? rows : [["plex", "Plex"]];
}

function renderStatus() {
  const ok = Boolean(state?.game?.attached);
  const status = $("#status");
  status.className = `status ${ok ? "ok" : "err"}`;
  setText(status, ok ? "connected" : "bridge offline");
}

function renderNowPlaying() {
  const track = state?.track || {};
  const title = track.title || "Nothing playing";
  const artist = track.artist ? `${track.artist}${track.album ? " - " + track.album : ""}` : "";
  const artUrl = track.artwork_url || "";
  const art = $("#np-art");
  const backdrop = $("#np-backdrop");

  setText($("#np-title"), title);
  setText($("#np-artist"), artist);
  setText($("#np-pos"), fmt(track.position_ms));
  setText($("#np-dur"), fmt(track.duration_ms));
  setText($("#np-source"), activeSourceName() === "plex" ? "Plex Radio" : (activeSourceName() || "Radio"));

  if (artUrl) {
    const image = `url(${JSON.stringify(artUrl)})`;
    if (art.dataset.url !== artUrl) {
      art.style.backgroundImage = image;
      backdrop.style.backgroundImage = image;
      art.dataset.url = artUrl;
    }
    art.classList.add("has-art");
    $("#hero").classList.add("has-art");
  } else {
    art.style.backgroundImage = "";
    backdrop.style.backgroundImage = "";
    art.dataset.url = "";
    art.classList.remove("has-art");
    $("#hero").classList.remove("has-art");
  }

  const pct = track.duration_ms && track.position_ms
    ? Math.min(100, Math.max(0, (track.position_ms / track.duration_ms) * 100))
    : 0;
  $("#np-fill").style.width = `${pct}%`;

  const playing = plexSource()?.playback_state === "playing";
  $("#t-play").setAttribute("aria-label", playing ? "Pause" : "Play");
  $("#t-play-icon").textContent = playing ? "II" : "\u25b6";
}

function renderRadioSummary() {
  const active = sources().find(source => source.name === activeSourceName());
  const plex = plexSource();
  const note = plex?.auth_instructions || "";
  const stateText = plex
    ? `${plex.playback_state} - ${plex.auth_state.replace("_", " ")} - ${plex.details?.track_count ?? 0} tracks`
    : "Plex source is not registered";

  setText($("#radio-summary"), note || stateText);
  setText($("#metric-source"), active?.display_name || activeSourceName() || "-");
  setText($("#metric-buffer"), state?.audio?.ring_capacity
    ? `${Math.round((state.audio.ring_avail || 0) / 1024)} KB`
    : "-");
  setText($("#metric-mode"), state?.audio?.native_dsp_mode || "-");
}

function renderOutput() {
  const gain = state?.audio?.output_gain ?? cfg?.audio?.output_gain ?? 1;
  if (volDirty) return;
  const slider = $("#vol");
  if (Math.abs(parseFloat(slider.value || "0") - gain) > 0.005) slider.value = gain;
  $("#vol-out").value = `${Math.round(gain * 100)}%`;
}

function fillSelect(el, rows, current, emptyLabel, labelFn) {
  const sig = JSON.stringify([rows.map(row => [row.key, row.title, row.artist, row.leaf_count]), current, emptyLabel]);
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
  const note = plex?.auth_instructions || "";
  const base = plex
    ? `${plex.playback_state} - ${plex.auth_state.replace("_", " ")} - ${plex.details?.track_count ?? 0} tracks`
    : "disabled";
  const status = note ? `${base} - ${note}` : base;
  const statusEl = $("#plex-status");
  statusEl.className = plex?.auth_state === "error" ? "err" : plex?.auth_state === "needs_auth" ? "warn" : "";
  setText(statusEl, status);
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
  const [libraries, playlists] = await Promise.all([
    api.get("/api/source/plex/libraries"),
    api.get("/api/source/plex/playlists"),
  ]);
  plexLists = {
    libraries: libraries.libraries || [],
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
  const patch = { plex: collectPlexConfig() };
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

function field(section, [key, label, type, min, max, step]) {
  const id = `f-${section}-${key}`;
  const current = cfg?.[section]?.[key];
  if (type === "checkbox") {
    return `<div class="field checkbox">
      <input type="checkbox" id="${id}" data-section="${section}" data-key="${key}" ${current ? "checked" : ""}>
      <label for="${id}">${esc(label)}</label>
    </div>`;
  }
  if (type === "select") {
    const options = (min || []).map(value =>
      `<option value="${esc(value)}" ${current === value ? "selected" : ""}>${esc(value)}</option>`
    ).join("");
    return `<div class="field">
      <label for="${id}">${esc(label)}</label>
      <select id="${id}" data-section="${section}" data-key="${key}">${options}</select>
    </div>`;
  }
  if (type === "source-select") {
    const options = sourceOptions(current).map(([value, text]) =>
      `<option value="${esc(value)}" ${current === value ? "selected" : ""}>${esc(text)}</option>`
    ).join("");
    return `<div class="field">
      <label for="${id}">${esc(label)}</label>
      <select id="${id}" data-section="${section}" data-key="${key}">${options}</select>
    </div>`;
  }
  if (type === "bands") {
    const values = Array.isArray(current) ? current : [0, 0, 0, 0, 0];
    const inputs = EQ_BAND_LABELS.map((band, index) => `
      <label>
        <span>${esc(band)}</span>
        <input type="number" min="-6" max="6" step="0.5" value="${esc(values[index] ?? 0)}" data-band-index="${index}">
      </label>`).join("");
    return `<div class="field bands" data-section="${section}" data-key="${key}">${inputs}</div>`;
  }
  const attrs = type === "number" ? ` min="${min ?? ""}" max="${max ?? ""}" step="${step ?? 1}"` : "";
  return `<div class="field">
    <label for="${id}">${esc(label)}</label>
    <input id="${id}" type="${type}" data-section="${section}" data-key="${key}"${attrs} value="${esc(current ?? "")}">
  </div>`;
}

function renderSettings() {
  $("#settings-form").innerHTML = SCHEMA.map(([section, title, fields]) =>
    `<fieldset><legend>${esc(title)}</legend>${fields.map(item => field(section, item)).join("")}</fieldset>`
  ).join("");
}

function collectSettings() {
  const patch = {};
  $$("#settings-form [data-section]").forEach(el => {
    const section = el.dataset.section;
    const key = el.dataset.key;
    patch[section] ??= {};
    if (el.classList.contains("bands")) {
      patch[section][key] = $$("input", el).map(input => {
        const value = parseFloat(input.value);
        return Number.isFinite(value) ? Math.max(-6, Math.min(6, value)) : 0;
      });
    } else if (el.type === "checkbox") {
      patch[section][key] = el.checked;
    } else if (el.type === "number") {
      const value = parseFloat(el.value);
      patch[section][key] = Number.isFinite(value) ? value : 0;
    } else {
      patch[section][key] = el.value;
    }
  });
  return patch;
}

function openDrawer() {
  $("#drawer").classList.add("open");
  $("#drawer").setAttribute("aria-hidden", "false");
  $("#scrim").hidden = false;
}

function closeDrawer() {
  $("#drawer").classList.remove("open");
  $("#drawer").setAttribute("aria-hidden", "true");
  $("#scrim").hidden = true;
}

async function transport(action) {
  const plex = plexSource();
  if (!plex) {
    toast("Plex source is not registered", true);
    return;
  }
  let nextAction = action;
  if (nextAction === "play" && plex.playback_state === "playing") nextAction = "pause";
  try {
    if (nextAction !== "pause") await api.send("/api/source/switch", { source: "plex" });
    await api.send(`/api/source/plex/${nextAction}`);
  } catch (error) {
    toast(error.message, true);
  }
}

function wire() {
  $("#t-play").onclick = () => transport("play");
  $("#t-next").onclick = () => transport("next");
  $("#t-prev").onclick = () => transport("previous");

  $("#vol").addEventListener("input", event => {
    volDirty = true;
    $("#vol-out").value = `${Math.round(parseFloat(event.target.value) * 100)}%`;
  });
  $("#vol").addEventListener("change", async event => {
    try {
      await api.send("/api/options", { output_gain: parseFloat(event.target.value) });
    } catch (error) {
      toast(error.message, true);
    }
    setTimeout(() => { volDirty = false; }, 400);
  });

  $("#plex-load").onclick = async () => {
    try {
      await savePlexConfig({ load: true });
      toast("Plex loaded");
    } catch (error) {
      setText($("#plex-status"), error.message);
      toast(error.message, true);
    }
  };
  $("#plex-library").onchange = async () => {
    $("#plex-playlist").value = "";
    $("#plex-artist").value = "";
    $("#plex-album").value = "";
    plexLists.artists = [];
    plexLists.albums = [];
    try { await savePlexConfig({ loadLibrary: true }); }
    catch (error) { setText($("#plex-status"), error.message); toast(error.message, true); }
  };
  $("#plex-artist").onchange = async () => {
    $("#plex-playlist").value = "";
    $("#plex-album").value = "";
    plexLists.albums = [];
    try { await savePlexConfig({ loadAlbums: true }); }
    catch (error) { setText($("#plex-status"), error.message); toast(error.message, true); }
  };
  $("#plex-album").onchange = async () => {
    $("#plex-playlist").value = "";
    try { await savePlexConfig(); }
    catch (error) { setText($("#plex-status"), error.message); toast(error.message, true); }
  };
  $("#plex-playlist").onchange = async () => {
    if ($("#plex-playlist").value) {
      $("#plex-artist").value = "";
      $("#plex-album").value = "";
    }
    try { await savePlexConfig(); }
    catch (error) { setText($("#plex-status"), error.message); toast(error.message, true); }
  };
  $("#plex-config").addEventListener("submit", async event => {
    event.preventDefault();
    try {
      await savePlexConfig();
      toast("Saved");
    } catch (error) {
      setText($("#plex-status"), error.message);
      toast(error.message, true);
    }
  });
  $("#plex-play").onclick = async () => {
    try {
      await savePlexConfig({ play: true });
      toast("Plex playing");
    } catch (error) {
      setText($("#plex-status"), error.message);
      toast(error.message, true);
    }
  };

  $("#open-settings").onclick = async () => {
    cfg = await api.get("/api/config");
    renderSettings();
    openDrawer();
  };
  $("#close-settings").onclick = closeDrawer;
  $("#scrim").onclick = closeDrawer;
  $("#save-config").onclick = async () => {
    try {
      cfg = await api.send("/api/config", collectSettings(), "PUT");
      renderSettings();
      renderPlexPanel();
      closeDrawer();
      toast("Saved");
    } catch (error) {
      toast(error.message, true);
    }
  };
  $("#reload-config").onclick = async () => {
    cfg = await api.send("/api/config/reload");
    renderSettings();
    renderPlexPanel();
    toast("Reloaded from disk");
  };
}

function render() {
  renderStatus();
  renderNowPlaying();
  renderRadioSummary();
  renderOutput();
  renderPlexPanel();
}

function connectEvents() {
  try {
    const events = new EventSource("/api/events");
    events.onmessage = event => {
      state = JSON.parse(event.data);
      render();
    };
    events.onerror = () => {
      events.close();
      setTimeout(poll, 1000);
    };
  } catch {
    poll();
  }
}

async function poll() {
  try {
    state = await api.get("/api/state");
    render();
  } catch {
    // Keep the last visible state while the bridge is offline.
  }
  setTimeout(poll, 1000);
}

wire();
api.get("/api/config").then(next => {
  cfg = next;
  renderPlexPanel();
  renderOutput();
  if (cfg.plex?.enabled && cfg.plex?.token) loadPlexLists().catch(() => {});
}).catch(() => {});
connectEvents();
