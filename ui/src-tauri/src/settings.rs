//! User settings, persisted next to the app's config.
//!
//! The control block is volatile — it disappears with the audio stream — so
//! preferences have to live somewhere durable and be pushed back each time a
//! stream appears.

use std::path::PathBuf;
use std::sync::OnceLock;

use serde::{Deserialize, Serialize};
use tauri::Manager;

use crate::control::{AGGRESSIVENESS_DEFAULT_DB, AGGRESSIVENESS_MAX_DB, AGGRESSIVENESS_MIN_DB};

/// Resolved once at startup so `save` needs nothing but `&self`.
static PATH: OnceLock<Option<PathBuf>> = OnceLock::new();

#[derive(Serialize, Deserialize, Clone, Copy)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    pub enabled: bool,
    pub aggressiveness_db: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            enabled: true,
            aggressiveness_db: AGGRESSIVENESS_DEFAULT_DB,
        }
    }
}

impl Settings {
    pub fn load(app: &tauri::AppHandle) -> Self {
        let path = PATH.get_or_init(|| {
            app.path()
                .app_config_dir()
                .ok()
                .map(|dir| dir.join("settings.json"))
        });

        let Some(path) = path else {
            return Self::default();
        };
        let Ok(text) = std::fs::read_to_string(path) else {
            return Self::default();
        };
        let mut settings: Self = serde_json::from_str(&text).unwrap_or_default();
        // A hand-edited file must not be able to push the engine out of range.
        settings.aggressiveness_db = settings
            .aggressiveness_db
            .clamp(AGGRESSIVENESS_MIN_DB, AGGRESSIVENESS_MAX_DB);
        settings
    }

    /// Best effort: losing a preference is not worth interrupting the user.
    pub fn save(&self) {
        let Some(Some(path)) = PATH.get() else {
            return;
        };
        if let Some(parent) = path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if let Ok(text) = serde_json::to_string_pretty(self) {
            let _ = std::fs::write(path, text);
        }
    }
}
