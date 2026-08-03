//! Offline calibration harness: runs a WAV file through the same C ABI the APO
//! uses, at a chosen attenuation limit.
//!
//! Lets the aggressiveness be tuned by ear without re-recording or touching the
//! installed pack:
//!
//! ```text
//! cargo run --release --example enhance -- in.wav out.wav 30
//! ```

use std::path::Path;

use kwiet_dsp::{
    kwiet_dsp_block_frames, kwiet_dsp_create, kwiet_dsp_destroy, kwiet_dsp_process,
    kwiet_dsp_set_attenuation_db,
};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 4 {
        eprintln!("usage: enhance <in.wav> <out.wav> <atten_lim_db>");
        std::process::exit(2);
    }
    let atten: f32 = args[3].parse().expect("attenuation must be a number");

    let mut reader = hound::WavReader::open(Path::new(&args[1])).expect("cannot open input");
    let spec = reader.spec();
    assert_eq!(spec.sample_rate, 48_000, "DeepFilterNet3 needs 48 kHz");

    // Normalise to f32 whatever the file stores.
    let samples: Vec<f32> = match spec.sample_format {
        hound::SampleFormat::Float => reader.samples::<f32>().map(|s| s.unwrap()).collect(),
        hound::SampleFormat::Int => {
            let scale = 1.0 / (1i64 << (spec.bits_per_sample - 1)) as f32;
            reader.samples::<i32>().map(|s| s.unwrap() as f32 * scale).collect()
        }
    };

    let channels = spec.channels as u32;
    let dsp = kwiet_dsp_create(spec.sample_rate, channels);
    assert!(!dsp.is_null(), "engine creation failed");
    // SAFETY: `dsp` is a live engine, used from this thread only.
    unsafe { kwiet_dsp_set_attenuation_db(dsp, atten) };
    // SAFETY: same.
    let hop = unsafe { kwiet_dsp_block_frames(dsp) } as usize;

    let block = hop * channels as usize;
    let mut output = vec![0.0f32; samples.len()];
    let mut processed = 0usize;
    for (src, dst) in samples
        .chunks_exact(block)
        .zip(output.chunks_exact_mut(block))
    {
        // SAFETY: both slices hold exactly `hop * channels` floats and do not
        // overlap; single-threaded.
        let rc = unsafe { kwiet_dsp_process(dsp, src.as_ptr(), dst.as_mut_ptr(), hop as u32) };
        assert_eq!(rc, 0, "process failed");
        processed += block;
    }
    // Tail shorter than one block: pass it through untouched.
    output[processed..].copy_from_slice(&samples[processed..]);

    // SAFETY: created above, destroyed once, nothing else is inside it.
    unsafe { kwiet_dsp_destroy(dsp) };

    let mut writer = hound::WavWriter::create(
        Path::new(&args[2]),
        hound::WavSpec {
            channels: spec.channels,
            sample_rate: spec.sample_rate,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        },
    )
    .expect("cannot create output");
    for s in &output {
        let v = (s.clamp(-1.0, 1.0) * 32767.0) as i16;
        writer.write_sample(v).expect("write failed");
    }
    writer.finalize().expect("finalize failed");

    println!("{} -> {} at {atten} dB", args[1], args[2]);
}
