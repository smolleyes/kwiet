//! Access to the APO's shared-memory control block.
//!
//! Mirrors `apo/src/KwietControl.h`. That header is the contract: any change
//! there must be reflected here and bump `EXPECTED_VERSION`.
//!
//! The block only exists while a capture stream is open — the APO creates it at
//! `LockForProcess`. So every read tries to (re)open it, and the UI is expected
//! to render an idle state when it is absent.

use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};

use serde::Serialize;
use windows::core::w;
use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::System::Memory::{
    MapViewOfFile, OpenFileMappingW, UnmapViewOfFile, FILE_MAP_READ, FILE_MAP_WRITE,
    MEMORY_MAPPED_VIEW_ADDRESS,
};

const MAPPING_NAME: windows::core::PCWSTR = w!("Global\\KwietControlV1");
const EXPECTED_MAGIC: u32 = 0x5449_574B; // 'KWIT'
const EXPECTED_VERSION: u32 = 1;

pub const AGGRESSIVENESS_MIN_DB: f64 = 0.0;
pub const AGGRESSIVENESS_MAX_DB: f64 = 100.0;
pub const AGGRESSIVENESS_DEFAULT_DB: f64 = 50.0;

const PEAK_SCALE: f64 = 32767.0;
/// Floor of the meters, in dB. Below this the signal is drawn as silence.
pub const METER_FLOOR_DB: f64 = -60.0;

#[repr(C)]
struct ControlBlock {
    magic: AtomicU32,
    version: AtomicU32,
    enabled: AtomicI32,
    aggressiveness_tenths: AtomicI32,
    streaming: AtomicI32,
    generation: AtomicI32,
    sample_rate: AtomicI32,
    channels: AtomicI32,
    latency_frames: AtomicI32,
    peak_in: AtomicI32,
    peak_out: AtomicI32,
    underruns: AtomicI32,
    dsp_errors: AtomicI32,
    dsp_active: AtomicI32,
    _reserved: [i32; 16],
}

/// What the UI renders. Levels are already converted to dB so the frontend
/// never has to know about the wire format.
#[derive(Serialize, Clone, Copy, Default)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    /// False when no capture stream is open: the block does not exist.
    pub present: bool,
    pub streaming: bool,
    /// True when the DSP pipeline runs; false means the APO is a passthrough.
    pub dsp_active: bool,
    pub enabled: bool,
    pub aggressiveness_db: f64,
    pub generation: i32,
    pub sample_rate: i32,
    pub channels: i32,
    pub latency_ms: f64,
    /// Meter levels in dB, floored at [`METER_FLOOR_DB`].
    pub level_in_db: f64,
    pub level_out_db: f64,
    pub underruns: i32,
    pub dsp_errors: i32,
}

fn peak_to_db(peak: i32) -> f64 {
    if peak <= 0 {
        return METER_FLOOR_DB;
    }
    let db = 20.0 * (f64::from(peak) / PEAK_SCALE).log10();
    db.max(METER_FLOOR_DB)
}

/// A mapping of the control block, held only for the duration of one operation.
pub struct Control {
    handle: HANDLE,
    block: *mut ControlBlock,
}

// SAFETY: the block contains only atomics, and every access below goes through
// them. The pointer itself is never mutated after `open`.
unsafe impl Send for Control {}

impl Control {
    fn open() -> Option<Self> {
        // SAFETY: constant, NUL-terminated name; failure is reported by a null
        // handle, which we check.
        let handle =
            unsafe { OpenFileMappingW(FILE_MAP_READ.0 | FILE_MAP_WRITE.0, false, MAPPING_NAME) }
                .ok()?;

        // SAFETY: `handle` is a valid section handle; a null base is checked.
        let view: MEMORY_MAPPED_VIEW_ADDRESS = unsafe {
            MapViewOfFile(
                handle,
                FILE_MAP_READ | FILE_MAP_WRITE,
                0,
                0,
                std::mem::size_of::<ControlBlock>(),
            )
        };
        if view.Value.is_null() {
            // SAFETY: `handle` came from OpenFileMappingW and is closed once.
            unsafe {
                let _ = CloseHandle(handle);
            };
            return None;
        }

        let block = view.Value.cast::<ControlBlock>();
        // SAFETY: the view is at least size_of::<ControlBlock>() bytes.
        let this = Self { handle, block };
        let b = this.block();
        if b.magic.load(Ordering::Acquire) != EXPECTED_MAGIC
            || b.version.load(Ordering::Acquire) != EXPECTED_VERSION
        {
            return None;
        }
        Some(this)
    }

    fn block(&self) -> &ControlBlock {
        // SAFETY: `block` points at a mapped view that outlives `self`.
        unsafe { &*self.block }
    }

    fn snapshot(&self) -> Snapshot {
        let b = self.block();
        let sample_rate = b.sample_rate.load(Ordering::Relaxed);
        let latency_frames = b.latency_frames.load(Ordering::Relaxed);
        Snapshot {
            present: true,
            streaming: b.streaming.load(Ordering::Acquire) != 0,
            dsp_active: b.dsp_active.load(Ordering::Relaxed) != 0,
            enabled: b.enabled.load(Ordering::Relaxed) != 0,
            aggressiveness_db: f64::from(b.aggressiveness_tenths.load(Ordering::Relaxed)) / 10.0,
            generation: b.generation.load(Ordering::Relaxed),
            sample_rate,
            channels: b.channels.load(Ordering::Relaxed),
            latency_ms: if sample_rate > 0 {
                f64::from(latency_frames) * 1000.0 / f64::from(sample_rate)
            } else {
                0.0
            },
            level_in_db: peak_to_db(b.peak_in.load(Ordering::Relaxed)),
            level_out_db: peak_to_db(b.peak_out.load(Ordering::Relaxed)),
            underruns: b.underruns.load(Ordering::Relaxed),
            dsp_errors: b.dsp_errors.load(Ordering::Relaxed),
        }
    }

    fn set_enabled(&self, enabled: bool) {
        self.block()
            .enabled
            .store(i32::from(enabled), Ordering::Relaxed);
    }

    fn set_aggressiveness(&self, db: f64) {
        let clamped = db.clamp(AGGRESSIVENESS_MIN_DB, AGGRESSIVENESS_MAX_DB);
        self.block()
            .aggressiveness_tenths
            .store((clamped * 10.0).round() as i32, Ordering::Relaxed);
    }
}

impl Drop for Control {
    fn drop(&mut self) {
        // SAFETY: both came from open() and are released exactly once.
        unsafe {
            let _ = UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS {
                Value: self.block.cast(),
            });
            let _ = CloseHandle(self.handle);
        }
    }
}

/// Opens the block for each operation instead of holding it open.
///
/// Keeping the mapping alive looked like a free way to carry the user's
/// settings across streams, since the APO's next `CreateFileMapping` would find
/// the section and leave it as it was. It cost correctness. A named section
/// lives as long as *any* handle to it, so whenever audiodg tore the APO down
/// without calling `UnlockForProcess` -- which is what happens when the pack is
/// deselected in Settings -- the block stayed mapped with `streaming` still set,
/// and the panel went on reporting a stream that had ended: full status line,
/// no signal, and the overlay that should have said why hidden behind it.
///
/// Opening per call makes the section's existence mean exactly what the panel
/// reads it to mean: the APO is loaded and holding it. The settings that used to
/// ride along are re-pushed by the watcher thread as soon as it sees a new
/// generation, so nothing is lost but a few hundred milliseconds at the APO's
/// defaults when a stream starts.
pub struct ControlHandle;

impl ControlHandle {
    /// With no stream the block does not exist, so `present` is false and the
    /// settings fields are left at zero. The caller fills them from what the
    /// user actually chose — putting placeholder values here made the panel
    /// overwrite the user's own click 40 ms after they made it.
    pub fn snapshot(&mut self) -> Snapshot {
        match Control::open() {
            Some(control) => control.snapshot(),
            None => Snapshot::default(),
        }
    }

    pub fn set_enabled(&mut self, enabled: bool) {
        if let Some(control) = Control::open() {
            control.set_enabled(enabled);
        }
    }

    pub fn set_aggressiveness(&mut self, db: f64) {
        if let Some(control) = Control::open() {
            control.set_aggressiveness(db);
        }
    }
}
