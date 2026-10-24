//! Game keyboard input for Lua mods.
//!
//! A key specification names one key of the keyboard (or an extra mouse
//! button), optionally with Ctrl, Alt and Shift: `F4`, `R`, `Shift+F4`,
//! `Ctrl+Alt+Delete`. Names are case-insensitive; `canonical` gives the
//! stable spelling returned to mods.
//!
//! `is_key_down` samples the current state, like the original API. Presses
//! of keys a mod `watch`es are also latched by a sampler thread that runs
//! every few milliseconds, so a short tap between two `on_update` calls is
//! still delivered by `take_presses`. Keys are only read while the game
//! window is focused, never for other applications.

use std::collections::{HashMap, VecDeque};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

const VK_SHIFT: u16 = 0x10;
const VK_CONTROL: u16 = 0x11;
const VK_MENU: u16 = 0x12;

/// Presses kept per mod between two `take_presses` calls; older ones drop.
const MAX_QUEUED_PRESSES: usize = 64;
/// Sampling period while at least one mod watches keys.
const SAMPLE_PERIOD: Duration = Duration::from_millis(4);
/// Sampling period while no mod watches keys.
const IDLE_PERIOD: Duration = Duration::from_millis(100);

/// Canonical key names and their Windows virtual-key codes.
const NAMED_KEYS: &[(&str, u16)] = &[
    ("Backspace", 0x08),
    ("Tab", 0x09),
    ("Enter", 0x0D),
    ("Pause", 0x13),
    ("CapsLock", 0x14),
    ("Escape", 0x1B),
    ("Space", 0x20),
    ("PageUp", 0x21),
    ("PageDown", 0x22),
    ("End", 0x23),
    ("Home", 0x24),
    ("Left", 0x25),
    ("Up", 0x26),
    ("Right", 0x27),
    ("Down", 0x28),
    ("Insert", 0x2D),
    ("Delete", 0x2E),
    ("Numpad0", 0x60),
    ("Numpad1", 0x61),
    ("Numpad2", 0x62),
    ("Numpad3", 0x63),
    ("Numpad4", 0x64),
    ("Numpad5", 0x65),
    ("Numpad6", 0x66),
    ("Numpad7", 0x67),
    ("Numpad8", 0x68),
    ("Numpad9", 0x69),
    ("NumpadMultiply", 0x6A),
    ("NumpadAdd", 0x6B),
    ("NumpadSubtract", 0x6D),
    ("NumpadDecimal", 0x6E),
    ("NumpadDivide", 0x6F),
    ("Semicolon", 0xBA),
    ("Equals", 0xBB),
    ("Comma", 0xBC),
    ("Minus", 0xBD),
    ("Period", 0xBE),
    ("Slash", 0xBF),
    ("Backquote", 0xC0),
    ("LeftBracket", 0xDB),
    ("Backslash", 0xDC),
    ("RightBracket", 0xDD),
    ("Quote", 0xDE),
    ("MouseMiddle", 0x04),
    ("Mouse4", 0x05),
    ("Mouse5", 0x06),
];

const ALIASES: &[(&str, &str)] = &[
    ("esc", "Escape"),
    ("return", "Enter"),
    ("del", "Delete"),
    ("ins", "Insert"),
    ("pgup", "PageUp"),
    ("pgdn", "PageDown"),
    ("pagedn", "PageDown"),
];

/// One key with its required modifier state. Modifiers must match exactly:
/// `F4` does not fire while Shift is held, `Shift+F4` does.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct Combo {
    key: u16,
    ctrl: bool,
    alt: bool,
    shift: bool,
}

impl Combo {
    /// Stable spelling: modifiers in Ctrl, Alt, Shift order, then the key.
    pub(crate) fn canonical(&self) -> String {
        let mut parts = Vec::new();
        if self.ctrl {
            parts.push("Ctrl".to_owned());
        }
        if self.alt {
            parts.push("Alt".to_owned());
        }
        if self.shift {
            parts.push("Shift".to_owned());
        }
        parts.push(key_name(self.key));
        parts.join("+")
    }
}

fn modifier_code(name: &str) -> Option<u16> {
    match name.to_ascii_lowercase().as_str() {
        "shift" => Some(VK_SHIFT),
        "ctrl" | "control" => Some(VK_CONTROL),
        "alt" => Some(VK_MENU),
        _ => None,
    }
}

fn key_code(name: &str) -> Option<u16> {
    let lower = name.to_ascii_lowercase();
    let lower = ALIASES
        .iter()
        .find(|(alias, _)| *alias == lower)
        .map(|(_, target)| target.to_ascii_lowercase())
        .unwrap_or(lower);
    if let Some((_, code)) = NAMED_KEYS
        .iter()
        .find(|(known, _)| known.to_ascii_lowercase() == lower)
    {
        return Some(*code);
    }
    let bytes = lower.as_bytes();
    if bytes.len() == 1 && (bytes[0].is_ascii_lowercase() || bytes[0].is_ascii_digit()) {
        // Virtual-key codes of letters and digits are their uppercase ASCII.
        return Some(bytes[0].to_ascii_uppercase() as u16);
    }
    if let Some(number) = lower.strip_prefix('f').and_then(|n| n.parse::<u16>().ok()) {
        if (1..=24).contains(&number) {
            return Some(0x70 + number - 1);
        }
    }
    None
}

fn key_name(code: u16) -> String {
    match code {
        VK_SHIFT => "Shift".to_owned(),
        VK_CONTROL => "Ctrl".to_owned(),
        VK_MENU => "Alt".to_owned(),
        0x30..=0x39 | 0x41..=0x5A => (code as u8 as char).to_string(),
        0x70..=0x87 => format!("F{}", code - 0x70 + 1),
        _ => NAMED_KEYS
            .iter()
            .find(|(_, known)| *known == code)
            .map(|(name, _)| (*name).to_owned())
            .unwrap_or_else(|| format!("0x{code:02X}")),
    }
}

/// Parse a key specification such as `Shift+F4`. A lone modifier (`Shift`)
/// is accepted: `is_key_down("Shift")` reads that modifier itself.
pub(crate) fn parse(spec: &str) -> Result<Combo, String> {
    let parts: Vec<&str> = spec.split('+').map(str::trim).collect();
    if parts.iter().any(|part| part.is_empty()) {
        return Err(format!("invalid key specification '{spec}'"));
    }
    let (mut ctrl, mut alt, mut shift) = (false, false, false);
    let mut key = None;
    for (index, part) in parts.iter().enumerate() {
        let last = index + 1 == parts.len();
        match modifier_code(part) {
            Some(code) if !last || parts.len() > 1 => {
                let flag = match code {
                    VK_CONTROL => &mut ctrl,
                    VK_MENU => &mut alt,
                    _ => &mut shift,
                };
                if *flag {
                    return Err(format!("modifier repeated in '{spec}'"));
                }
                *flag = true;
            }
            Some(code) => key = Some(code),
            None if last => {
                key = Some(key_code(part).ok_or_else(|| format!("unknown key '{part}' in '{spec}'"))?);
            }
            None => return Err(format!("'{part}' is not a modifier in '{spec}' (use Ctrl, Alt or Shift)")),
        }
    }
    match key {
        Some(key) => Ok(Combo { key, ctrl, alt, shift }),
        None => Err(format!("no key after the modifiers in '{spec}'")),
    }
}

/// Whether `combo` is held now, given a reader of key states. A lone
/// modifier only checks itself; other combos need the exact modifier set.
pub(crate) fn is_held(combo: &Combo, down: &dyn Fn(u16) -> bool) -> bool {
    if !down(combo.key) {
        return false;
    }
    if matches!(combo.key, VK_SHIFT | VK_CONTROL | VK_MENU) {
        return true;
    }
    down(VK_CONTROL) == combo.ctrl && down(VK_MENU) == combo.alt && down(VK_SHIFT) == combo.shift
}

/// Latching state shared by every mod: what each mod watches, what it has
/// not taken yet, and the last sampled state of each watched key.
#[derive(Default)]
pub(crate) struct Latch {
    watches: HashMap<String, Vec<Combo>>,
    queues: HashMap<String, VecDeque<Combo>>,
    previous: HashMap<u16, bool>,
}

impl Latch {
    pub(crate) fn watch(&mut self, mod_id: &str, combos: Vec<Combo>) {
        self.queues.entry(mod_id.to_owned()).or_default().clear();
        if combos.is_empty() {
            self.watches.remove(mod_id);
        } else {
            self.watches.insert(mod_id.to_owned(), combos);
        }
    }

    pub(crate) fn take(&mut self, mod_id: &str) -> Vec<Combo> {
        self.queues
            .get_mut(mod_id)
            .map(|queue| queue.drain(..).collect())
            .unwrap_or_default()
    }

    pub(crate) fn is_watching(&self) -> bool {
        !self.watches.is_empty()
    }

    /// One sample: queue every watched combo whose key went down since the
    /// previous sample with its exact modifiers. While the game is not
    /// focused nothing is read and every key counts as held, so a key still
    /// down when focus returns must be released before it presses again.
    pub(crate) fn sample(&mut self, focused: bool, down: &dyn Fn(u16) -> bool) {
        let mut keys: Vec<u16> = self
            .watches
            .values()
            .flatten()
            .map(|combo| combo.key)
            .collect();
        keys.sort_unstable();
        keys.dedup();
        self.previous.retain(|key, _| keys.contains(key));
        for key in keys {
            if !focused {
                self.previous.insert(key, true);
                continue;
            }
            let now = down(key);
            let was = self.previous.insert(key, now).unwrap_or(now);
            if !now || was {
                continue;
            }
            for (mod_id, combos) in &self.watches {
                for combo in combos.iter().filter(|combo| combo.key == key) {
                    if is_held(combo, down) {
                        let queue = self.queues.entry(mod_id.clone()).or_default();
                        if queue.len() == MAX_QUEUED_PRESSES {
                            queue.pop_front();
                        }
                        queue.push_back(*combo);
                    }
                }
            }
        }
    }
}

fn latch() -> &'static Mutex<Latch> {
    static LATCH: OnceLock<Mutex<Latch>> = OnceLock::new();
    LATCH.get_or_init(|| {
        // Without the sampler, watched presses are simply never queued.
        if let Err(error) = std::thread::Builder::new()
            .name("shroudforge-game-keys".to_owned())
            .spawn(run_sampler)
        {
            tracing::warn!(target: "shroudforge::input", "game key sampler not started: {error}");
        }
        Mutex::new(Latch::default())
    })
}

fn run_sampler() {
    loop {
        let watching = {
            let mut latch = latch().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if latch.is_watching() {
                latch.sample(game_focused(), &key_down);
            }
            latch.is_watching()
        };
        std::thread::sleep(if watching { SAMPLE_PERIOD } else { IDLE_PERIOD });
    }
}

/// Replace the combos a mod watches. Their presses are latched from now on.
pub(crate) fn watch(mod_id: &str, combos: Vec<Combo>) {
    latch()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .watch(mod_id, combos);
}

/// Presses of the mod's watched combos since the previous call, in order.
pub(crate) fn take_presses(mod_id: &str) -> Vec<Combo> {
    latch()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .take(mod_id)
}

/// Whether the keys of `combo` are all held now: `is_key_down` semantics,
/// which ignore modifiers the spec does not name, as the original API did.
pub(crate) fn are_down(combo: &Combo, down: &dyn Fn(u16) -> bool) -> bool {
    down(combo.key)
        && (!combo.ctrl || down(VK_CONTROL))
        && (!combo.alt || down(VK_MENU))
        && (!combo.shift || down(VK_SHIFT))
}

/// `is_key_down`: the keys of `combo` held now while the game window is focused.
pub(crate) fn is_down_now(combo: &Combo) -> bool {
    game_focused() && are_down(combo, &key_down)
}

#[cfg(windows)]
fn game_focused() -> bool {
    use windows_sys::Win32::{
        System::Threading::GetCurrentProcessId,
        UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId},
    };
    let foreground = unsafe { GetForegroundWindow() };
    if foreground.is_null() {
        return false;
    }
    let mut process_id = 0;
    unsafe { GetWindowThreadProcessId(foreground, &mut process_id) };
    process_id == unsafe { GetCurrentProcessId() }
}

#[cfg(windows)]
fn key_down(code: u16) -> bool {
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;
    (unsafe { GetAsyncKeyState(code as i32) }) < 0
}

#[cfg(not(windows))]
fn game_focused() -> bool {
    false
}

#[cfg(not(windows))]
fn key_down(_code: u16) -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn reader(held: &[u16]) -> impl Fn(u16) -> bool + '_ {
        move |code| held.contains(&code)
    }

    #[test]
    fn parses_keys_modifiers_and_aliases_case_insensitively() {
        assert_eq!(parse("F4").unwrap().canonical(), "F4");
        assert_eq!(parse("f24").unwrap().key, 0x87);
        assert_eq!(parse("shift+r").unwrap().canonical(), "Shift+R");
        assert_eq!(parse("Shift + Ctrl + Alt + 7").unwrap().canonical(), "Ctrl+Alt+Shift+7");
        assert_eq!(parse("control+esc").unwrap().canonical(), "Ctrl+Escape");
        assert_eq!(parse("numpad5").unwrap().canonical(), "Numpad5");
        assert_eq!(parse("Mouse4").unwrap().key, 0x05);
        assert_eq!(parse("Shift").unwrap().canonical(), "Shift");
    }

    #[test]
    fn rejects_invalid_specifications() {
        for spec in ["", "F25", "Shift+", "+F4", "Shift+Shift+F4", "R+F4", "Ctrl+Alt", "Hyper+F4"] {
            assert!(parse(spec).is_err(), "{spec} should be rejected");
        }
    }

    #[test]
    fn modifiers_must_match_exactly() {
        let plain = parse("F4").unwrap();
        let shifted = parse("Shift+F4").unwrap();
        assert!(is_held(&plain, &reader(&[0x73])));
        assert!(!is_held(&plain, &reader(&[0x73, VK_SHIFT])));
        assert!(is_held(&shifted, &reader(&[0x73, VK_SHIFT])));
        assert!(!is_held(&shifted, &reader(&[0x73, VK_SHIFT, VK_CONTROL])));
        assert!(is_held(&parse("Shift").unwrap(), &reader(&[VK_SHIFT, VK_CONTROL])));
    }

    #[test]
    fn is_key_down_ignores_unnamed_modifiers_like_the_original_api() {
        assert!(are_down(&parse("F5").unwrap(), &reader(&[0x74, VK_SHIFT])));
        assert!(are_down(&parse("Shift+F5").unwrap(), &reader(&[0x74, VK_SHIFT, VK_CONTROL])));
        assert!(!are_down(&parse("Shift+F5").unwrap(), &reader(&[0x74])));
    }

    #[test]
    fn latches_short_presses_once_per_rising_edge() {
        let mut latch = Latch::default();
        latch.watch("architect", vec![parse("F6").unwrap(), parse("Shift+F6").unwrap()]);
        latch.sample(true, &reader(&[]));
        latch.sample(true, &reader(&[0x75])); // tap: down for one sample only
        latch.sample(true, &reader(&[]));
        latch.sample(true, &reader(&[0x75, VK_SHIFT]));
        latch.sample(true, &reader(&[0x75, VK_SHIFT])); // held: no repeat
        let names: Vec<String> = latch.take("architect").iter().map(Combo::canonical).collect();
        assert_eq!(names, ["F6", "Shift+F6"]);
        assert!(latch.take("architect").is_empty(), "take empties the queue");
    }

    #[test]
    fn ignores_presses_while_the_game_is_not_focused() {
        let mut latch = Latch::default();
        latch.watch("architect", vec![parse("F6").unwrap()]);
        latch.sample(false, &reader(&[0x75]));
        latch.sample(false, &reader(&[]));
        assert!(latch.take("architect").is_empty());
        // Held while focus returns: not a new press.
        latch.sample(false, &reader(&[0x75]));
        latch.sample(true, &reader(&[0x75]));
        assert!(latch.take("architect").is_empty());
    }

    #[test]
    fn keeps_mods_separate_and_bounded() {
        let mut latch = Latch::default();
        latch.watch("a", vec![parse("R").unwrap()]);
        latch.watch("b", vec![parse("T").unwrap()]);
        for _ in 0..(MAX_QUEUED_PRESSES + 10) {
            latch.sample(true, &reader(&[0x52]));
            latch.sample(true, &reader(&[]));
        }
        assert_eq!(latch.take("a").len(), MAX_QUEUED_PRESSES);
        assert!(latch.take("b").is_empty());
        latch.watch("a", vec![]);
        assert!(!latch.watches.contains_key("a"));
        let names: HashSet<String> = NAMED_KEYS.iter().map(|(name, _)| (*name).to_owned()).collect();
        assert_eq!(names.len(), NAMED_KEYS.len(), "key names are unique");
    }
}
