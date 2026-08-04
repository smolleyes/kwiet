// Generates the Kwiet identity from geometry, so every asset stays consistent
// and can be regenerated: node assets/build-assets.mjs
//
// The mark is a bubble of quiet. A solid celadon disc with the voice cut clean
// out of it in negative space, and the noise -- amber splinters -- held outside,
// never crossing in. A ring of untouched ground between the disc and the
// splinters is what sells it: the noise is not merely absent, it is kept at bay.
//
// Colours come from the panel: celadon is what Kwiet keeps, amber what it takes
// away. Logo and UI say the same thing.
//
// Designed at 16 px first. A filled disc with a cut-out squiggle keeps a strong
// silhouette when the splinters have blurred to nothing, which is why the voice
// is negative space rather than a stroke laid on top.

import { execFileSync } from "node:child_process";
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "svg");
const PNG = join(HERE, "png");
const ICONS = join(HERE, "..", "ui", "src-tauri", "icons");

const INK = {
  ground: "#0D1417",
  noise: "#E0A458",
  voice: "#9FD3C0",
  // The amber is tuned for the panel's near-black. On paper or a white README
  // it washes out, so the light lockup takes a deeper burnt tone at the same hue.
  noiseOnLight: "#B9762A",
};

/** Deterministic jitter, so regenerating never reshuffles the mark. */
function lcg(seed) {
  let s = seed >>> 0;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s / 0x1_0000_0000;
  };
}

const round = (n, digits = 2) => Number(n.toFixed(digits));
const lerp = (a, b, t) => a + (b - a) * t;

/**
 * The voice. Few lobes and a fat stroke, because this has to stay open at 16 px;
 * uneven harmonics so it reads as speech rather than a test tone.
 */
function voiceY(t, amp) {
  const taper = Math.sin(Math.PI * Math.min(1, Math.max(0, t))) ** 0.4;
  const shape =
    0.86 * Math.sin(2 * Math.PI * 1.5 * (t - 0.5)) +
    0.14 * Math.sin(2 * Math.PI * 3 * (t - 0.5));
  return -amp * taper * shape;
}

function voicePath(x0, span, cy, amp, steps = 160) {
  let d = "";
  for (let i = 0; i <= steps; i += 1) {
    const t = i / steps;
    d += `${i === 0 ? "M" : "L"}${round(x0 + span * t)} ${round(cy + voiceY(t, amp))}`;
  }
  return d;
}

/**
 * Noise, in the annulus outside the quiet halo. Splinters lie roughly tangent
 * to the disc: debris circling it, not arrows aimed at it.
 */
function splinters({ cx, cy, rInner, rOuter, scale, seed, count, minLen, maxLen, width }) {
  const rand = lcg(seed);
  const out = [];
  for (let i = 0; i < count; i += 1) {
    // Clumped rather than evenly spaced: a regular ring of ticks reads as a
    // laurel wreath, and evenly scattered dashes read as confetti. Real noise
    // comes in gusts, so the angles bunch up.
    const ang = (rand() * Math.PI * 2 + Math.sin(rand() * 9) * 0.5) % (Math.PI * 2);
    // Biased inward so the field crowds the halo and thins as it goes out.
    const r = lerp(rInner, rOuter, rand() ** 2.1);
    const px = cx + Math.cos(ang) * r;
    const py = cy + Math.sin(ang) * r;

    const tangent = ang + Math.PI / 2 + (rand() - 0.5) * 2.4;
    // Lengths spread hard, so the field has grain: some near-dots, some shards.
    const len = lerp(minLen, maxLen, rand() ** 2.2) * scale;
    const hx = (Math.cos(tangent) * len) / 2;
    const hy = (Math.sin(tangent) * len) / 2;

    // Fade with distance so the outer edge dissolves instead of stopping dead.
    const fade = 1 - (r - rInner) / (rOuter - rInner);
    out.push(
      `<path d="M${round(px - hx)} ${round(py - hy)}L${round(px + hx)} ${round(py + hy)}" ` +
        `stroke-width="${round(lerp(width * 0.6, width, rand()) * scale)}" ` +
        `stroke-opacity="${round(lerp(0.22, 0.92, rand()) * (0.3 + 0.7 * fade))}"/>`,
    );
  }
  return out;
}

/**
 * Three cuts of the same mark, because one drawing cannot serve 16 px and
 * 512 px. `full` is the real thing; `bold` thins the field and fattens the cut
 * for 32-48 px; `micro` drops the noise entirely, since below 24 px the
 * splinters only turn into a brown smear around the disc.
 */
const CUTS = {
  full: { count: 88, minLen: 7, maxLen: 46, width: 12, halo: 17, cut: 36, amp: 44, grow: 1 },
  bold: { count: 22, minLen: 13, maxLen: 32, width: 13, halo: 28, cut: 44, amp: 40, grow: 1.7 },
  micro: { count: 0, minLen: 0, maxLen: 0, width: 0, halo: 0, cut: 52, amp: 38, grow: 1 },
};

function mark({ size = 512, tile = false, cut = "full", noise = INK.noise } = {}) {
  const scale = size / 512;
  const cx = size / 2;
  const cy = size / 2;
  const c = CUTS[cut];

  // With no splinters to make room for, the disc can claim the whole frame.
  const rDisc = (c.count === 0 ? 208 : tile ? 148 : 156) * scale;
  const rOuter = (tile ? 222 : 246) * scale;

  const cutWidth = c.cut * scale;
  const cutSpan = rDisc * (c.count === 0 ? 1.32 : 1.62);
  const amp = c.amp * scale * (c.count === 0 ? 1.15 : 1);

  const field = splinters({
    cx,
    cy,
    rInner: rDisc + c.halo * scale,
    rOuter,
    scale: scale * c.grow,
    seed: 0x6b77_6965,
    count: c.count,
    minLen: c.minLen,
    maxLen: c.maxLen,
    width: c.width,
  });

  const cutPath = voicePath(cx - cutSpan / 2, cutSpan, cy, amp);
  const maskId = "quiet";

  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" ` +
    `width="${size}" height="${size}" role="img" aria-label="Kwiet">\n` +
    `  <mask id="${maskId}">\n` +
    `    <circle cx="${cx}" cy="${cy}" r="${round(rDisc)}" fill="#fff"/>\n` +
    `    <path d="${cutPath}" fill="none" stroke="#000" stroke-width="${round(cutWidth)}" ` +
    `stroke-linecap="round" stroke-linejoin="round"/>\n` +
    `  </mask>\n` +
    (tile
      ? `  <rect width="${size}" height="${size}" rx="${round(size * 0.225)}" fill="${INK.ground}"/>\n`
      : "") +
    `  <g stroke="${noise}" stroke-linecap="round" fill="none">\n` +
    field.map((p) => `    ${p}`).join("\n") +
    `\n  </g>\n` +
    `  <circle cx="${cx}" cy="${cy}" r="${round(rDisc)}" fill="${INK.voice}" mask="url(#${maskId})"/>\n` +
    `</svg>\n`
  );
}

// ---------------------------------------------------------------------------
// Wordmark
//
// Drawn as monoline geometry rather than set in a typeface: the mark is built
// from strokes with round caps, so the letters are too, and the lockup holds
// together. It also means the SVG needs no font installed to render, which
// matters on a README that GitHub rasterises on Linux.
//
// Grid: baseline 150, x-height top 50, ascenders from 0, stroke 22.
// ---------------------------------------------------------------------------

const GLYPHS = {
  // Stem full height; arm and leg meet on it, forming one continuous chevron.
  k: { advance: 62, d: ["M0 0L0 150", "M58 50L0 106L56 150"] },
  w: { advance: 100, d: ["M0 50L24 150L50 84L76 150L100 50"] },
  // The dot is a zero-length stroke: the round cap draws it, at the same weight
  // as everything else.
  i: { advance: 4, d: ["M0 50L0 150", "M0 12L0 12"] },
  e: { advance: 100, d: ["M0 100L100 100", "M100 100A50 50 0 1 0 85.36 135.36"] },
  t: { advance: 46, d: ["M23 8L23 150", "M0 50L48 50"] },
};

const STROKE = 22;

/**
 * `height` is the cap height, i.e. the glyph grid. Round caps push half a
 * stroke past the grid on every side, so the reported box adds that back --
 * otherwise every caller that trusts these numbers clips the letters.
 */
function wordmark({ height = 150, colour = INK.voice, tracking = 22 } = {}) {
  const k = height / 150;
  const bleed = (STROKE / 2) * k;
  const parts = [];
  let x = 0;
  for (const ch of "kwiet") {
    const g = GLYPHS[ch];
    parts.push(
      `<g transform="translate(${round(x)} 0)">` +
        g.d.map((d) => `<path d="${d}"/>`).join("") +
        `</g>`,
    );
    x += g.advance + tracking;
  }
  const gridWidth = x - tracking;
  return {
    width: gridWidth * k + bleed * 2,
    height: height + bleed * 2,
    svg:
      `<g transform="translate(${round(bleed)} ${round(bleed)}) scale(${round(k, 4)})" ` +
      `fill="none" stroke="${colour}" stroke-width="${STROKE}" ` +
      `stroke-linecap="round" stroke-linejoin="round">` +
      parts.join("") +
      `</g>`,
  };
}

/** The word on its own, for places that already show the mark nearby. */
function wordmarkOnly({ colour = INK.voice } = {}) {
  const word = wordmark({ height: 92, colour });
  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${round(word.width)} ${round(word.height)}" ` +
    `width="${round(word.width)}" height="${round(word.height)}" role="img" aria-label="Kwiet">\n` +
    `  ${word.svg}\n</svg>\n`
  );
}

/** Mark and wordmark locked up horizontally, for the README and the installer. */
// The mark carries a halo of splinters inside its box, so the disc is only
// about 60% of `markSize`. Matching box heights would leave the disc looking
// dwarfed by the letters -- hence a mark box well over the wordmark's height.
function lockup({
  markSize = 210,
  colour = INK.voice,
  gap = 24,
  noise = INK.noise,
  cut = "full",
} = {}) {
  const word = wordmark({ height: 92, colour });
  const pad = 16;
  const w = markSize + gap + word.width + pad * 2;
  const h = Math.max(markSize, word.height) + pad * 2;
  const markY = pad + (h - pad * 2 - markSize) / 2;
  // Optical, not mathematical: the wordmark's x-height sits low in its box, so
  // centring it on the box leaves it looking to have slipped downward.
  const wordY = pad + (h - pad * 2 - word.height) / 2 - word.height * 0.02;

  const inner = mark({ size: markSize, noise, cut })
    .replace(/^<svg[^>]*>\n?/, "")
    .replace(/<\/svg>\n?$/, "")
    // Two marks on one page would otherwise collide on the mask id.
    .replace(/"quiet"/g, '"quiet-lockup"')
    .replace(/url\(#quiet\)/g, "url(#quiet-lockup)");

  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${round(w)} ${round(h)}" ` +
    `width="${round(w)}" height="${round(h)}" role="img" aria-label="Kwiet">\n` +
    `  <g transform="translate(${pad} ${round(markY)})">\n${inner}  </g>\n` +
    `  <g transform="translate(${round(pad + markSize + gap)} ${round(wordY)})">${word.svg}</g>\n` +
    `</svg>\n`
  );
}

mkdirSync(OUT, { recursive: true });
writeFileSync(join(OUT, "mark-full.svg"), mark());
writeFileSync(join(OUT, "mark-full-tile.svg"), mark({ tile: true }));
writeFileSync(join(OUT, "mark-bold.svg"), mark({ cut: "bold" }));
writeFileSync(join(OUT, "mark-bold-tile.svg"), mark({ tile: true, cut: "bold" }));
writeFileSync(join(OUT, "mark-micro.svg"), mark({ cut: "micro" }));
writeFileSync(join(OUT, "mark-micro-tile.svg"), mark({ tile: true, cut: "micro" }));
writeFileSync(join(OUT, "lockup.svg"), lockup());
// For the installer header strip, where the mark ends up around 40 px tall.
writeFileSync(join(OUT, "lockup-compact.svg"), lockup({ cut: "micro", markSize: 150, gap: 16 }));
writeFileSync(
  join(OUT, "lockup-compact-ink.svg"),
  lockup({ cut: "micro", markSize: 150, gap: 16, colour: INK.ground, noise: INK.noiseOnLight }),
);
writeFileSync(join(OUT, "wordmark.svg"), wordmarkOnly());
writeFileSync(
  join(OUT, "lockup-ink.svg"),
  lockup({ colour: INK.ground, noise: INK.noiseOnLight }),
);
writeFileSync(join(OUT, "mark-full-ink.svg"), mark({ noise: INK.noiseOnLight }));
// The panel's header shows the mark at 18 px, where the splinters are mud.
writeFileSync(join(HERE, "..", "ui", "src", "mark.svg"), mark({ cut: "micro" }));
console.log("wrote", OUT);

// ---------------------------------------------------------------------------
// Rasters
//
// Committed to the repo rather than built in CI, so a release never depends on
// ImageMagick being on the runner. Rerun this script only when the mark changes.
//
// Two icon sets, because they are seen against different things. The product
// icon -- taskbar pin, Start, the .exe, the installer -- carries the dark tile
// and looks like a product. The tray icon must sit on a taskbar that may be
// light or dark, so it is transparent and leans on the disc for contrast.
// ---------------------------------------------------------------------------

function haveMagick() {
  try {
    execFileSync("magick", ["-version"], { stdio: "ignore" });
    return true;
  } catch {
    return false;
  }
}

if (!haveMagick()) {
  console.log("ImageMagick absent: SVG only, rasters left untouched");
  process.exit(0);
}

const render = (svg, out, size) =>
  execFileSync("magick", [
    "-background", "none",
    join(OUT, svg),
    "-resize", `${size}x${size}`,
    out,
  ]);

/** Below 24 px the splinters are mud; 32-48 px takes the thinned field. */
const cutFor = (size) => (size < 28 ? "micro" : size < 64 ? "bold" : "full");

function iconSet(sizes, suffix, dir) {
  return sizes.map((size) => {
    const out = join(dir, `_${size}.png`);
    render(`mark-${cutFor(size)}${suffix}.svg`, out, size);
    return out;
  });
}

mkdirSync(PNG, { recursive: true });
mkdirSync(ICONS, { recursive: true });

const ICO_SIZES = [16, 20, 24, 32, 48, 64, 128, 256];

// Product icon: tiled.
const product = iconSet(ICO_SIZES, "-tile", PNG);
execFileSync("magick", [...product, join(ICONS, "icon.ico")]);

// Tray icon: transparent. Windows picks a frame by DPI, so ship the small ones.
const tray = iconSet([16, 20, 24, 32, 40, 48], "", PNG);
execFileSync("magick", [...tray, join(ICONS, "tray.ico")]);

// Tauri's bundler wants these exact names for the NSIS installer and the
// window icon.
render("mark-full-tile.svg", join(ICONS, "32x32.png"), 32);
render("mark-full-tile.svg", join(ICONS, "128x128.png"), 128);
render("mark-full-tile.svg", join(ICONS, "128x128@2x.png"), 256);
render("mark-full-tile.svg", join(ICONS, "icon.png"), 512);

// For the README, the docs, and anywhere a PNG is easier than an SVG.
render("mark-full.svg", join(PNG, "mark-1024.png"), 1024);
render("mark-full-tile.svg", join(PNG, "mark-tile-1024.png"), 1024);
execFileSync("magick", [
  "-background", "none",
  join(OUT, "lockup.svg"),
  "-resize", "1200x",
  join(PNG, "lockup-1200.png"),
]);

for (const f of [...product, ...tray]) rmSync(f, { force: true });
console.log("wrote", PNG, "and", ICONS);

// The MSI dialogs want 24-bit BMP at exactly these sizes: msiexec draws them at
// their natural size into a fixed dialog, so anything larger is cropped rather
// than scaled down. No DPI trick to play here -- unlike NSIS, the Windows
// Installer UI is not per-monitor aware, so native size is the right size.
//
// 493 px of width is generous compared to what NSIS gave us, which is the other
// reason the artwork can be legible here: the mark has room to be itself.
const WIX = join(HERE, "..", "ui", "src-tauri", "wix");
mkdirSync(WIX, { recursive: true });

const bmp = (width, height, layers, out, ground = INK.ground) =>
  execFileSync("magick", [
    "-size", `${width}x${height}`, `xc:${ground}`,
    ...layers,
    `BMP3:${join(WIX, out)}`,
  ]);

// Both bitmaps are backgrounds that WiX prints its own text over, in black,
// with no way to restyle it. A dark image is therefore not a style choice but a
// bug: the title lands on it and disappears. Hence white where the text goes,
// and the dark brand confined to the left band the text never reaches.

// Top banner of every page after the welcome. The page title is printed at the
// left, so the lockup goes right -- in its ink colours, for a light ground.
bmp(493, 58, [
  "(", "-background", "none", join(OUT, "lockup-compact-ink.svg"), "-resize", "150x40", ")",
  "-gravity", "east", "-geometry", "+18+0", "-composite",
], "banner.bmp", "white");

// Welcome and finish pages: text runs down the right two thirds.
bmp(493, 312, [
  "-fill", INK.ground, "-draw", "rectangle 0,0 163,311",
  "(", "-background", "none", join(OUT, "mark-full.svg"), "-resize", "116x116", ")",
  "-gravity", "northwest", "-geometry", "+24+46", "-composite",
  // The word goes under the mark without its own: the full lockup here would
  // draw the disc twice in one image.
  "(", "-background", "none", join(OUT, "wordmark.svg"), "-resize", "104x", ")",
  "-gravity", "northwest", "-geometry", "+30+200", "-composite",
], "dialog.bmp", "white");

console.log("wrote", WIX);
