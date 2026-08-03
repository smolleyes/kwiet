//! Is the effect pack installed, and is Windows actually using it?
//!
//! Installing is not enough. Windows 11 requires the user to pick the pack by
//! hand in Settings > Sound > <microphone> > Audio enhancements, and a pack
//! that is installed but not picked is completely inert: no error, no log line,
//! nothing anywhere says so. Reading that silence as a bug in the APO cost a
//! whole evening, so the panel now names the state outright.
//!
//! Two registry facts answer it, both written by Windows itself:
//!
//! * the pack is installed when its component appears under the software device
//!   enumerator;
//! * the pack is selected on an endpoint when that endpoint's `FxProperties`
//!   carries a value keyed by our own CLSID.
//!
//! *Which* endpoint matters comes from COM: the default microphone is not
//! something the registry answers reliably.
//!
//! Everything here is read-only and needs no elevation.

use serde::Serialize;
use windows::Win32::Media::Audio::{
    eCapture, eCommunications, IMMDeviceEnumerator, MMDeviceEnumerator,
};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoTaskMemFree, CLSCTX_ALL, COINIT_MULTITHREADED,
};
use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

/// Mirrors `ExtensionId` in `installer/effectpack/kwiet_extension.inf`.
const EXTENSION_ID: &str = "{6a05fc42-1a06-48a3-907c-740f7f7ca2bf}";
/// Mirrors `AddComponent` in the same file.
const COMPONENT: &str = "kwieteffectpack";
/// Mirrors `CLSID_KwietApo` in `apo/src/KwietGuids.h` and the copy in
/// `installer/lib/common.ps1`. Windows keys the selection marker by the pack's
/// own CLSID; we match on the prefix rather than a specific property id, since
/// the id is an undocumented implementation detail we only know by observation.
const SELECTION_PREFIX: &str = "{65d564e6-9709-4f5c-85cf-449d92949cfe}";

const CAPTURE_KEY: &str = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture";
const DRIVER_ENUM_KEY: &str = r"SYSTEM\CurrentControlSet\Enum\SWD\DriverEnum";
/// PKEY_Device_FriendlyName, as the endpoint registry spells it.
const FRIENDLY_NAME: &str = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";
const DEVICE_STATE_ACTIVE: u32 = 1;

/// What the panel needs to tell the user why nothing is happening.
#[derive(Serialize, Clone, PartialEq, Eq, Default, Debug)]
#[serde(rename_all = "camelCase")]
pub struct PackStatus {
    /// The driver package is present on this machine.
    pub installed: bool,
    /// Kwiet is selected on the microphone Windows hands to applications.
    pub selected_on_default: bool,
    /// Friendly name of that microphone, when it could be read.
    pub default_device: Option<String>,
    /// Active microphones where Kwiet is selected but which are *not* the
    /// default one. This is the quiet trap: everything looks installed, the
    /// pack is genuinely selected, and still nothing runs.
    pub selected_elsewhere: Vec<String>,
}

/// COM has to be live on whichever thread calls [`status`].
pub fn init_com_for_thread() {
    // SAFETY: called once per worker thread. A thread already initialised in
    // another apartment answers RPC_E_CHANGED_MODE, which is fine -- COM stays
    // usable for the one call we make.
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }
}

fn pack_installed() -> bool {
    let prefix = format!("{EXTENSION_ID}#{COMPONENT}&");
    RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(DRIVER_ENUM_KEY, KEY_READ)
        .map(|key| {
            key.enum_keys()
                .flatten()
                .any(|name| name.to_ascii_lowercase().starts_with(&prefix))
        })
        .unwrap_or(false)
}

fn selected_on(endpoint: &RegKey) -> bool {
    endpoint
        .open_subkey_with_flags("FxProperties", KEY_READ)
        .map(|fx| {
            fx.enum_values()
                .flatten()
                .any(|(name, _)| name.to_ascii_lowercase().starts_with(SELECTION_PREFIX))
        })
        .unwrap_or(false)
}

/// The communications role, not the console one: it is the role every meeting
/// app asks for, and the two can differ.
fn default_capture_device_id() -> Option<String> {
    // SAFETY: standard MMDevice enumeration. Every call is checked, and the
    // string GetId hands back is freed with the allocator COM expects.
    unsafe {
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL).ok()?;
        let device = enumerator
            .GetDefaultAudioEndpoint(eCapture, eCommunications)
            .ok()?;
        let id = device.GetId().ok()?;
        let text = id.to_string().ok();
        CoTaskMemFree(Some(id.0.cast()));
        text
    }
}

/// Endpoint ids look like `{0.0.1.00000000}.{guid}`; the registry keys them by
/// the trailing brace group alone.
fn endpoint_key_name(device_id: &str) -> Option<&str> {
    device_id.rfind("}.{").map(|i| &device_id[i + 2..])
}

pub fn status() -> PackStatus {
    let mut status = PackStatus {
        installed: pack_installed(),
        ..Default::default()
    };

    let default_key = default_capture_device_id()
        .as_deref()
        .and_then(endpoint_key_name)
        .map(str::to_ascii_lowercase);

    let Ok(capture) = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey_with_flags(CAPTURE_KEY, KEY_READ)
    else {
        return status;
    };

    for name in capture.enum_keys().flatten() {
        let Ok(endpoint) = capture.open_subkey_with_flags(&name, KEY_READ) else {
            continue;
        };
        // Unplugged and disabled microphones are noise here: a pack selected on
        // a headset that is in a drawer is not a problem to report.
        if endpoint.get_value::<u32, _>("DeviceState").unwrap_or(0) != DEVICE_STATE_ACTIVE {
            continue;
        }

        let lowered = name.to_ascii_lowercase();
        let is_default = default_key.as_deref() == Some(lowered.as_str());
        let friendly = endpoint
            .open_subkey_with_flags("Properties", KEY_READ)
            .ok()
            .and_then(|p| p.get_value::<String, _>(FRIENDLY_NAME).ok());

        if is_default {
            status.default_device = friendly.clone();
        }
        if selected_on(&endpoint) {
            if is_default {
                status.selected_on_default = true;
            } else {
                status
                    .selected_elsewhere
                    .push(friendly.unwrap_or_else(|| "micro sans nom".to_owned()));
            }
        }
    }

    status
}

#[cfg(test)]
mod tests {
    use super::{endpoint_key_name, init_com_for_thread, status};

    /// Not a unit test: it reports what this machine actually looks like, which
    /// is the only way to check the detection against reality. Run it by hand:
    /// `cargo test -- --ignored --nocapture`.
    #[test]
    #[ignore = "reads live machine state"]
    fn report_this_machine() {
        init_com_for_thread();
        println!("{:#?}", status());
    }

    #[test]
    fn endpoint_key_name_keeps_only_the_trailing_brace_group() {
        assert_eq!(
            endpoint_key_name("{0.0.1.00000000}.{b2c3d4e5-0000-1111-2222-333344445555}"),
            Some("{b2c3d4e5-0000-1111-2222-333344445555}")
        );
    }

    #[test]
    fn endpoint_key_name_rejects_a_string_that_is_not_an_endpoint_id() {
        assert_eq!(endpoint_key_name("{0.0.1.00000000}"), None);
    }
}
