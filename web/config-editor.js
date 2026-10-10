// Éditeur YAML de la configuration du configurateur
// (https://transit-tracker.eastsideurbanism.org/configurator), injecté dans le tableau de bord web
// via `web_server: js_include`. Lit/écrit les entités internes du configurateur à travers
// l'endpoint `transit_tracker: config_editor:` ; chaque sauvegarde recharge le tracker.

const ENDPOINT = "/transit-tracker/config";
const JS_YAML_URL = "https://cdnjs.cloudflare.com/ajax/libs/js-yaml/4.1.0/js-yaml.min.js";

const TIME_DISPLAYS = ["departure", "arrival"];
const UNIT_DISPLAYS = ["long", "short", "none"];
const LIST_MODES = ["sequential", "nextPerRoute"];

// ---------------------------------------------------------------------------
// Entités -> YAML (même structure que « Generate YAML » du configurateur)
// ---------------------------------------------------------------------------

function splitLines(text) {
  return (text || "").split("\n").filter((line) => line.trim() !== "");
}

function formatOffset(seconds) {
  if (!seconds) return undefined;
  return seconds % 60 === 0 ? `${seconds / 60}min` : `${seconds}s`;
}

function entitiesToConfig(raw) {
  const stops = new Map();
  for (const entry of (raw.schedule || "").split(";")) {
    if (!entry.trim()) continue;
    const [routeId, stopId, offset] = entry.split(",");
    if (!stops.has(stopId)) {
      stops.set(stopId, { stop_id: stopId, time_offset: formatOffset(Number(offset) || 0), routes: [] });
    }
    stops.get(stopId).routes.push(routeId);
  }

  const styles = splitLines(raw.route_styles).map((line) => {
    const [routeId, name, hex] = line.split(";");
    return { route_id: routeId, name, color: `c_${hex || ""}` };
  });
  const colors = [...new Set(styles.map((s) => s.color))].map((id) => ({ id, hex: id.slice(2) }));

  const abbreviations = splitLines(raw.abbreviations).map((line) => {
    const [from, to = ""] = line.split(";");
    return { from, to };
  });

  const tracker = { base_url: raw.base_url };
  if (raw.feed_code) tracker.feed_code = raw.feed_code;
  Object.assign(tracker, {
    time_display: raw.time_display,
    show_units: raw.time_units,
    list_mode: raw.list_mode,
    scroll_headsigns: raw.scroll_headsigns,
    header_text: raw.header_text,
    stops: [...stops.values()],
    styles,
    abbreviations,
  });

  const config = {};
  if (colors.length > 0) config.color = colors;
  config.transit_tracker = tracker;
  config.localization = {
    now: raw.now_str,
    min_long: raw.min_long_str,
    min_short: raw.min_short_str,
    hours_short: raw.hours_short_str,
  };
  return config;
}

// ---------------------------------------------------------------------------
// YAML -> entités (avec validation)
// ---------------------------------------------------------------------------

class ConfigError extends Error {}

function str(value, where, { allowEmpty = true, forbidden = "" } = {}) {
  if (value === undefined || value === null) value = "";
  if (typeof value === "object") throw new ConfigError(`${where} : une valeur simple est attendue`);
  value = String(value);
  if (!allowEmpty && value === "") throw new ConfigError(`${where} est obligatoire`);
  for (const c of forbidden) {
    if (value.includes(c)) {
      throw new ConfigError(`${where} ne peut pas contenir ${c === "\n" ? "de retour à la ligne" : `« ${c} »`}`);
    }
  }
  return value;
}

function list(value, where) {
  if (value === undefined || value === null) return [];
  if (!Array.isArray(value)) throw new ConfigError(`${where} doit être une liste`);
  return value;
}

function oneOf(value, allowed, where) {
  value = str(value, where, { allowEmpty: false });
  if (!allowed.includes(value)) throw new ConfigError(`${where} doit valoir ${allowed.join(", ")}`);
  return value;
}

function parseOffset(value, where) {
  if (value === undefined || value === null || value === "") return 0;
  if (typeof value === "number") return Math.round(value);
  const m = String(value).trim().match(/^(-?\d+(?:\.\d+)?)\s*(ms|s|sec|min|h)?$/);
  if (!m) throw new ConfigError(`${where} : durée invalide « ${value} » (ex. -5min, 30s)`);
  const factor = { ms: 0.001, s: 1, sec: 1, min: 60, h: 3600 }[m[2] || "s"];
  return Math.round(Number(m[1]) * factor);
}

function configToEntities(config) {
  if (!config || typeof config !== "object") throw new ConfigError("Le YAML doit être un dictionnaire");
  const t = config.transit_tracker;
  if (!t || typeof t !== "object") throw new ConfigError("La section transit_tracker: est manquante");

  const colorIds = new Map();
  list(config.color, "color").forEach((c, i) => {
    const hex = str(c && c.hex, `color[${i}].hex`, { allowEmpty: false }).replace(/^#/, "");
    colorIds.set(str(c.id, `color[${i}].id`, { allowEmpty: false }), hex);
  });

  const schedule = list(t.stops, "stops").flatMap((stop, i) => {
    const where = `stops[${i}]`;
    const stopId = str(stop && stop.stop_id, `${where}.stop_id`, { allowEmpty: false, forbidden: ",;" });
    const offset = parseOffset(stop.time_offset, `${where}.time_offset`);
    const routes = list(stop.routes, `${where}.routes`);
    if (routes.length === 0) throw new ConfigError(`${where}.routes ne doit pas être vide`);
    return routes.map((route, j) => {
      const routeId = str(route, `${where}.routes[${j}]`, { allowEmpty: false, forbidden: ",;" });
      return offset !== 0 ? `${routeId},${stopId},${offset}` : `${routeId},${stopId}`;
    });
  });

  const routeStyles = list(t.styles, "styles").map((style, i) => {
    const where = `styles[${i}]`;
    const routeId = str(style && style.route_id, `${where}.route_id`, { allowEmpty: false, forbidden: ";\n" });
    const name = str(style.name, `${where}.name`, { forbidden: ";\n" });
    const color = str(style.color, `${where}.color`, { allowEmpty: false });
    const hex = colorIds.get(color) ?? color.replace(/^#/, "");
    if (!/^[0-9a-fA-F]{6}$/.test(hex)) {
      throw new ConfigError(`${where}.color : « ${color} » n'est ni un id de color: ni une couleur hexadécimale`);
    }
    return `${routeId};${name};${hex}`;
  });

  const abbreviations = list(t.abbreviations, "abbreviations").map((a, i) => {
    const where = `abbreviations[${i}]`;
    const from = str(a && a.from, `${where}.from`, { allowEmpty: false, forbidden: ";\n" });
    const to = str(a.to, `${where}.to`, { forbidden: ";\n" });
    return to === "" ? from : `${from};${to}`;
  });

  const scroll = t.scroll_headsigns ?? false;
  if (typeof scroll !== "boolean") throw new ConfigError("scroll_headsigns doit valoir true ou false");

  const loc = config.localization || {};
  return {
    base_url: str(t.base_url, "base_url"),
    feed_code: str(t.feed_code, "feed_code"),
    schedule: schedule.join(";"),
    header_text: str(t.header_text, "header_text"),
    abbreviations: abbreviations.join("\n"),
    route_styles: routeStyles.join("\n"),
    time_display: oneOf(t.time_display ?? "departure", TIME_DISPLAYS, "time_display"),
    time_units: oneOf(t.show_units ?? "long", UNIT_DISPLAYS, "show_units"),
    list_mode: oneOf(t.list_mode ?? "sequential", LIST_MODES, "list_mode"),
    scroll_headsigns: scroll,
    now_str: str(loc.now ?? "Now", "localization.now"),
    min_long_str: str(loc.min_long ?? "min", "localization.min_long"),
    min_short_str: str(loc.min_short ?? "m", "localization.min_short"),
    hours_short_str: str(loc.hours_short ?? "h", "localization.hours_short"),
  };
}

// ---------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------

let jsYamlPromise;
function loadJsYaml() {
  jsYamlPromise ??= new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = JS_YAML_URL;
    script.onload = () => resolve(window.jsyaml);
    script.onerror = () => {
      jsYamlPromise = undefined;
      reject(new Error("Impossible de charger js-yaml (accès Internet requis)"));
    };
    document.head.appendChild(script);
  });
  return jsYamlPromise;
}

const STYLE = `
#tt-yaml-open {
  position: fixed; right: 16px; bottom: 16px; z-index: 1000;
  padding: 10px 16px; border: 0; border-radius: 999px; cursor: pointer;
  font: 500 14px system-ui, sans-serif; color: #fff; background: #03a9f4;
  box-shadow: 0 2px 8px rgba(0,0,0,.25);
}
#tt-yaml-dialog {
  --bg: #fff; --fg: #1c1c1c; --muted: #6b6b6b; --border: #d0d0d0; --field: #fafafa;
  --ok: #1b7f3b; --err: #c62828;
  width: min(900px, calc(100vw - 32px)); height: min(85vh, 900px); padding: 0;
  border: 1px solid var(--border); border-radius: 12px; color: var(--fg); background: var(--bg);
  font: 14px system-ui, sans-serif;
}
@media (prefers-color-scheme: dark) {
  #tt-yaml-dialog {
    --bg: #1e1e1e; --fg: #e6e6e6; --muted: #9a9a9a; --border: #3a3a3a; --field: #161616;
    --ok: #5fd38a; --err: #ff6b6b;
  }
}
#tt-yaml-dialog[open] { display: flex; flex-direction: column; }
#tt-yaml-dialog::backdrop { background: rgba(0,0,0,.5); }
#tt-yaml-dialog header, #tt-yaml-dialog footer {
  display: flex; align-items: center; gap: 8px; padding: 12px 16px;
}
#tt-yaml-dialog header { border-bottom: 1px solid var(--border); }
#tt-yaml-dialog footer { border-top: 1px solid var(--border); flex-wrap: wrap; }
#tt-yaml-dialog h2 { margin: 0; font-size: 16px; flex: 1; }
#tt-yaml-dialog textarea {
  flex: 1; margin: 0; padding: 12px 16px; border: 0; resize: none; outline: none;
  color: var(--fg); background: var(--field); tab-size: 2;
  font: 13px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace; white-space: pre;
}
#tt-yaml-dialog .status { flex: 1; min-width: 200px; color: var(--muted); white-space: pre-wrap; }
#tt-yaml-dialog .status.ok { color: var(--ok); }
#tt-yaml-dialog .status.err { color: var(--err); }
#tt-yaml-dialog button {
  padding: 8px 14px; border: 1px solid var(--border); border-radius: 8px; cursor: pointer;
  font: inherit; color: var(--fg); background: transparent;
}
#tt-yaml-dialog button.primary { border-color: #03a9f4; color: #fff; background: #03a9f4; }
#tt-yaml-dialog button:disabled { opacity: .5; cursor: default; }
`;

function createUi() {
  const style = document.createElement("style");
  style.textContent = STYLE;
  document.head.appendChild(style);

  const open = document.createElement("button");
  open.id = "tt-yaml-open";
  open.textContent = "Configuration YAML";
  document.body.appendChild(open);

  const dialog = document.createElement("dialog");
  dialog.id = "tt-yaml-dialog";
  dialog.innerHTML = `
    <header>
      <h2>Configuration YAML</h2>
      <button type="button" data-action="close" aria-label="Fermer">✕</button>
    </header>
    <textarea spellcheck="false" autocapitalize="off" autocomplete="off"></textarea>
    <footer>
      <span class="status"></span>
      <button type="button" data-action="reload">Relire l'appareil</button>
      <button type="button" data-action="save" class="primary">Enregistrer et recharger</button>
    </footer>`;
  document.body.appendChild(dialog);

  const textarea = dialog.querySelector("textarea");
  const status = dialog.querySelector(".status");
  const saveButton = dialog.querySelector('[data-action="save"]');
  let savedText = "";
  let deviceKeys = [];
  let busy = false;

  const setStatus = (message, kind = "") => {
    status.textContent = message;
    status.className = `status ${kind}`;
  };
  const isDirty = () => textarea.value !== savedText;
  const refreshDirty = () => {
    saveButton.disabled = busy;
    if (!busy && !status.classList.contains("err")) {
      setStatus(isDirty() ? "Modifications non enregistrées" : "");
    }
  };

  async function load() {
    busy = true;
    setStatus("Lecture de la configuration…");
    try {
      const [jsyaml, response] = await Promise.all([loadJsYaml(), fetch(ENDPOINT, { cache: "no-store" })]);
      if (!response.ok) throw new Error(`L'appareil a répondu ${response.status}`);
      const raw = await response.json();
      deviceKeys = Object.keys(raw);
      savedText = jsyaml.dump(entitiesToConfig(raw), { lineWidth: -1, noRefs: true, quotingType: '"' });
      textarea.value = savedText;
      setStatus("");
    } catch (e) {
      setStatus(e.message, "err");
    } finally {
      busy = false;
      refreshDirty();
    }
  }

  async function save() {
    if (busy) return;
    busy = true;
    saveButton.disabled = true;
    try {
      const jsyaml = await loadJsYaml();
      let parsed;
      try {
        parsed = jsyaml.load(textarea.value);
      } catch (e) {
        throw new ConfigError(`YAML invalide : ${e.reason || e.message}${e.mark ? ` (ligne ${e.mark.line + 1})` : ""}`);
      }
      const entities = configToEntities(parsed);
      // N'envoie que les clés exposées par l'appareil
      const body = Object.fromEntries(Object.entries(entities).filter(([key]) => deviceKeys.includes(key)));
      setStatus("Enregistrement…");
      const response = await fetch(ENDPOINT, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!response.ok) throw new Error((await response.text()) || `L'appareil a répondu ${response.status}`);
      savedText = textarea.value;
      setStatus(`Enregistré à ${new Date().toLocaleTimeString()} — tracker rechargé`, "ok");
    } catch (e) {
      setStatus(e.message, "err");
    } finally {
      busy = false;
      refreshDirty();
    }
  }

  const close = () => {
    if (isDirty() && !confirm("Fermer sans enregistrer les modifications ?")) return;
    dialog.close();
  };

  open.addEventListener("click", () => {
    dialog.showModal();
    if (!isDirty()) load();
  });
  dialog.addEventListener("cancel", (e) => {
    e.preventDefault();
    close();
  });
  dialog.querySelector('[data-action="close"]').addEventListener("click", close);
  dialog.querySelector('[data-action="reload"]').addEventListener("click", () => {
    if (isDirty() && !confirm("Abandonner les modifications et relire la configuration de l'appareil ?")) return;
    load();
  });
  saveButton.addEventListener("click", save);

  textarea.addEventListener("input", () => {
    if (status.classList.contains("err") || status.classList.contains("ok")) setStatus("");
    refreshDirty();
  });
  textarea.addEventListener("keydown", (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "s") {
      e.preventDefault();
      save();
    } else if (e.key === "Tab" && !e.shiftKey) {
      e.preventDefault();
      textarea.setRangeText("  ", textarea.selectionStart, textarea.selectionEnd, "end");
      refreshDirty();
    }
  });
}

// ---------------------------------------------------------------------------
// Panneau Wi-Fi : réseaux visibles et connexion (endpoint `transit_tracker: wifi_manager:`)
// ---------------------------------------------------------------------------

const WIFI_ENDPOINT = "/transit-tracker/wifi";

const WIFI_STYLE = `
#tt-wifi-open {
  position: fixed; right: 16px; bottom: 64px; z-index: 1000;
  padding: 10px 16px; border: 0; border-radius: 999px; cursor: pointer;
  font: 500 14px system-ui, sans-serif; color: #fff; background: #03a9f4;
  box-shadow: 0 2px 8px rgba(0,0,0,.25);
}
#tt-wifi-dialog {
  --bg: #fff; --fg: #1c1c1c; --muted: #6b6b6b; --border: #d0d0d0; --field: #f3f3f3; --hover: #eaf6fd;
  --ok: #1b7f3b; --err: #c62828;
  width: min(520px, calc(100vw - 32px)); max-height: min(85vh, 760px); padding: 0;
  border: 1px solid var(--border); border-radius: 12px; color: var(--fg); background: var(--bg);
  font: 14px system-ui, sans-serif;
}
@media (prefers-color-scheme: dark) {
  #tt-wifi-dialog {
    --bg: #1e1e1e; --fg: #e6e6e6; --muted: #9a9a9a; --border: #3a3a3a; --field: #2a2a2a; --hover: #10364a;
    --ok: #5fd38a; --err: #ff6b6b;
  }
}
#tt-wifi-dialog[open] { display: flex; flex-direction: column; }
#tt-wifi-dialog::backdrop { background: rgba(0,0,0,.5); }
#tt-wifi-dialog header, #tt-wifi-dialog footer { display: flex; align-items: center; gap: 8px; padding: 12px 16px; }
#tt-wifi-dialog header { border-bottom: 1px solid var(--border); }
#tt-wifi-dialog footer { border-top: 1px solid var(--border); flex-wrap: wrap; }
#tt-wifi-dialog h2 { margin: 0; font-size: 16px; flex: 1; }
#tt-wifi-dialog .current { padding: 12px 16px; border-bottom: 1px solid var(--border); color: var(--muted); }
#tt-wifi-dialog .current b { color: var(--fg); }
#tt-wifi-dialog ul { list-style: none; margin: 0; padding: 4px 0; overflow-y: auto; flex: 1; min-height: 120px; }
#tt-wifi-dialog li button {
  width: 100%; display: flex; align-items: center; gap: 10px; padding: 10px 16px; border: 0;
  background: transparent; color: var(--fg); font: inherit; text-align: left; cursor: pointer;
}
#tt-wifi-dialog li button:hover, #tt-wifi-dialog li button:focus-visible { background: var(--hover); outline: none; }
#tt-wifi-dialog li .name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#tt-wifi-dialog li .meta { color: var(--muted); font-variant-numeric: tabular-nums; white-space: nowrap; }
#tt-wifi-dialog li.connected .name::after { content: " · connecté"; color: var(--ok); }
#tt-wifi-dialog .bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 14px; }
#tt-wifi-dialog .bars i { width: 3px; background: var(--border); border-radius: 1px; }
#tt-wifi-dialog .bars i.on { background: var(--fg); }
#tt-wifi-dialog .empty { padding: 16px; color: var(--muted); }
#tt-wifi-dialog form { display: grid; gap: 10px; padding: 12px 16px; border-top: 1px solid var(--border); }
#tt-wifi-dialog form[hidden] { display: none; }
#tt-wifi-dialog label { display: grid; gap: 4px; color: var(--muted); font-size: 13px; }
#tt-wifi-dialog input {
  padding: 8px 10px; border: 1px solid var(--border); border-radius: 8px; font: inherit;
  color: var(--fg); background: var(--field);
}
#tt-wifi-dialog .row { display: flex; gap: 8px; justify-content: flex-end; flex-wrap: wrap; }
#tt-wifi-dialog .status { flex: 1; min-width: 200px; color: var(--muted); }
#tt-wifi-dialog .status.ok { color: var(--ok); }
#tt-wifi-dialog .status.err { color: var(--err); }
#tt-wifi-dialog button.btn {
  padding: 8px 14px; border: 1px solid var(--border); border-radius: 8px; cursor: pointer;
  font: inherit; color: var(--fg); background: transparent;
}
#tt-wifi-dialog button.primary { border-color: #03a9f4; color: #fff; background: #03a9f4; }
#tt-wifi-dialog button:disabled { opacity: .5; cursor: default; }
`;

function signalBars(rssi) {
  const level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return `<span class="bars" aria-hidden="true">${[5, 8, 11, 14]
    .map((h, i) => `<i style="height:${h}px" class="${i < level ? "on" : ""}"></i>`).join("")}</span>`;
}

function escapeHtml(text) {
  return text.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

function createWifiUi() {
  const style = document.createElement("style");
  style.textContent = WIFI_STYLE;
  document.head.appendChild(style);

  const open = document.createElement("button");
  open.id = "tt-wifi-open";
  open.textContent = "Wi-Fi";
  document.body.appendChild(open);

  const dialog = document.createElement("dialog");
  dialog.id = "tt-wifi-dialog";
  dialog.innerHTML = `
    <header>
      <h2>Wi-Fi</h2>
      <button type="button" class="btn" data-action="close" aria-label="Fermer">✕</button>
    </header>
    <div class="current"></div>
    <ul aria-label="Réseaux visibles"></ul>
    <form hidden>
      <label>Réseau <input id="tt-wifi-ssid" name="ssid" maxlength="32" autocomplete="off" required></label>
      <label>Mot de passe <input id="tt-wifi-password" name="password" type="password" maxlength="64"
        autocomplete="off" placeholder="Laisser vide pour un réseau ouvert"></label>
      <div class="row">
        <button type="button" class="btn" data-action="cancel">Annuler</button>
        <button type="submit" class="btn primary">Se connecter</button>
      </div>
    </form>
    <footer>
      <span class="status"></span>
      <button type="button" class="btn" data-action="other">Autre réseau…</button>
      <button type="button" class="btn" data-action="scan">Rechercher</button>
    </footer>`;
  document.body.appendChild(dialog);

  const current = dialog.querySelector(".current");
  const list = dialog.querySelector("ul");
  const form = dialog.querySelector("form");
  const ssidInput = form.querySelector("#tt-wifi-ssid");
  const passwordInput = form.querySelector("#tt-wifi-password");
  const status = dialog.querySelector(".status");
  const scanButton = dialog.querySelector('[data-action="scan"]');
  let pollTimer = null;
  let lastState = null;
  let connectingTo = null;

  const setStatus = (message, kind = "") => {
    status.textContent = message;
    status.className = `status ${kind}`;
  };

  function render(state) {
    lastState = state;
    current.innerHTML = state.connected
      ? `Connecté à <b>${escapeHtml(state.ssid)}</b> · ${escapeHtml(state.ip)} · ${state.rssi} dBm`
      : "Non connecté";

    if (state.networks.length === 0) {
      list.innerHTML = `<li class="empty">${state.scanning ? "Recherche des réseaux…" : "Aucun réseau trouvé. Lance une recherche."}</li>`;
    } else {
      list.innerHTML = state.networks.map((n, i) => `
        <li class="${state.connected && n.ssid === state.ssid ? "connected" : ""}">
          <button type="button" data-index="${i}">
            ${signalBars(n.rssi)}
            <span class="name">${escapeHtml(n.ssid)}</span>
            <span class="meta">${n.lock ? "🔒 " : ""}${n.rssi} dBm</span>
          </button>
        </li>`).join("");
    }
    scanButton.disabled = state.scanning;
    scanButton.textContent = state.scanning ? "Recherche…" : "Rechercher";

    // The status keeps the previous attempt's message: only follow the one about this network
    if (connectingTo !== null && state.status && state.status.includes(connectingTo)) {
      const failed = state.status.startsWith("Échec");
      const done = failed || state.status.startsWith("Connecté");
      setStatus(state.status, failed ? "err" : done ? "ok" : "");
      if (done) connectingTo = null;
    }
  }

  async function refresh() {
    try {
      const response = await fetch(WIFI_ENDPOINT, { cache: "no-store" });
      if (!response.ok) throw new Error(`L'appareil a répondu ${response.status}`);
      render(await response.json());
    } catch (e) {
      // Après un changement de réseau, l'appareil n'est plus joignable à cette adresse
      if (connectingTo !== null) {
        setStatus(`Connexion à ${connectingTo} en cours. Si elle réussit, l'afficheur aura une nouvelle adresse IP ` +
          "(appuie sur ses deux boutons pour l'afficher). En cas d'échec, il revient ici dans 30 s.");
      } else {
        setStatus(e.message, "err");
      }
    }
  }

  async function scan() {
    scanButton.disabled = true;
    try {
      await fetch(`${WIFI_ENDPOINT}/scan`, { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
    } catch (e) {
      setStatus(e.message, "err");
    }
    refresh();
  }

  function showForm(ssid) {
    form.hidden = false;
    ssidInput.value = ssid;
    passwordInput.value = "";
    (ssid ? passwordInput : ssidInput).focus();
  }

  function startPolling() {
    stopPolling();
    pollTimer = setInterval(refresh, 1500);
  }
  function stopPolling() {
    if (pollTimer !== null) clearInterval(pollTimer);
    pollTimer = null;
  }

  open.addEventListener("click", () => {
    dialog.showModal();
    form.hidden = true;
    setStatus("");
    refresh();
    scan();
    startPolling();
  });
  dialog.addEventListener("close", stopPolling);
  dialog.querySelector('[data-action="close"]').addEventListener("click", () => dialog.close());
  dialog.querySelector('[data-action="cancel"]').addEventListener("click", () => { form.hidden = true; });
  dialog.querySelector('[data-action="other"]').addEventListener("click", () => showForm(""));
  scanButton.addEventListener("click", scan);

  list.addEventListener("click", (e) => {
    const item = e.target.closest("button[data-index]");
    if (!item || !lastState) return;
    showForm(lastState.networks[Number(item.dataset.index)].ssid);
  });

  form.addEventListener("submit", async (e) => {
    e.preventDefault();
    const ssid = ssidInput.value.trim();
    const password = passwordInput.value;
    if (password && password.length < 8) {
      setStatus("Le mot de passe doit faire au moins 8 caractères (ou rester vide pour un réseau ouvert).", "err");
      return;
    }
    try {
      const response = await fetch(`${WIFI_ENDPOINT}/connect`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid, password }),
      });
      if (!response.ok) throw new Error((await response.text()) || `L'appareil a répondu ${response.status}`);
      connectingTo = ssid;
      form.hidden = true;
      passwordInput.value = "";
      setStatus(`Connexion à ${ssid}…`);
    } catch (err) {
      setStatus(err.message, "err");
    }
  });
}

function init() {
  createUi();
  createWifiUi();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", init);
} else {
  init();
}
