/* Kwiet panel logic.
 *
 * The scope is the point of this UI: it draws the last few seconds of the
 * microphone as two stacked envelopes. The celadon area is what reaches your
 * apps; the amber above it is what Kwiet took out. When you stop talking the
 * amber stays and the celadon collapses — that gap is the product.
 */

const invoke = window.__TAURI__.core.invoke;

const FLOOR_DB = -60;
const HISTORY = 220; // samples kept on screen, ~9 s at 25 Hz
const POLL_MS = 40;
/** The pack only changes when somebody visits Windows settings. */
const PACK_POLL_MS = 1000;

const el = {
  scope: document.getElementById("scope"),
  scopeIdle: document.getElementById("scopeIdle"),
  idleTitle: document.getElementById("idleTitle"),
  idleBody: document.getElementById("idleBody"),
  idleAction: document.getElementById("idleAction"),
  dot: document.getElementById("dot"),
  stateText: document.getElementById("stateText"),
  removed: document.getElementById("removed"),
  figure: document.querySelector(".figure"),
  slider: document.getElementById("aggressiveness"),
  sliderValue: document.getElementById("aggressivenessValue"),
  toggle: document.getElementById("enabled"),
  toggleSub: document.getElementById("toggleSub"),
  status: document.getElementById("status"),
};

const ctx = el.scope.getContext("2d");
const history = [];
/** Last known effect-pack state; null until the first poll answers. */
let pack = null;
/** Smoothed figure, so the readout does not flicker on every frame. */
let removedSmoothed = 0;
/** Set while dragging, so polling does not fight the user's hand. */
let sliderHeld = false;

const clamp01 = (v) => Math.max(0, Math.min(1, v));
/** dB to 0..1 across the meter's range. */
const norm = (db) => clamp01((db - FLOOR_DB) / -FLOOR_DB);

function resizeScope() {
  const ratio = window.devicePixelRatio || 1;
  const rect = el.scope.getBoundingClientRect();
  el.scope.width = Math.round(rect.width * ratio);
  el.scope.height = Math.round(rect.height * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

function drawScope() {
  const w = el.scope.clientWidth;
  const h = el.scope.clientHeight;
  ctx.clearRect(0, 0, w, h);

  // Reference grid every 20 dB: enough to read a level, quiet enough to ignore.
  ctx.strokeStyle = "rgba(255,255,255,0.045)";
  ctx.lineWidth = 1;
  for (let db = -20; db > FLOOR_DB; db -= 20) {
    const y = Math.round(h - norm(db) * h) + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  if (history.length < 2) return;

  const step = w / (HISTORY - 1);
  const xOf = (i) => w - (history.length - 1 - i) * step;

  // Amber first: the raw envelope, i.e. everything the microphone heard.
  ctx.beginPath();
  ctx.moveTo(xOf(0), h);
  for (let i = 0; i < history.length; i++) {
    ctx.lineTo(xOf(i), h - norm(history[i].inDb) * h);
  }
  ctx.lineTo(xOf(history.length - 1), h);
  ctx.closePath();
  ctx.fillStyle = "rgba(224,164,88,0.26)";
  ctx.fill();

  ctx.beginPath();
  for (let i = 0; i < history.length; i++) {
    const x = xOf(i);
    const y = h - norm(history[i].inDb) * h;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.strokeStyle = "rgba(224,164,88,0.85)";
  ctx.lineWidth = 1;
  ctx.stroke();

  // Celadon on top: what actually leaves for the apps.
  ctx.beginPath();
  ctx.moveTo(xOf(0), h);
  for (let i = 0; i < history.length; i++) {
    ctx.lineTo(xOf(i), h - norm(history[i].outDb) * h);
  }
  ctx.lineTo(xOf(history.length - 1), h);
  ctx.closePath();
  ctx.fillStyle = "rgba(159,211,192,0.9)";
  ctx.fill();
}

/** The microphone's name, or a neutral stand-in when Windows gives none. */
const micName = (name) => (name ? `« ${name} »` : "ton micro");

/**
 * What to say over the scope when no signal is coming through.
 *
 * Ordered from the thing that blocks everything to the thing that is merely
 * waiting, so the user is always told the *first* reason nothing is happening.
 * The middle two are the trap Windows sets: the pack installs without a word,
 * stays switched off until somebody picks it in Settings, and nothing in the
 * audio stack ever mentions it.
 */
function idleState() {
  if (!pack) {
    return {
      title: "Aucun micro actif",
      body: "Ouvre une application qui écoute ton micro pour voir le signal.",
    };
  }
  if (!pack.installed) {
    return {
      title: "Pack d'effets absent",
      body: "Windows n'a pas le pack Kwiet. Réinstalle Kwiet, puis choisis-le dans les paramètres du micro.",
    };
  }
  if (!pack.selectedOnDefault && pack.selectedElsewhere.length > 0) {
    return {
      title: "Choisi sur le mauvais micro",
      body: `Kwiet est en place sur « ${pack.selectedElsewhere[0]} », mais les applications reçoivent ${micName(pack.defaultDevice)}.`,
      action: true,
    };
  }
  if (!pack.selectedOnDefault) {
    return {
      title: "Kwiet n'est pas encore choisi",
      body: `Le pack est installé, mais Windows ne s'en sert pas. Ouvre ${micName(pack.defaultDevice)} dans les paramètres du son, puis choisis Kwiet sous « Améliorations audio ».`,
      action: true,
    };
  }
  return {
    title: "Aucune application n'utilise le micro",
    body: `Kwiet est en place sur ${micName(pack.defaultDevice)} et attend. Ouvre une application qui écoute le micro.`,
  };
}

function renderState(s) {
  const live = s.present && s.streaming;
  el.scopeIdle.hidden = live;

  const idle = live ? null : idleState();
  if (idle) {
    el.idleTitle.textContent = idle.title;
    el.idleBody.textContent = idle.body;
    el.idleAction.hidden = !idle.action;
    el.scopeIdle.classList.toggle("needs-action", Boolean(idle.action));
  }

  el.dot.className = "dot";
  if (!live) {
    if (idle.action) {
      el.dot.classList.add("bypass");
      el.stateText.textContent = "non configuré";
    } else {
      el.stateText.textContent = "en veille";
    }
  } else if (!s.enabled) {
    el.dot.classList.add("bypass");
    el.stateText.textContent = "contourné";
  } else if (!s.dspActive) {
    el.dot.classList.add("bypass");
    el.stateText.textContent = "sans filtrage";
  } else {
    el.dot.classList.add("live");
    el.stateText.textContent = "nettoyage actif";
  }

  el.toggle.setAttribute("aria-checked", String(s.enabled));
  el.toggleSub.textContent = s.enabled
    ? "Actif sur toutes les applications"
    : "Le micro passe sans traitement";

  if (!sliderHeld) {
    el.slider.value = String(Math.round(s.aggressivenessDb));
    updateSliderChrome();
  }

  // The readout only means something while the DSP is really filtering.
  if (live && s.enabled && s.dspActive) {
    const instant = Math.max(0, s.levelInDb - s.levelOutDb);
    removedSmoothed += (instant - removedSmoothed) * 0.2;
    el.removed.textContent = removedSmoothed.toFixed(1);
    el.figure.classList.toggle("quiet", removedSmoothed < 0.5);
  } else {
    removedSmoothed = 0;
    el.removed.textContent = "—";
    el.figure.classList.add("quiet");
  }

  const fault = s.underruns > 0 || s.dspErrors > 0;
  el.status.classList.toggle("fault", fault);
  if (!s.present) {
    el.status.textContent = "aucun flux de capture";
  } else {
    const bits = [
      `${(s.sampleRate / 1000).toFixed(1)} kHz`,
      `${s.channels} ch`,
      `${s.latencyMs.toFixed(0)} ms de retard`,
    ];
    if (fault) bits.push(`${s.underruns} décrochages`, `${s.dspErrors} erreurs`);
    el.status.textContent = bits.join("  ·  ");
  }
}

function updateSliderChrome() {
  const v = Number(el.slider.value);
  el.slider.style.setProperty("--fill", `${v}%`);
  el.sliderValue.textContent = `${v} dB`;
}

async function tick() {
  let s;
  try {
    s = await invoke("snapshot");
  } catch {
    return;
  }

  if (s.present && s.streaming) {
    history.push({ inDb: s.levelInDb, outDb: s.levelOutDb });
    while (history.length > HISTORY) history.shift();
  } else if (history.length > 0) {
    history.length = 0;
  }

  renderState(s);
  drawScope();
}

async function pollPack() {
  try {
    pack = await invoke("pack_status");
  } catch {
    // Keep the previous answer rather than flashing a wrong one.
  }
}

el.idleAction.addEventListener("click", () => {
  invoke("open_microphone_settings");
});

el.slider.addEventListener("pointerdown", () => {
  sliderHeld = true;
});
window.addEventListener("pointerup", () => {
  sliderHeld = false;
});
el.slider.addEventListener("input", () => {
  updateSliderChrome();
  invoke("set_aggressiveness", { db: Number(el.slider.value) });
});

el.toggle.addEventListener("click", () => {
  const next = el.toggle.getAttribute("aria-checked") !== "true";
  el.toggle.setAttribute("aria-checked", String(next));
  invoke("set_enabled", { enabled: next });
});

window.addEventListener("resize", () => {
  resizeScope();
  drawScope();
});

resizeScope();
updateSliderChrome();
pollPack();
tick();
setInterval(tick, POLL_MS);
setInterval(pollPack, PACK_POLL_MS);
