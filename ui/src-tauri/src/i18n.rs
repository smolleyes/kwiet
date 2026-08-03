//! The handful of strings that live outside the web view: tray menu and tooltip.
//!
//! The panel keeps its own dictionary in `src/i18n.js`. Duplicating three
//! strings is cheaper than plumbing one dictionary across the process boundary,
//! and the tray has to be labelled before the web view has even loaded.

use windows::Win32::Globalization::GetUserDefaultLocaleName;

/// The two we actually wrote. Anything else falls back to English.
pub const LANGUAGES: [&str; 2] = ["fr", "en"];

pub struct Strings {
    pub open: &'static str,
    pub quit: &'static str,
    pub tooltip: &'static str,
}

pub fn strings(language: &str) -> Strings {
    match language {
        "fr" => Strings {
            open: "Ouvrir Kwiet",
            quit: "Quitter",
            tooltip: "Kwiet — nettoyage du micro",
        },
        _ => Strings {
            open: "Open Kwiet",
            quit: "Quit",
            tooltip: "Kwiet — microphone cleaning",
        },
    }
}

pub fn resolve(preference: Option<&str>) -> String {
    match preference {
        Some(lang) if LANGUAGES.contains(&lang) => lang.to_owned(),
        _ => system_language(),
    }
}

fn system_language() -> String {
    // LOCALE_NAME_MAX_LENGTH.
    let mut buffer = [0u16; 85];
    // SAFETY: the buffer is the documented maximum size, and the returned length
    // is checked before it is used to slice.
    let len = unsafe { GetUserDefaultLocaleName(&mut buffer) };
    if len <= 1 {
        return "en".to_owned();
    }
    // The count includes the terminating NUL.
    let name = String::from_utf16_lossy(&buffer[..(len - 1) as usize]);
    if name.to_ascii_lowercase().starts_with("fr") {
        "fr".to_owned()
    } else {
        "en".to_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::resolve;

    #[test]
    fn resolve_keeps_a_language_we_ship() {
        assert_eq!(resolve(Some("fr")), "fr");
    }

    #[test]
    fn resolve_rejects_a_language_we_do_not_ship() {
        // Falls through to the system language, which is one of ours either way.
        assert!(["fr", "en"].contains(&resolve(Some("de")).as_str()));
    }
}
