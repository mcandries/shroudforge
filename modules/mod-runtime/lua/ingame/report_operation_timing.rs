use std::sync::Mutex;
use std::time::{Duration, Instant};

use super::runtime_provider;

const INTERVAL: Duration = Duration::from_secs(5);
static LAST: Mutex<Option<Instant>> = Mutex::new(None);

/// Every 5 s, how long runtime operations waited (the native timing) and how
/// long the mods' calls took, in the log (diagnostic).
pub(crate) fn report_operation_timing() {
    let Ok(mut last) = LAST.lock() else {
        return;
    };
    let now = Instant::now();
    match *last {
        Some(previous) if now.duration_since(previous) < INTERVAL => return,
        None => {
            *last = Some(now);
            return;
        }
        _ => {}
    }
    *last = Some(now);
    let native = runtime_provider::operation_timing().unwrap_or_default();
    let calls = take_call_timing();
    if native.is_empty() && calls.is_empty() {
        return;
    }
    tracing::info!(target: "shroudforge::timing", "native: {native} | calls: {calls}");
}

static CALLS: Mutex<Vec<(&'static str, u64, u128, u128)>> = Mutex::new(Vec::new());

/// One call of a runtime function, as the mod sees it.
pub(crate) fn record_call(name: &'static str, started: Instant) {
    let micros = started.elapsed().as_micros();
    if let Ok(mut calls) = CALLS.lock() {
        match calls.iter_mut().find(|entry| entry.0 == name) {
            Some(entry) => {
                entry.1 += 1;
                entry.2 += micros;
                entry.3 = entry.3.max(micros);
            }
            None => calls.push((name, 1, micros, micros)),
        }
    }
}

fn take_call_timing() -> String {
    let Ok(mut calls) = CALLS.lock() else {
        return String::new();
    };
    let text = calls
        .iter()
        .map(|(name, count, total, maximum)| {
            format!(
                "{name} {count}x avg {:.2} max {:.2} ms",
                *total as f64 / 1000.0 / *count as f64,
                *maximum as f64 / 1000.0
            )
        })
        .collect::<Vec<_>>()
        .join("; ");
    calls.clear();
    text
}
