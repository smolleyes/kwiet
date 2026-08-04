//! Kwiet DSP engine, exposed through the stable C ABI declared in
//! `include/kwiet_dsp.h`.
//!
//! Wraps [DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet) (crate
//! `deep_filter`, tract/ONNX backend, model embedded at build time) behind a
//! handful of C entry points, so the C++ APO never links Rust types.
//!
//! # Threading contract
//!
//! [`kwiet_dsp_process`] is called by the host's **single** worker thread and
//! is the only entry point allowed to touch the model. The control entry
//! points may be called from any thread at any time; they only publish atomics
//! that `process` picks up on its next call.
//!
//! # Real-time contract
//!
//! `process` runs off the audio thread (the host buffers through lock-free
//! rings), so it may allocate — tract does, per inference. It must still never
//! block, and it must never unwind into C: every entry point catches panics
//! and reports an error code, on which the host fails open to passthrough.

#![deny(missing_docs)]

use std::cell::UnsafeCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

// The `deep_filter` package builds a library named `df`, so that is the name
// the crate is referred to by.
use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use sonora::config::{Config, EchoCanceller};
use sonora::{AudioProcessing, StreamConfig};

/// ABI version reported by [`kwiet_dsp_abi_version`].
///
/// v2 replaced the milestone-2 placeholder gain with DeepFilterNet3, which
/// changed what `kwiet_dsp_set_attenuation_db` means and added
/// [`kwiet_dsp_block_frames`].
///
/// v3 added acoustic echo cancellation ahead of the denoiser, and with it
/// [`kwiet_dsp_process_render`] for the loopback reference.
const ABI_VERSION: u32 = 3;

/// Success.
const OK: i32 = 0;
/// A required handle was null.
const ERR_HANDLE: i32 = -1;
/// Buffers were null, or the frame count was not a positive multiple of the
/// block size.
const ERR_ARGS: i32 = -2;
/// The model failed, or a panic was caught.
const ERR_INTERNAL: i32 = -3;

/// DeepFilterNet3 is trained for 48 kHz and nothing else.
const REQUIRED_SAMPLE_RATE: u32 = 48_000;

/// Interleaved channel counts we are willing to downmix.
const MAX_CHANNELS: u32 = 16;

/// Upper bound on one `process` call, well above the host's 10 ms quantum.
const MAX_FRAMES: u32 = 8192;

/// Maximum noise attenuation, in dB, applied when the host sets nothing.
///
/// Deliberately below DeepFilterNet's own default of 100 dB: suppressing all
/// the way to digital silence clips speech onsets and pumps audibly, whereas a
/// residual noise floor masks both.
///
/// This value has been challenged and holds. The limit is a hard cap, not a
/// hint: `measure_whistle_across_the_aggressiveness_range` shows a steady tone
/// leaving attenuated by exactly the limit -- 20.5 dB at 20, 50.5 at 50, 80.5
/// at 80. That made 75 dB look like the obvious fix for a whistle still audible
/// at 50. Tried in real use: **75 dB destroys the voice.**
///
/// The lesson is about the measurement, not the number. A synthetic tone with
/// no speech present says nothing about what a cap does to speech, because the
/// cap applies to whatever the network decides to suppress -- and near a voice
/// it decides differently. Raising the default requires listening tests on
/// speech, not a sweep on a sine.
///
/// Kept in step with `KWIET_AGGRESSIVENESS_DEFAULT_TENTHS` in
/// apo/src/KwietControl.h and `AGGRESSIVENESS_DEFAULT_DB` in
/// ui/src-tauri/src/control.rs.
const DEFAULT_ATTEN_LIM_DB: f32 = 50.0;

/// Accepted range for the attenuation limit. Out-of-range values are clamped
/// rather than rejected: a bad control value must never take audio down.
const MIN_ATTEN_LIM_DB: f32 = 0.0;
const MAX_ATTEN_LIM_DB: f32 = 100.0;

/// Model state plus the scratch buffers used to bridge interleaved host audio
/// and DeepFilterNet's mono, planar frames.
struct Inner {
    df: DfTract,
    /// WebRTC's AEC3, ported to Rust. Runs *before* the denoiser: echo is a
    /// correlated copy of a signal we are given, which an adaptive filter
    /// removes far better than a denoiser trained on unrelated noise -- and
    /// leaving it to the denoiser would mean asking it to erase speech.
    aec: AudioProcessing,
    mono_in: Vec<f32>,
    /// Near-end after echo cancellation, before denoising.
    mono_aec: Vec<f32>,
    mono_out: Vec<f32>,
    /// Reference frames downmixed to mono for the canceller's render path.
    mono_render: Vec<f32>,
    render_scratch: Vec<f32>,
}

/// Opaque engine handle shared with the C host.
pub struct KwietDsp {
    sample_rate: u32,
    channels: u32,
    /// Frames DeepFilterNet consumes per inference; `process` requires a
    /// multiple of this.
    hop: u32,
    /// Pending attenuation limit, published by any thread as `f32::to_bits`.
    atten_db_bits: AtomicU32,
    /// Set when `atten_db_bits` changed and the model has yet to be told.
    atten_dirty: AtomicBool,
    inner: UnsafeCell<Inner>,
}

// SAFETY: `inner` is reachable only from kwiet_dsp_process, which the host
// contract pins to a single worker thread. Every field another thread can
// touch is atomic, so sharing `&KwietDsp` across threads is sound.
unsafe impl Sync for KwietDsp {}
// SAFETY: the handle carries no thread-affine state; the host creates it on
// one thread and uses it on the worker.
unsafe impl Send for KwietDsp {}

impl KwietDsp {
    fn new(sample_rate: u32, channels: u32) -> Option<Self> {
        if sample_rate != REQUIRED_SAMPLE_RATE || channels == 0 || channels > MAX_CHANNELS {
            return None;
        }

        // Mono model: the host's interleaved channels are downmixed in, and
        // the enhanced mono result is written back to every channel.
        let runtime = RuntimeParams::default_with_ch(1).with_atten_lim(DEFAULT_ATTEN_LIM_DB);
        let params = DfParams::default();
        let df = DfTract::new(params, &runtime).ok()?;

        let hop = df.hop_size;
        if hop == 0 {
            return None;
        }

        // Echo cancellation only. Noise suppression and gain control stay off:
        // DeepFilterNet3 does the first far better, and the second belongs to
        // the application, which is already doing it.
        let aec = AudioProcessing::builder()
            .config(Config {
                echo_canceller: Some(EchoCanceller::default()),
                ..Default::default()
            })
            .capture_config(StreamConfig::new(REQUIRED_SAMPLE_RATE, 1))
            .render_config(StreamConfig::new(REQUIRED_SAMPLE_RATE, 1))
            .build();

        Some(Self {
            sample_rate,
            channels,
            hop: hop as u32,
            atten_db_bits: AtomicU32::new(DEFAULT_ATTEN_LIM_DB.to_bits()),
            atten_dirty: AtomicBool::new(false),
            inner: UnsafeCell::new(Inner {
                df,
                aec,
                mono_in: vec![0.0; hop],
                mono_aec: vec![0.0; hop],
                mono_out: vec![0.0; hop],
                mono_render: vec![0.0; hop],
                render_scratch: vec![0.0; hop],
            }),
        })
    }

    /// Feeds one block of the far-end (loopback) signal to the echo canceller.
    ///
    /// Called from the same worker thread as [`Self::process`], before it, so
    /// the canceller sees what the speakers played before it sees what the
    /// microphone heard.
    ///
    /// # Safety
    ///
    /// Caller must guarantee no other thread is inside the engine.
    unsafe fn process_render(&self, render: &[f32], channels: usize) -> Result<(), i32> {
        // SAFETY: single-worker contract, see the type-level comment.
        let inner = unsafe { &mut *self.inner.get() };
        let hop = self.hop as usize;
        if channels == 0 || render.len() != hop * channels {
            return Err(ERR_ARGS);
        }

        let inv = 1.0 / channels as f32;
        for (frame, slot) in render
            .chunks_exact(channels)
            .zip(inner.mono_render.iter_mut())
        {
            *slot = frame.iter().sum::<f32>() * inv;
        }

        let Inner {
            aec,
            mono_render,
            render_scratch,
            ..
        } = inner;
        // The render path wants an output buffer it may write; we discard it.
        aec.process_render_f32(&[mono_render], &mut [render_scratch])
            .map_err(|_| ERR_INTERNAL)?;
        Ok(())
    }

    /// Enhances `input` into `output`, both interleaved and of the same length.
    ///
    /// # Safety
    ///
    /// Caller must guarantee no other thread is inside this method.
    unsafe fn process(&self, input: &[f32], output: &mut [f32]) -> Result<(), i32> {
        // SAFETY: single-worker contract, see the type-level comment.
        let inner = unsafe { &mut *self.inner.get() };

        if self.atten_dirty.swap(false, Ordering::Relaxed) {
            let db = f32::from_bits(self.atten_db_bits.load(Ordering::Relaxed));
            inner.df.set_atten_lim(db);
        }

        let Inner {
            df,
            aec,
            mono_in,
            mono_aec,
            mono_out,
            ..
        } = inner;
        let channels = self.channels as usize;
        let hop = self.hop as usize;
        let inv_channels = 1.0 / channels as f32;

        for block in input
            .chunks_exact(hop * channels)
            .zip(output.chunks_exact_mut(hop * channels))
        {
            let (src, dst) = block;

            for (frame, slot) in src.chunks_exact(channels).zip(mono_in.iter_mut()) {
                *slot = frame.iter().sum::<f32>() * inv_channels;
            }

            // Echo first, then noise. If no reference has ever been fed the
            // canceller sees silence on its render path and passes the signal
            // through, which is the right degraded behaviour.
            aec.process_capture_f32(&[mono_in], &mut [mono_aec])
                .map_err(|_| ERR_INTERNAL)?;

            let noisy = ArrayView2::from_shape((1, hop), mono_aec).map_err(|_| ERR_INTERNAL)?;
            let enh = ArrayViewMut2::from_shape((1, hop), mono_out).map_err(|_| ERR_INTERNAL)?;
            df.process(noisy, enh).map_err(|_| ERR_INTERNAL)?;

            for (frame, value) in dst.chunks_exact_mut(channels).zip(mono_out.iter()) {
                frame.fill(*value);
            }
        }
        Ok(())
    }

    fn set_attenuation_db(&self, db: f32) {
        // NaN would be published straight into the model; fall back to the
        // default instead.
        let db = if db.is_nan() {
            DEFAULT_ATTEN_LIM_DB
        } else {
            db.clamp(MIN_ATTEN_LIM_DB, MAX_ATTEN_LIM_DB)
        };
        self.atten_db_bits.store(db.to_bits(), Ordering::Relaxed);
        self.atten_dirty.store(true, Ordering::Relaxed);
    }

    /// Rate the engine was created for; always [`REQUIRED_SAMPLE_RATE`].
    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }
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
/// Returns null if the format is unsupported — notably any rate other than
/// 48 kHz, which DeepFilterNet3 does not handle — or if the model fails to
/// load. The host then stays in passthrough, which is a valid degraded mode.
/// Not real-time safe: loads and optimises the network.
#[no_mangle]
pub extern "C" fn kwiet_dsp_create(sample_rate: u32, channels: u32) -> *mut KwietDsp {
    let result = catch_unwind(|| match KwietDsp::new(sample_rate, channels) {
        Some(dsp) => Box::into_raw(Box::new(dsp)),
        None => std::ptr::null_mut(),
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

/// Frames consumed per inference. [`kwiet_dsp_process`] requires its frame
/// count to be a non-zero multiple of this value; the host uses it as its
/// worker block size.
///
/// Returns 0 for a null handle.
///
/// # Safety
///
/// `dsp` must be null or a live pointer from [`kwiet_dsp_create`].
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_block_frames(dsp: *const KwietDsp) -> u32 {
    // SAFETY: `as_ref` turns null into None instead of dereferencing it.
    match unsafe { dsp.as_ref() } {
        Some(engine) => engine.hop,
        None => 0,
    }
}

/// Enhances one or more blocks of interleaved float samples.
///
/// `frames` must be a non-zero multiple of [`kwiet_dsp_block_frames`].
/// Returns [`OK`], or a negative code on which the host falls open to
/// passthrough for that block.
///
/// # Safety
///
/// `dsp` must come from [`kwiet_dsp_create`]. `input` and `output` must each
/// point to at least `frames * channels` readable/writable floats. They may be
/// identical (in-place) but must not partially overlap. Only one thread may be
/// inside this function at a time.
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_process(
    dsp: *mut KwietDsp,
    input: *const f32,
    output: *mut f32,
    frames: u32,
) -> i32 {
    // SAFETY: `as_ref` turns null into None instead of dereferencing it.
    let Some(engine) = (unsafe { dsp.as_ref() }) else {
        return ERR_HANDLE;
    };
    if input.is_null() || output.is_null() || frames == 0 || frames > MAX_FRAMES {
        return ERR_ARGS;
    }
    if frames % engine.hop != 0 {
        return ERR_ARGS;
    }
    let Some(samples) = (frames as usize).checked_mul(engine.channels as usize) else {
        return ERR_ARGS;
    };

    let result = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the caller guarantees `input` holds `samples` readable floats.
        let src = unsafe { std::slice::from_raw_parts(input, samples) };
        // SAFETY: the caller guarantees `output` holds `samples` writable floats
        // and that the buffers alias exactly or not at all. Each block is fully
        // read into the mono scratch before the output slice is written.
        let dst = unsafe { std::slice::from_raw_parts_mut(output, samples) };
        // SAFETY: the caller guarantees single-threaded use of `process`.
        unsafe { engine.process(src, dst) }
    }));

    match result {
        Ok(Ok(())) => OK,
        Ok(Err(code)) => code,
        Err(_) => ERR_INTERNAL,
    }
}

/// Feeds one block of the far-end reference (what the speakers are playing) to
/// the echo canceller.
///
/// `frames` must equal [`kwiet_dsp_block_frames`]; `channels` is the reference
/// stream's own channel count and is downmixed internally. Call this from the
/// same worker thread as [`kwiet_dsp_process`], before it, for each block.
///
/// Returns [`OK`], or a negative code. A failure here is not fatal: the host
/// may keep processing capture, and the canceller simply has nothing to
/// subtract.
///
/// # Safety
///
/// `dsp` must come from [`kwiet_dsp_create`]. `render` must point to at least
/// `frames * channels` readable floats. Only one thread may be inside the
/// engine at a time.
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_process_render(
    dsp: *mut KwietDsp,
    render: *const f32,
    frames: u32,
    channels: u32,
) -> i32 {
    // SAFETY: `as_ref` turns null into None instead of dereferencing it.
    let Some(engine) = (unsafe { dsp.as_ref() }) else {
        return ERR_HANDLE;
    };
    if render.is_null() || frames != engine.hop || channels == 0 || channels > MAX_CHANNELS {
        return ERR_ARGS;
    }
    let Some(samples) = (frames as usize).checked_mul(channels as usize) else {
        return ERR_ARGS;
    };

    let result = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the caller guarantees `render` holds `samples` readable floats.
        let src = unsafe { std::slice::from_raw_parts(render, samples) };
        // SAFETY: the caller guarantees single-threaded use of the engine.
        unsafe { engine.process_render(src, channels as usize) }
    }));

    match result {
        Ok(Ok(())) => OK,
        Ok(Err(code)) => code,
        Err(_) => ERR_INTERNAL,
    }
}

/// Sets the **maximum noise attenuation** in decibels: 0 leaves the signal
/// untouched, 100 lets DeepFilterNet suppress as much as it wants. This is the
/// aggressiveness control, not a gain on the signal.
///
/// Thread-safe and lock-free; the value is picked up by the next
/// [`kwiet_dsp_process`]. Out-of-range values are clamped.
///
/// # Safety
///
/// `dsp` must be null or a pointer from [`kwiet_dsp_create`] that is still alive.
#[no_mangle]
pub unsafe extern "C" fn kwiet_dsp_set_attenuation_db(dsp: *mut KwietDsp, db: f32) {
    // SAFETY: `as_ref` turns null into None instead of dereferencing it.
    let Some(engine) = (unsafe { dsp.as_ref() }) else {
        return;
    };
    engine.set_attenuation_db(db);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Building the model is slow, so the tests that need one share it.
    fn engine() -> KwietDsp {
        KwietDsp::new(48_000, 2).expect("DeepFilterNet3 should load at 48 kHz")
    }

    #[test]
    fn create_should_reject_non_48k_rates() {
        assert!(KwietDsp::new(44_100, 2).is_none());
    }

    #[test]
    fn create_should_reject_zero_channels() {
        assert!(KwietDsp::new(48_000, 0).is_none());
    }

    #[test]
    fn block_frames_should_be_the_model_hop() {
        let dsp = engine();
        assert!(dsp.hop > 0);
        assert_eq!(dsp.hop, 480, "DeepFilterNet3 hops 10 ms at 48 kHz");
    }

    #[test]
    fn process_should_reject_a_partial_block() {
        let dsp = Box::into_raw(Box::new(engine()));
        let mut buf = vec![0.0f32; 64];
        // SAFETY: valid engine; the frame count is deliberately not a multiple
        // of the hop size.
        let rc = unsafe { kwiet_dsp_process(dsp, buf.as_ptr(), buf.as_mut_ptr(), 32) };
        assert_eq!(rc, ERR_ARGS);
        // SAFETY: created just above, destroyed once.
        unsafe { kwiet_dsp_destroy(dsp) };
    }

    #[test]
    fn process_should_accept_one_block_of_stereo() {
        let dsp = engine();
        let hop = dsp.hop as usize;
        let input = vec![0.0f32; hop * 2];
        let mut output = vec![0.0f32; hop * 2];
        // SAFETY: this test is single-threaded.
        let rc = unsafe { dsp.process(&input, &mut output) };
        assert!(rc.is_ok());
    }

    #[test]
    fn process_should_write_both_channels_identically() {
        let dsp = engine();
        let hop = dsp.hop as usize;
        let input: Vec<f32> = (0..hop * 2).map(|i| (i as f32 * 0.001).sin()).collect();
        let mut output = vec![0.0f32; hop * 2];
        // SAFETY: this test is single-threaded.
        unsafe { dsp.process(&input, &mut output) }.expect("process should succeed");
        assert!(output.chunks_exact(2).all(|f| f[0] == f[1]));
    }

    #[test]
    fn set_attenuation_should_clamp_below_range() {
        let dsp = engine();
        dsp.set_attenuation_db(-20.0);
        let db = f32::from_bits(dsp.atten_db_bits.load(Ordering::Relaxed));
        assert_eq!(db, MIN_ATTEN_LIM_DB);
    }

    #[test]
    fn set_attenuation_should_reject_nan() {
        let dsp = engine();
        dsp.set_attenuation_db(f32::NAN);
        let db = f32::from_bits(dsp.atten_db_bits.load(Ordering::Relaxed));
        assert!(db.is_finite());
    }

    #[test]
    fn set_attenuation_should_flag_the_model_as_stale() {
        let dsp = engine();
        dsp.atten_dirty.store(false, Ordering::Relaxed);
        dsp.set_attenuation_db(42.0);
        assert!(dsp.atten_dirty.load(Ordering::Relaxed));
    }

    #[test]
    fn create_should_retain_sample_rate() {
        assert_eq!(engine().sample_rate(), 48_000);
    }

    #[test]
    fn process_should_reject_null_handle() {
        let mut out = [0.0f32; 4];
        // SAFETY: null handle is explicitly part of the contract under test.
        let rc =
            unsafe { kwiet_dsp_process(std::ptr::null_mut(), out.as_ptr(), out.as_mut_ptr(), 480) };
        assert_eq!(rc, ERR_HANDLE);
    }

    #[test]
    fn block_frames_should_be_zero_for_null_handle() {
        // SAFETY: null handle is explicitly part of the contract under test.
        assert_eq!(unsafe { kwiet_dsp_block_frames(std::ptr::null()) }, 0);
    }

    /// Deterministic white-ish noise, so the measurement below does not depend
    /// on an rng crate and stays reproducible.
    fn noise_signal(len: usize) -> Vec<f32> {
        let mut state = 0x2545_F491_4F6C_DD1Du64;
        (0..len)
            .map(|_| {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                (state >> 40) as f32 / 8_388_608.0 - 1.0
            })
            .collect()
    }

    fn measure_rtf(dsp: &KwietDsp, input: &[f32], blocks: usize) -> f64 {
        let mut output = vec![0.0f32; input.len()];
        for _ in 0..20 {
            // SAFETY: this test is single-threaded.
            unsafe { dsp.process(input, &mut output) }.expect("warmup should succeed");
        }
        let start = std::time::Instant::now();
        for _ in 0..blocks {
            // SAFETY: this test is single-threaded.
            unsafe { dsp.process(input, &mut output) }.expect("process should succeed");
        }
        let elapsed = start.elapsed().as_secs_f64();
        let audio = blocks as f64 * dsp.hop as f64 / REQUIRED_SAMPLE_RATE as f64;
        elapsed / audio
    }

    fn rms(samples: &[f32]) -> f32 {
        (samples.iter().map(|s| s * s).sum::<f32>() / samples.len() as f32).sqrt()
    }

    /// Proves the network is actually running, not just being carried through:
    /// broadband noise is exactly what DeepFilterNet is trained to remove.
    #[test]
    fn process_should_attenuate_broadband_noise() {
        let dsp = engine();
        let block = dsp.hop as usize * dsp.channels as usize;
        // A long, non-repeating signal: feeding one block on a loop would look
        // periodic, which is not what the model is meant to suppress.
        let blocks = 60;
        let noise = noise_signal(block * blocks);
        let mut output = vec![0.0f32; block];

        let mut tail_in = 0.0f32;
        let mut tail_out = 0.0f32;
        for (i, chunk) in noise.chunks_exact(block).enumerate() {
            // SAFETY: this test is single-threaded.
            unsafe { dsp.process(chunk, &mut output) }.expect("process should succeed");
            // Skip the first blocks: the model needs context to settle.
            if i >= blocks / 2 {
                tail_in += rms(chunk);
                tail_out += rms(&output);
            }
        }

        let ratio = tail_out / tail_in;
        println!("noise kept after DeepFilterNet3: {:.1} %", ratio * 100.0);
        assert!(
            ratio < 0.5,
            "noise barely attenuated (ratio {ratio:.3}) -- is the model actually running?"
        );
    }

    /// A sustained whistle is where DeepFilterNet3 is weakest: it is harmonic
    /// and sits in the voice band, so the network reads it as speech and keeps
    /// it. Windows' own Voice Clarity removes it, which is a gap worth
    /// measuring rather than arguing about.
    ///
    /// This also answers whether WebRTC's classical noise suppressor -- which
    /// ships in the same crate as the echo canceller -- would close that gap: a
    /// steady tone is exactly what a spectral noise-floor estimator is good at.
    #[test]
    fn measure_whistle_versus_speech_band_noise() {
        let dsp = engine();
        let hop = dsp.hop as usize;
        let channels = dsp.channels as usize;
        const BLOCKS: usize = 120;

        let rms_of = |v: &[f32]| (v.iter().map(|s| s * s).sum::<f32>() / v.len() as f32).sqrt();
        let db = |x: f32| 20.0 * x.max(1e-12).log10();

        // A 2 kHz whistle, steady, at a modest level.
        let mut input = vec![0.0f32; hop * channels];
        let mut output = vec![0.0f32; hop * channels];
        let mut phase = 0.0f32;
        let (mut sum_in, mut sum_out) = (0.0f32, 0.0f32);

        for block in 0..BLOCKS {
            for frame in 0..hop {
                phase += 2000.0 * std::f32::consts::TAU / REQUIRED_SAMPLE_RATE as f32;
                let s = phase.sin() * 0.15;
                for ch in 0..channels {
                    input[frame * channels + ch] = s;
                }
            }
            // SAFETY: single-threaded test, buffers sized to the engine's block.
            unsafe { dsp.process(&input, &mut output) }.expect("process should succeed");
            // Let the network settle before measuring.
            if block >= BLOCKS - 40 {
                sum_in += rms_of(&input);
                sum_out += rms_of(&output);
            }
        }

        let kept = db(sum_out / 40.0) - db(sum_in / 40.0);
        println!("whistle attenuation through DeepFilterNet3: {:.1} dB", -kept);
    }

    /// Sweeps the aggressiveness control against a steady whistle, because the
    /// question it answers is a product one: where should the default sit?
    #[test]
    fn measure_whistle_across_the_aggressiveness_range() {
        let dsp = engine();
        let hop = dsp.hop as usize;
        let channels = dsp.channels as usize;
        let rms_of = |v: &[f32]| (v.iter().map(|s| s * s).sum::<f32>() / v.len() as f32).sqrt();
        let db = |x: f32| 20.0 * x.max(1e-12).log10();

        for limit in [20.0f32, 35.0, 50.0, 65.0, 80.0, 100.0] {
            dsp.set_attenuation_db(limit);
            let mut input = vec![0.0f32; hop * channels];
            let mut output = vec![0.0f32; hop * channels];
            let mut phase = 0.0f32;
            let (mut sum_in, mut sum_out) = (0.0f32, 0.0f32);
            const BLOCKS: usize = 120;

            for block in 0..BLOCKS {
                for frame in 0..hop {
                    phase += 2000.0 * std::f32::consts::TAU / REQUIRED_SAMPLE_RATE as f32;
                    let s = phase.sin() * 0.15;
                    for ch in 0..channels {
                        input[frame * channels + ch] = s;
                    }
                }
                // SAFETY: single-threaded test.
                unsafe { dsp.process(&input, &mut output) }.expect("process should succeed");
                if block >= BLOCKS - 40 {
                    sum_in += rms_of(&input);
                    sum_out += rms_of(&output);
                }
            }
            let attenuation = db(sum_in / 40.0) - db(sum_out / 40.0);
            println!("  limit {limit:5.0} dB -> whistle attenuated {attenuation:5.1} dB");
        }
    }

    /// The echo canceller has to actually cancel, through the same C ABI the
    /// APO uses -- not just inside the crate. Far end is a tone; the near end
    /// is that tone leaking back, delayed, plus a quiet local voice.
    #[test]
    fn process_should_cancel_a_delayed_echo() {
        let dsp = engine();
        let hop = dsp.hop as usize;
        let channels = dsp.channels as usize;
        const DELAY: usize = 120; // 2.5 ms of acoustic path
        const BLOCKS: usize = 300; // 3 s, enough for the filter to converge

        let mut tail = vec![0.0f32; DELAY];
        let mut render = vec![0.0f32; hop];
        let mut capture = vec![0.0f32; hop * channels];
        let mut output = vec![0.0f32; hop * channels];
        let mut phase = 0.0f32;
        let (mut sum_in, mut sum_out) = (0.0f32, 0.0f32);

        for block in 0..BLOCKS {
            for slot in render.iter_mut() {
                phase += 440.0 * std::f32::consts::TAU / REQUIRED_SAMPLE_RATE as f32;
                *slot = phase.sin() * 0.5;
            }
            // The echo is the render signal delayed by DELAY samples.
            for i in 0..hop {
                let echoed = if i < DELAY {
                    tail[i]
                } else {
                    render[i - DELAY]
                } * 0.63;
                let voice = ((block * hop + i) as f32 * 0.017).sin() * 0.02;
                for ch in 0..channels {
                    capture[i * channels + ch] = echoed + voice;
                }
            }
            tail.copy_from_slice(&render[hop - DELAY..]);

            // SAFETY: this test is single-threaded, and both buffers are sized
            // to the block the engine was created for.
            unsafe { dsp.process_render(&render, 1) }.expect("render should succeed");
            // SAFETY: same.
            unsafe { dsp.process(&capture, &mut output) }.expect("process should succeed");

            if block >= BLOCKS - 50 {
                sum_in += rms(&capture);
                sum_out += rms(&output);
            }
        }

        let db = |x: f32| 20.0 * x.max(1e-12).log10();
        let cancelled = db(sum_in / 50.0) - db(sum_out / 50.0);
        println!("echo cancelled: {cancelled:.1} dB");
        assert!(
            cancelled > 10.0,
            "echo barely cancelled ({cancelled:.1} dB) -- is AEC3 actually wired in?"
        );
    }

    #[test]
    fn process_render_should_reject_a_wrong_block_size() {
        let dsp = Box::into_raw(Box::new(engine()));
        let buf = vec![0.0f32; 64];
        // SAFETY: valid engine; the frame count deliberately is not the hop.
        let rc = unsafe { kwiet_dsp_process_render(dsp, buf.as_ptr(), 64, 1) };
        assert_eq!(rc, ERR_ARGS);
        // SAFETY: created just above, destroyed once.
        unsafe { kwiet_dsp_destroy(dsp) };
    }

    /// The whole architecture rests on the worker keeping up: it has one
    /// block-time to process one block. Anything close to 1.0 means the rings
    /// drain and the APO spends its life failing open to passthrough.
    ///
    /// Measured on noise, not on a tone: DeepFilterNet skips its heavier
    /// stages when the local SNR is high, so a clean tone flatters the result.
    #[test]
    fn process_should_run_well_faster_than_real_time() {
        let dsp = engine();
        let samples = dsp.hop as usize * dsp.channels as usize;
        let blocks = 200;

        let tone: Vec<f32> = (0..samples)
            .map(|i| (i as f32 * 0.02).sin() * 0.2)
            .collect();
        let rtf_tone = measure_rtf(&dsp, &tone, blocks);

        let noise = noise_signal(samples);
        let rtf_noise = measure_rtf(&dsp, &noise, blocks);

        println!("DeepFilterNet3 real-time factor: tone {rtf_tone:.3}, noise {rtf_noise:.3}");
        let worst = rtf_tone.max(rtf_noise);
        assert!(
            worst < 0.5,
            "real-time factor {worst:.3} leaves no headroom for the worker"
        );
    }
}
