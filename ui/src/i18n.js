/* Panel strings, French and English.
 *
 * Kept as one flat dictionary rather than a framework: the panel has a few
 * dozen strings and no routing, so anything heavier would be scaffolding around
 * an object literal.
 *
 * House rules for the copy: name things the way the user sees them, not the way
 * the code is built ("microphone", never "endpoint"); one job per string; and
 * an error says what went wrong and what to do about it, without apologising.
 */

const STRINGS = {
  fr: {
    "state.idle": "en veille",
    "state.unconfigured": "non configuré",
    "state.bypassed": "contourné",
    "state.unfiltered": "sans filtrage",
    "state.live": "nettoyage actif",

    "legend.removed": "bruit retiré",
    "legend.kept": "voix conservée",

    "idle.noStream.title": "Aucun micro actif",
    "idle.noStream.body":
      "Ouvre une application qui écoute ton micro pour voir le signal.",
    "idle.notInstalled.title": "Pack d'effets absent",
    "idle.notInstalled.body":
      "Windows n'a pas le pack Kwiet. Réinstalle Kwiet, puis choisis-le dans les paramètres du son.",
    "idle.wrongMic.title": "Choisi sur le mauvais micro",
    "idle.wrongMic.body":
      "Kwiet est en place sur « {other} », mais les applications reçoivent {mic}.",
    "idle.notSelected.title": "Kwiet n'est pas encore choisi",
    "idle.notSelected.body":
      "Le pack est installé, mais Windows ne s'en sert pas. Ouvre {mic} dans les paramètres du son, puis choisis Kwiet sous « Améliorations audio ».",
    "idle.waiting.title": "Aucune application n'utilise le micro",
    "idle.waiting.body":
      "Kwiet est en place sur {mic} et attend. Ouvre une application qui écoute le micro.",
    "idle.action": "Ouvrir les paramètres du son",
    "idle.thisMic": "ton micro",

    "meter.label": "Niveau transmis",
    "meter.removed": "{db} dB retirés",

    "control.label": "Intensité",
    "control.low": "discrète",
    "control.high": "maximale",
    "control.hint":
      "Plus haut, le silence devient total mais les débuts de mots peuvent être rabotés.",

    "toggle.label": "Nettoyage du micro",
    "toggle.on": "Actif sur toutes les applications",
    "toggle.off": "Le micro passe sans traitement",

    "status.noStream": "aucun flux de capture",
    "status.latency": "{ms} ms de retard",
    "status.underruns": "{n} décrochages",
    "status.errors": "{n} erreurs",
  },

  en: {
    "state.idle": "idle",
    "state.unconfigured": "not set up",
    "state.bypassed": "bypassed",
    "state.unfiltered": "not filtering",
    "state.live": "cleaning",

    "legend.removed": "noise removed",
    "legend.kept": "voice kept",

    "idle.noStream.title": "No microphone in use",
    "idle.noStream.body": "Open an app that listens to your microphone to see the signal.",
    "idle.notInstalled.title": "Effect pack missing",
    "idle.notInstalled.body":
      "Windows does not have the Kwiet pack. Reinstall Kwiet, then choose it in sound settings.",
    "idle.wrongMic.title": "Chosen on the wrong microphone",
    "idle.wrongMic.body":
      "Kwiet is set up on “{other}”, but apps are getting {mic}.",
    "idle.notSelected.title": "Kwiet is not chosen yet",
    "idle.notSelected.body":
      "The pack is installed, but Windows is not using it. Open {mic} in sound settings, then choose Kwiet under “Audio enhancements”.",
    "idle.waiting.title": "No app is using the microphone",
    "idle.waiting.body":
      "Kwiet is set up on {mic} and waiting. Open an app that listens to the microphone.",
    "idle.action": "Open sound settings",
    "idle.thisMic": "your microphone",

    "meter.label": "Level sent to apps",
    "meter.removed": "{db} dB removed",

    "control.label": "Strength",
    "control.low": "gentle",
    "control.high": "maximum",
    "control.hint":
      "Higher means total silence between words, at the risk of clipping the start of them.",

    "toggle.label": "Microphone cleaning",
    "toggle.on": "Active in every app",
    "toggle.off": "Microphone passes through untouched",

    "status.noStream": "no capture stream",
    "status.latency": "{ms} ms delay",
    "status.underruns": "{n} dropouts",
    "status.errors": "{n} errors",
  },
};

export const LANGUAGES = ["fr", "en"];

/** Anything not French gets English: those are the two we actually wrote. */
export function detectLanguage() {
  const tag = (navigator.language || "en").toLowerCase();
  return tag.startsWith("fr") ? "fr" : "en";
}

let current = detectLanguage();

export function setLanguage(lang) {
  current = LANGUAGES.includes(lang) ? lang : detectLanguage();
  document.documentElement.lang = current;
  applyStaticStrings();
  return current;
}

export const language = () => current;

/** Missing keys surface as the key itself rather than an empty label. */
export function t(key, vars) {
  let text = STRINGS[current][key] ?? STRINGS.en[key] ?? key;
  if (vars) {
    for (const [name, value] of Object.entries(vars)) {
      text = text.replaceAll(`{${name}}`, value);
    }
  }
  return text;
}

/** Fills every element carrying a `data-i18n` key. */
export function applyStaticStrings(root = document) {
  for (const node of root.querySelectorAll("[data-i18n]")) {
    node.textContent = t(node.dataset.i18n);
  }
}
