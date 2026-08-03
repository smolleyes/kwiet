/* Kwiet panel logic.
 *
 * Two readings of the same microphone, everywhere. The scope draws the last few
 * seconds as stacked envelopes; the meter shows the same instant as one bar.
 * In both, celadon is what reaches your apps and amber is what Kwiet took out,
 * so the gap between them is the product.
 */

import { applyStaticStrings, detectLanguage, language, setLanguage, t } from "./i18n.js";

const invoke = window.__TAURI__.core.invoke;

const FLOOR_DB = -60;
const HISTORY = 220; // samples kept on screen, ~9 s at 25 Hz
const POLL_MS = 40;
/** The pack only changes when somebody visits Windows settings. */
const PACK_POLL_MS = 1000;

/** Peak hold, so a transient that is over before the eye catches it still shows. */
const PEAK_HOLD_MS = 900;
const PEAK_FALL_DB_PER_S = 26;

const el = {
  scope: document.getElementById("scope"),
  scopeIdle: document.getElementById("scopeIdle"),
  idleTitle: document.getElementById("idleTitle"),
  idleBody: document.getElementById("idleBody"),
  idleAction: document.getElementById("idleAction"),
  dot: document.getElementById("dot"),
  stateText: document.getElementById("stateText"),
  meterValue: document.getElementById("meterValue"),
  meterNoise: document.getElementById("meterNoise"),
  meterFill: document.getElementById("meterFill"),
  meterPeak: document.getElementById("meterPeak"),
  meterRemoved: document.getElementById("meterRemoved"),
  slider: document.getElementById("aggressiveness"),
  sliderValue: document.getElementById("aggressivenessValue"),
  toggle: document.getElementById("enabled"),
  toggleSub: document.getElementById("toggleSub"),
  status: document.getElementById("status"),
  lang: document.getElementById("lang"),
};

const ctx = el.scope.getContext("2d");
const history = [];
/** Last known effect-pack state; null until the first poll answers. */
let pack = null;
/** Smoothed, so the readout does not flicker on every frame. */
let removedSmoothed = 0;
let peakDb = FLOOR_DB;
let peakHeldUntil = 0;
/** Set while dragging, so polling does not fight the user's hand. */
let sliderHeld = false;

const clamp01 = (v) => Math.max(0, Math.min(1, v));
/** dB to 0..1 across the meter's range. */
const norm = (db) => clamp01((db - FLOOR_DB) / -FLOOR_DB);
const pct = (db) => `${(norm(db) * 100).toFixed(1)}%`;

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
const micName = (name) => (name ? `« ${name} »` : t("idle.thisMic"));

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
  if (!pack || !pack.known) return { key: "noStream" };
  if (!pack.installed) return { key: "notInstalled" };
  if (!pack.selectedOnDefault && pack.selectedElsewhere.length > 0) {
    return {
      key: "wrongMic",
      vars: { other: pack.selectedElsewhere[0], mic: micName(pack.defaultDevice) },
      action: true,
    };
  }
  if (!pack.selectedOnDefault) {
    return { key: "notSelected", vars: { mic: micName(pack.defaultDevice) }, action: true };
  }
  return { key: "waiting", vars: { mic: micName(pack.defaultDevice) } };
}

function renderMeter(s, live) {
  const filtering = live && s.enabled && s.dspActive;
  const outDb = live ? s.levelOutDb : FLOOR_DB;
  const inDb = live ? s.levelInDb : FLOOR_DB;

  const now = performance.now();
  if (outDb >= peakDb) {
    peakDb = outDb;
    peakHeldUntil = now + PEAK_HOLD_MS;
  } else if (now > peakHeldUntil) {
    peakDb = Math.max(FLOOR_DB, peakDb - (PEAK_FALL_DB_PER_S * POLL_MS) / 1000);
  }

  el.meterFill.style.setProperty("--level", pct(outDb));
  el.meterNoise.style.setProperty("--noise", pct(inDb));
  el.meterPeak.style.setProperty("--peak", pct(peakDb));
  el.meterPeak.hidden = peakDb <= FLOOR_DB;

  const silent = outDb <= FLOOR_DB;
  el.meterValue.textContent = silent ? "—" : `${outDb.toFixed(1)} dB`;
  el.meterValue.classList.toggle("quiet", silent);

  // Only meaningful while the DSP is really filtering.
  if (filtering) {
    const instant = Math.max(0, s.levelInDb - s.levelOutDb);
    removedSmoothed += (instant - removedSmoothed) * 0.2;
    el.meterRemoved.textContent =
      removedSmoothed < 0.5 ? "" : t("meter.removed", { db: removedSmoothed.toFixed(1) });
  } else {
    removedSmoothed = 0;
    el.meterRemoved.textContent = "";
  }
}

function renderState(s) {
  const live = s.present && s.streaming;
  el.scopeIdle.hidden = live;

  const idle = live ? null : idleState();
  if (idle) {
    el.idleTitle.textContent = t(`idle.${idle.key}.title`);
    el.idleBody.textContent = t(`idle.${idle.key}.body`, idle.vars);
    el.idleAction.hidden = !idle.action;
    el.scopeIdle.classList.toggle("needs-action", Boolean(idle.action));
  }

  el.dot.className = "dot";
  if (!live) {
    el.dot.classList.toggle("bypass", Boolean(idle.action));
    el.stateText.textContent = t(idle.action ? "state.unconfigured" : "state.idle");
  } else if (!s.enabled) {
    el.dot.classList.add("bypass");
    el.stateText.textContent = t("state.bypassed");
  } else if (!s.dspActive) {
    el.dot.classList.add("bypass");
    el.stateText.textContent = t("state.unfiltered");
  } else {
    el.dot.classList.add("live");
    el.stateText.textContent = t("state.live");
  }

  el.toggle.setAttribute("aria-checked", String(s.enabled));
  el.toggleSub.textContent = t(s.enabled ? "toggle.on" : "toggle.off");

  if (!sliderHeld) {
    el.slider.value = String(Math.round(s.aggressivenessDb));
    updateSliderChrome();
  }

  renderMeter(s, live);

  const fault = s.underruns > 0 || s.dspErrors > 0;
  el.status.classList.toggle("fault", fault);
  if (!s.present) {
    el.status.textContent = t("status.noStream");
  } else {
    const bits = [
      `${(s.sampleRate / 1000).toFixed(1)} kHz`,
      `${s.channels} ch`,
      t("status.latency", { ms: s.latencyMs.toFixed(0) }),
    ];
    if (fault) {
      bits.push(
        t("status.underruns", { n: s.underruns }),
        t("status.errors", { n: s.dspErrors }),
      );
    }
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

function applyLanguage(lang) {
  const chosen = setLanguage(lang);
  for (const button of el.lang.querySelectorAll("button")) {
    button.setAttribute("aria-pressed", String(button.dataset.lang === chosen));
  }
}

el.lang.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-lang]");
  if (!button) return;
  applyLanguage(button.dataset.lang);
  invoke("set_language", { language: button.dataset.lang });
});

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

async function start() {
  let saved = null;
  try {
    saved = await invoke("language");
  } catch {
    // First run, or an older build: fall back to the system language.
  }
  applyLanguage(saved ?? detectLanguage());
  applyStaticStrings();

  resizeScope();
  updateSliderChrome();
  await pollPack();
  await tick();
  setInterval(tick, POLL_MS);
  setInterval(pollPack, PACK_POLL_MS);
}

start();
