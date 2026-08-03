//! Kwiet DSP engine, exposed through the stable C ABI declared in
//! `include/kwiet_dsp.h`.
//!
//! Milestone 2 ships a deliberately trivial effect — a fixed attenuation — so
//! that the host plumbing (SPSC rings, worker thread, dynamic loading) can be
//! validated end to end before DeepFilterNet3 is wired in.
//!
//! # Real-time contract
//!
//! [`kwiet_dsp_process`] runs on the host's worker thread, which feeds a
//! real-time audio path. It must never allocate, lock, or block. It must also
//! never unwind into C, so every entry point catches panics and reports an
//! error code; the host then fails open to passthrough.

#![deny(missing_docs)]

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicU32, Ordering};

/// ABI version reported by [`kwiet_dsp_abi_version`].
const ABI_VERSION: u32 = 1;

/// Success.
const OK: i32 = 0;
/// A required handle was null.
const ERR_HANDLE: i32 = -1;
/// Buffers were null, or the frame count was out of range.
const ERR_ARGS: i32 = -2;
/// A panic was caught inside the engine.
const ERR_INTERNAL: i32 = -3;

/// Upper bound on a single `process` call, generous compared with the host's
/// 10 ms quantum (480 frames at 48 kHz). Guards against a corrupt frame count
/// turning into a huge slice.
const MAX_FRAMES: u32 = 8192;

/// Default attenuation applied until the host sets one, in decibels.
const DEFAULT_ATTENUATION_DB: f32 = -6.0;

/// Attenuation range accepted by [`kwiet_dsp_set_attenuation_db`]. Values
/// outside are clamped rather than rejected: a bad control value must never
/// take the audio path down.
const MIN_ATTENUATION_DB: f32 = -60.0;
const MAX_ATTENUATION_DB: f32 = 0.0;

/// Opaque engine handle shared with the C host.
pub struct KwietDsp {
    sample_rate: u32,
    channels: u32,
    /// Linear gain, stored as `f32::to_bits` so it can be swapped atomically
    /// from the control thread while the worker thread reads it.
    gain_bits: AtomicU32,
}

impl KwietDsp {
    fn new(sample_rate: u32, channels: u32) -> Self {
        Self {
            sample_rate,
            channels,
            gain_bits: AtomicU32::new(db_to_gain(DEFAULT_ATTENUATION_DB).to_bits()),
        }
    }

    /// Applies the current gain to `frames` interleaved samples.
    fn process(&self, input: &[f32], output: &mut [f32]) {
        let gain = f32::from_bits(self.gain_bits.load(Ordering::Relaxed));
        for (dst, src) in output.iter_mut().zip(input) {
            *dst = *src * gain;
        }
    }

    fn set_attenuation_db(&self, db: f32) {
        // NaN would poison every subsequent sample; fall back to the default.
        let db = if db.is_nan() {
            DEFAULT_ATTENUATION_DB
        } else {
            db.clamp(MIN_ATTENUATION_DB, MAX_ATTENUATION_DB)
        };
        self.gain_bits
            .store(db_to_gain(db).to_bits(), Ordering::Relaxed);
    }

    /// Sample count expected for `frames` interleaved frames.
    fn samples_for(&self, frames: u32) -> Option<usize> {
        (frames as usize).checked_mul(self.channels as usize)
    }

    /// Rate the engine was created for. DeepFilterNet3 expects 48 kHz, so the
    /// wrapper that replaces this trivial effect will resample (or refuse)
    /// based on this value.
    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }
}

fn db_to_gain(db: f32) -> f32 {
    10f32.powf(db / 20.0)
}

/// Returns the ABI version of this library.
///
/// The host compares it against `KWIET_DSP_ABI_VERSION` before calling
/// anything else, which catches a stale DLL left in the DriverStore.
#[no_mangle]
pub extern "C" fn kwiet_dsp_abi_version() -> u32 {
    ABI_VERSION
}

/// Creates an engine for the given format.
///
/// Returns null if the format is unsupported or allocation fails. Not
/// real-time safe: call from `LockForProcess`, never from the audio path.
#[no_mangle]
pub extern "C" fn kwiet_dsp_create(sample_rate: u32, channels: u32) -> *mut KwietDsp {
    let result = catch_unwind(|| {
        if sample_rate == 0 || channels == 0 || channels > 16 {
            return std::ptr::null_mut();
        }
        Box::into_raw(Box::new(KwietDsp::new(sample_rate, channels)))
    });
    result.unwrap_or(std::ptr::null_mut())
}

/// Destroys an engine created by [`kwiet_dsp_create`].
///
/// # Safety
///
/// `dsp` must be null, or a pointer returned by [`kwiet_dsp_create`] that has
/// not already been destroyed. No other thread may be inside the engine.
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_destroy(dsp: *mut KwietDsp) {
    if dsp.is_null() {
        return;
    }
    // SAFETY: the caller guarantees `dsp` came from kwiet_dsp_create and is
    // destroyed exactly once, so reclaiming the Box is sound.
    let boxed = unsafe { Box::from_raw(dsp) };
    // Dropping must not unwind into C.
    let _ = catch_unwind(AssertUnwindSafe(move || drop(boxed)));
}

/// Processes one block of interleaved float samples.
///
/// Returns [`OK`] on success, or a negative code; on any negative code the
/// host falls open to passthrough for that block.
///
/// # Safety
///
/// `dsp` must come from [`kwiet_dsp_create`]. `input` and `output` must each
/// point to at least `frames * channels` readable/writable floats. They may be
/// identical (in-place) but must not partially overlap.
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_process(
    dsp: *mut KwietDsp,
    input: *const f32,
    output: *mut f32,
    frames: u32,
) -> i32 {
    // SAFETY: the caller guarantees `dsp` is null or a live engine pointer;
    // `as_ref` turns null into None rather than dereferencing it.
    let Some(engine) = (unsafe { dsp.as_ref() }) else {
        return ERR_HANDLE;
    };
    if input.is_null() || output.is_null() || frames == 0 || frames > MAX_FRAMES {
        return ERR_ARGS;
    }
    let Some(samples) = engine.samples_for(frames) else {
        return ERR_ARGS;
    };

    let result = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the caller guarantees `input` holds `samples` readable floats.
        let src = unsafe { std::slice::from_raw_parts(input, samples) };
        // SAFETY: the caller guarantees `output` holds `samples` writable floats
        // and that the buffers alias exactly or not at all. `process` reads each
        // index before writing it, so exact aliasing stays correct.
        let dst = unsafe { std::slice::from_raw_parts_mut(output, samples) };
        engine.process(src, dst);
    }));

    match result {
        Ok(()) => OK,
        Err(_) => ERR_INTERNAL,
    }
}

/// Sets the attenuation in decibels. Thread-safe and lock-free; may be called
/// while [`kwiet_dsp_process`] runs. Out-of-range values are clamped.
///
/// # Safety
///
/// `dsp` must be null or a pointer from [`kwiet_dsp_create`] that is still alive.
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_set_attenuation_db(dsp: *mut KwietDsp, db: f32) {
    // SAFETY: the caller guarantees `dsp` is null or a live engine pointer.
    let Some(engine) = (unsafe { dsp.as_ref() }) else {
        return;
    };
    engine.set_attenuation_db(db);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine() -> KwietDsp {
        KwietDsp::new(48_000, 2)
    }

    #[test]
    fn default_attenuation_is_minus_six_db() {
        let gain = f32::from_bits(engine().gain_bits.load(Ordering::Relaxed));
        assert!((gain - 0.501_187).abs() < 1e-5);
    }

    #[test]
    fn process_should_apply_gain_to_every_sample() {
        let dsp = engine();
        dsp.set_attenuation_db(-6.0);
        let input = [1.0f32; 8];
        let mut output = [0.0f32; 8];
        dsp.process(&input, &mut output);
        assert!(output.iter().all(|s| (s - 0.501_187).abs() < 1e-5));
    }

    #[test]
    fn process_should_support_zero_db_as_identity() {
        let dsp = engine();
        dsp.set_attenuation_db(0.0);
        let input = [0.25f32, -0.5, 0.75, -1.0];
        let mut output = [0.0f32; 4];
        dsp.process(&input, &mut output);
        assert_eq!(output, input);
    }

    #[test]
    fn set_attenuation_should_clamp_above_range() {
        let dsp = engine();
        dsp.set_attenuation_db(20.0);
        let gain = f32::from_bits(dsp.gain_bits.load(Ordering::Relaxed));
        assert!((gain - 1.0).abs() < 1e-6);
    }

    #[test]
    fn set_attenuation_should_reject_nan() {
        let dsp = engine();
        dsp.set_attenuation_db(f32::NAN);
        let gain = f32::from_bits(dsp.gain_bits.load(Ordering::Relaxed));
        assert!(gain.is_finite());
    }

    #[test]
    fn samples_for_should_account_for_channel_count() {
        assert_eq!(engine().samples_for(480), Some(960));
    }

    #[test]
    fn create_should_retain_sample_rate() {
        assert_eq!(engine().sample_rate(), 48_000);
    }

    #[test]
    fn create_should_reject_zero_channels() {
        assert!(kwiet_dsp_create(48_000, 0).is_null());
    }

    #[test]
    fn process_should_reject_null_handle() {
        let mut out = [0.0f32; 4];
        // SAFETY: null handle is explicitly part of the contract under test.
        let rc = unsafe { kwiet_dsp_process(std::ptr::null_mut(), out.as_ptr(), out.as_mut_ptr(), 2) };
        assert_eq!(rc, ERR_HANDLE);
    }

    #[test]
    fn process_should_reject_oversized_frame_count() {
        let dsp = kwiet_dsp_create(48_000, 2);
        let mut out = [0.0f32; 4];
        // SAFETY: dsp is a valid engine; the frame count is deliberately absurd.
        let rc = unsafe { kwiet_dsp_process(dsp, out.as_ptr(), out.as_mut_ptr(), MAX_FRAMES + 1) };
        assert_eq!(rc, ERR_ARGS);
        // SAFETY: dsp came from kwiet_dsp_create and is destroyed once.
        unsafe { kwiet_dsp_destroy(dsp) };
    }

    #[test]
    fn process_should_work_in_place() {
        let dsp = kwiet_dsp_create(48_000, 1);
        // SAFETY: dsp is valid; setting the control value is thread-safe.
        unsafe { kwiet_dsp_set_attenuation_db(dsp, 0.0) };
        let mut buf = [0.5f32; 4];
        let ptr = buf.as_mut_ptr();
        // SAFETY: in-place processing with exactly aliasing buffers is allowed.
        let rc = unsafe { kwiet_dsp_process(dsp, ptr, ptr, 4) };
        assert_eq!(rc, OK);
        assert_eq!(buf, [0.5f32; 4]);
        // SAFETY: dsp came from kwiet_dsp_create and is destroyed once.
        unsafe { kwiet_dsp_destroy(dsp) };
    }
}
