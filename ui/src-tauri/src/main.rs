// Kwiet tray UI. No console window on release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod control;
mod i18n;
mod pack;
mod settings;

use std::sync::Mutex;
use std::time::Duration;

use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{Manager, PhysicalPosition, State, WindowEvent};
use windows::core::w;
use windows::Win32::Foundation::{POINT, RECT};
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, MonitorFromPoint, MONITORINFO, MONITOR_DEFAULTTOPRIMARY,
};
use windows::Win32::UI::Shell::ShellExecuteW;
use windows::Win32::UI::WindowsAndMessaging::{GetCursorPos, SW_SHOWNORMAL};

use control::{ControlHandle, Snapshot};
use pack::PackStatus;
use settings::Settings;

struct AppState {
    control: Mutex<ControlHandle>,
    settings: Mutex<Settings>,
    /// Refreshed by a background thread: it costs a registry sweep plus a COM
    /// call, which is too much to run at the meter's frame rate.
    pack: Mutex<PackStatus>,
}

#[tauri::command]
fn snapshot(state: State<'_, AppState>) -> Snapshot {
    let mut control = state.control.lock().expect("control lock");
    control.snapshot()
}

#[tauri::command]
fn pack_status(state: State<'_, AppState>) -> PackStatus {
    state.pack.lock().expect("pack lock").clone()
}

/// Opens the Sound page, the only place Windows 11 lets an effect pack be
/// chosen.
///
/// Deliberately not `ms-settings:sound-defaultinputproperties`, which sounds
/// like the exact page we want and is not: on Windows 11 26200 it opens a
/// properties page for a phantom device titled "Null description", with no
/// enhancements section at all. `ms-settings:sound` lands on the Sound page,
/// where the microphone is one click away, and it works.
#[tauri::command]
fn open_microphone_settings() {
    // SAFETY: constant, NUL-terminated strings. Fire and forget: if Settings
    // refuses to open there is nothing useful left to tell the user.
    unsafe {
        let _ = ShellExecuteW(
            None,
            w!("open"),
            w!("ms-settings:sound"),
            None,
            None,
            SW_SHOWNORMAL,
        );
    }
}

/// The panel asks for this before its first paint, so it can start in the right
/// language instead of flashing one and settling on the other.
#[tauri::command]
fn language(state: State<'_, AppState>) -> String {
    let preference = state
        .settings
        .lock()
        .expect("settings lock")
        .language
        .clone();
    i18n::resolve(preference.as_deref())
}

#[tauri::command]
fn set_language(app: tauri::AppHandle, state: State<'_, AppState>, language: String) {
    let chosen = i18n::resolve(Some(&language));
    {
        let mut settings = state.settings.lock().expect("settings lock");
        settings.language = Some(chosen.clone());
        settings.save();
    }
    // The tray is outside the web view, so it has to be relabelled by hand.
    if let Some(tray) = app.tray_by_id("kwiet") {
        if let Ok(menu) = build_tray_menu(&app, &chosen) {
            let _ = tray.set_menu(Some(menu));
        }
        let _ = tray.set_tooltip(Some(i18n::strings(&chosen).tooltip));
    }
}

fn build_tray_menu(app: &tauri::AppHandle, language: &str) -> tauri::Result<Menu<tauri::Wry>> {
    let strings = i18n::strings(language);
    let show = MenuItem::with_id(app, "show", strings.open, true, None::<&str>)?;
    let separator = PredefinedMenuItem::separator(app)?;
    let quit = MenuItem::with_id(app, "quit", strings.quit, true, None::<&str>)?;
    Menu::with_items(app, &[&show, &separator, &quit])
}

#[tauri::command]
fn set_enabled(state: State<'_, AppState>, enabled: bool) {
    state
        .control
        .lock()
        .expect("control lock")
        .set_enabled(enabled);
    let mut settings = state.settings.lock().expect("settings lock");
    settings.enabled = enabled;
    settings.save();
}

#[tauri::command]
fn set_aggressiveness(state: State<'_, AppState>, db: f64) {
    state
        .control
        .lock()
        .expect("control lock")
        .set_aggressiveness(db);
    let mut settings = state.settings.lock().expect("settings lock");
    settings.aggressiveness_db = db;
    settings.save();
}

/// Usable area of the screen the pointer is on, i.e. the desktop minus the
/// taskbar. Taken from the pointer rather than the primary monitor so that on a
/// multi-screen desk the panel opens next to the tray icon that was clicked.
fn work_area() -> Option<RECT> {
    // SAFETY: both calls take pointers to stack values we own, and each result
    // is checked before the struct is read.
    unsafe {
        let mut point = POINT::default();
        GetCursorPos(&mut point).ok()?;
        let monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        let mut info = MONITORINFO {
            cbSize: std::mem::size_of::<MONITORINFO>() as u32,
            ..Default::default()
        };
        if GetMonitorInfoW(monitor, &mut info).as_bool() {
            Some(info.rcWork)
        } else {
            None
        }
    }
}

/// Anchors the panel where a tray application belongs: bottom right, tucked
/// above the taskbar. `rcWork` already excludes the taskbar wherever the user
/// has put it, so a docked-left or top taskbar lands correctly too.
fn anchor_to_tray(window: &tauri::WebviewWindow) {
    const MARGIN: i32 = 12;
    let (Some(work), Ok(size)) = (work_area(), window.outer_size()) else {
        return;
    };
    // Both are physical pixels, so no scale factor is involved.
    let x = work.right - size.width as i32 - MARGIN;
    let y = work.bottom - size.height as i32 - MARGIN;
    let _ = window.set_position(PhysicalPosition::new(x.max(work.left), y.max(work.top)));
}

fn toggle_panel(app: &tauri::AppHandle) {
    let Some(window) = app.get_webview_window("panel") else {
        return;
    };
    if window.is_visible().unwrap_or(false) {
        let _ = window.hide();
    } else {
        // Re-anchored on every open: the taskbar may have moved, or the click
        // may have come from a different screen than last time.
        anchor_to_tray(&window);
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            let settings = Settings::load(app.handle());
            app.manage(AppState {
                control: Mutex::new(ControlHandle),
                settings: Mutex::new(settings),
                pack: Mutex::new(PackStatus::default()),
            });

            // Installed-but-not-selected is invisible from inside the audio
            // stack, so it has to be polled from outside it. Slowly: the answer
            // only changes when somebody visits Settings.
            let handle = app.handle().clone();
            std::thread::spawn(move || {
                pack::init_com_for_thread();
                loop {
                    let status = pack::status();
                    let state: State<'_, AppState> = handle.state();
                    *state.pack.lock().expect("pack lock") = status;
                    std::thread::sleep(Duration::from_secs(2));
                }
            });

            // The control block only lives for the duration of a stream, and a
            // fresh one starts at the APO's defaults. Watching `generation`
            // lets us re-push the user's settings as soon as a stream appears,
            // without the UI having to be open.
            let handle = app.handle().clone();
            std::thread::spawn(move || {
                let mut pushed_generation = i32::MIN;
                loop {
                    {
                        let state: State<'_, AppState> = handle.state();
                        let snap = state.control.lock().expect("control lock").snapshot();
                        if snap.present && snap.generation != pushed_generation {
                            let settings = state.settings.lock().expect("settings lock").clone();
                            let mut control = state.control.lock().expect("control lock");
                            control.set_enabled(settings.enabled);
                            control.set_aggressiveness(settings.aggressiveness_db);
                            pushed_generation = snap.generation;
                        } else if !snap.present {
                            // Stream gone: push again when the next one shows up.
                            pushed_generation = i32::MIN;
                        }
                    }
                    std::thread::sleep(Duration::from_millis(300));
                }
            });

            let language = {
                let state: State<'_, AppState> = app.state();
                let preference = state
                    .settings
                    .lock()
                    .expect("settings lock")
                    .language
                    .clone();
                i18n::resolve(preference.as_deref())
            };
            let menu = build_tray_menu(app.handle(), &language)?;

            // A dedicated tray cut: transparent, and drawn without the splinter
            // field, which turns to mud below 24 px. The window icon keeps its
            // dark tile, which is right for the taskbar and wrong for the tray.
            let tray_icon = tauri::image::Image::from_bytes(include_bytes!("../icons/tray.ico"))?;

            TrayIconBuilder::with_id("kwiet")
                .icon(tray_icon)
                .tooltip(i18n::strings(&language).tooltip)
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id().as_ref() {
                    "show" => toggle_panel(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        toggle_panel(tray.app_handle());
                    }
                })
                .build(app)?;

            Ok(())
        })
        .on_window_event(|window, event| {
            // Closing the panel returns it to the tray rather than quitting:
            // the point of the app is to keep running.
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .invoke_handler(tauri::generate_handler![
            snapshot,
            pack_status,
            open_microphone_settings,
            language,
            set_language,
            set_enabled,
            set_aggressiveness
        ])
        .run(tauri::generate_context!())
        .expect("Kwiet UI failed to start");
}
