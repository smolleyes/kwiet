# ui/ — the Kwiet panel (Tauri v2)

A tray application: stream state, cleaning strength, meters. It **never** speaks
to the real-time thread — only to the shared-memory block described in
[`../apo/src/KwietControl.h`](../apo/src/KwietControl.h), which is the contract.

```powershell
npm install
npm run dev      # development, frontend reload
npm run build    # MSI installers in src-tauri/target/release/bundle/msi
```

## What the backend does

| File | Role |
|---|---|
| `src-tauri/src/control.rs` | Opens `Global\KwietControlV1`, converts peaks to dB, clamps the strength |
| `src-tauri/src/pack.rs` | Reads whether the effect pack is installed, and whether Windows is actually using it |
| `src-tauri/src/i18n.rs` | The three strings that live outside the web view: tray menu and tooltip |
| `src-tauri/src/settings.rs` | Persists preferences in the app's config directory |
| `src-tauri/src/main.rs` | Tray icon, window anchoring, commands, and the thread watching `generation` |

Two constraints shape the design:

- **The block only exists while a capture stream is open.** The APO creates it at
  `LockForProcess`, so the panel must render an idle state when it is absent and
  retry the open on every poll.
- **A new stream starts from the APO's defaults.** A background thread watches
  the `generation` counter and re-pushes the user's preferences as soon as a
  stream appears, even with the panel closed.

The block is opened per operation rather than held. Holding it looked free and
cost correctness: a named section lives as long as any handle, so the panel went
on reporting a stream that had already ended.

Writing needs **no elevation** — the ACL the APO puts on the section allows it
(see `docs/architecture.md` §6).

## Detecting a pack that is installed but not chosen

Windows 11 requires the user to pick the effect pack by hand, and a pack that is
installed but not picked is entirely inert: no error, no log line, nothing
anywhere says so. The panel names the state outright, from two registry facts
Windows writes itself plus a COM call for which microphone actually receives the
applications. It distinguishes: pack missing, installed but not chosen, chosen on
a microphone that is not the default one, ready, and running.

## The visual stance

The subject of this product is the gap between two signals, so the panel makes
that gap its centrepiece. A scrolling history stacks the raw envelope (amber)
over the cleaned one (celadon); the amber visible above the celadon **is** the
noise removed. Stop talking and the celadon collapses while the amber stays —
you watch the product work. The meter says the same thing about the present
instant, in one bar.

Instrument-panel typography: **Bahnschrift** (the variable DIN that ships with
Windows) for labels, **Consolas** for anything numeric so digits stay in their
columns. No font is downloaded.

Slate blue-green ground, two signal colours only. No third hue: what is kept,
what is removed, nothing else.

## Scope

Deliberately limited to **control and diagnosis**. Installing and removing the
pack belongs to the installer; the panel reports what it finds and points at the
Windows page that fixes it.
