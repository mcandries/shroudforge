//! Calls into the game overlay layer (`modules/game-overlay`), which the
//! Vulkan loader maps into the game process. Without the layer (disabled,
//! dedicated server, unsupported loader), every call reports it unavailable.

use std::ffi::{c_char, CString};

const UNAVAILABLE: &str = "the game overlay is not loaded";

pub(crate) fn set_zone(owner: &str, id: &str, zone: &serde_json::Value) -> Result<(), String> {
    let layer = layer::get().ok_or(UNAVAILABLE)?;
    let owner = text(owner)?;
    let id = text(id)?;
    let zone = text(&zone.to_string())?;
    let mut error = [0 as c_char; 512];
    // SAFETY: valid NUL-terminated strings and a writable buffer of the given size.
    let result = unsafe { (layer.set_zone)(owner.as_ptr(), id.as_ptr(), zone.as_ptr(), error.as_mut_ptr(), error.len() as u32) };
    if result == 0 {
        return Ok(());
    }
    // SAFETY: the layer writes a NUL-terminated message, truncated to the buffer.
    Err(unsafe { std::ffi::CStr::from_ptr(error.as_ptr()) }.to_string_lossy().into_owned())
}

/// Whether the layer takes blobs (`ShroudforgeOverlay_SetBlob`, our build).
pub(crate) fn blobs_available() -> bool {
    layer::get().is_some_and(|layer| layer.set_blob.is_some() && layer.drop_blob.is_some())
}

/// Gives `data` to the layer as the blob `id` of `owner`, for its zones to
/// name by `{"blob": id}`. False when the layer cannot take it.
pub(crate) fn set_blob(owner: &str, id: u64, data: &[u8]) -> bool {
    let (Some(layer), Ok(owner)) = (layer::get(), text(owner)) else {
        return false;
    };
    let Some(set_blob) = layer.set_blob else {
        return false;
    };
    // SAFETY: a valid NUL-terminated string and `data.len()` readable bytes.
    unsafe { set_blob(owner.as_ptr(), id, data.as_ptr().cast(), data.len() as u64) == 0 }
}

pub(crate) fn drop_blob(owner: &str, id: u64) {
    if let (Some(layer), Ok(owner)) = (layer::get(), text(owner)) {
        if let Some(drop_blob) = layer.drop_blob {
            // SAFETY: a valid NUL-terminated string.
            unsafe { drop_blob(owner.as_ptr(), id) };
        }
    }
}

pub(crate) fn remove_zone(owner: &str, id: &str) {
    if let (Some(layer), Ok(owner), Ok(id)) = (layer::get(), text(owner), text(id)) {
        // SAFETY: valid NUL-terminated strings.
        unsafe { (layer.remove_zone)(owner.as_ptr(), id.as_ptr()) };
    }
}

/// Removes every zone of a mod; called when the mod stops running.
pub(crate) fn clear_owner(owner: &str) {
    if let (Some(layer), Ok(owner)) = (layer::get(), text(owner)) {
        // SAFETY: a valid NUL-terminated string.
        unsafe { (layer.clear_owner)(owner.as_ptr()) };
    }
}

/// `{available, active, width, height, scale}` of the overlay output.
pub(crate) fn status() -> serde_json::Value {
    let Some(layer) = layer::get() else {
        return serde_json::json!({ "available": false, "active": false });
    };
    let mut buffer = [0 as c_char; 1024];
    // SAFETY: a writable buffer of the given size.
    if unsafe { (layer.get_status)(buffer.as_mut_ptr(), buffer.len() as u32) } != 0 {
        return serde_json::json!({ "available": true, "active": false });
    }
    // SAFETY: the layer wrote a NUL-terminated JSON object.
    let text = unsafe { std::ffi::CStr::from_ptr(buffer.as_ptr()) }.to_string_lossy();
    let mut value: serde_json::Value = serde_json::from_str(&text).unwrap_or_else(|_| serde_json::json!({}));
    value["available"] = true.into();
    value
}

fn text(value: &str) -> Result<CString, String> {
    CString::new(value).map_err(|_| "overlay text cannot contain NUL characters".to_owned())
}

#[cfg(windows)]
mod layer {
    use std::ffi::c_char;
    use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};

    pub(super) struct Layer {
        pub(super) set_zone: unsafe extern "C" fn(*const c_char, *const c_char, *const c_char, *mut c_char, u32) -> i32,
        pub(super) remove_zone: unsafe extern "C" fn(*const c_char, *const c_char),
        pub(super) clear_owner: unsafe extern "C" fn(*const c_char),
        pub(super) get_status: unsafe extern "C" fn(*mut c_char, u32) -> i32,
        pub(super) set_blob: Option<unsafe extern "C" fn(*const c_char, u64, *const c_char, u64) -> i32>,
        pub(super) drop_blob: Option<unsafe extern "C" fn(*const c_char, u64)>,
    }

    /// The layer's exports, looked up at each call: the DLL is mapped when the
    /// game creates its Vulkan instance, possibly after the runtime started.
    pub(super) fn get() -> Option<Layer> {
        let name: Vec<u16> = "shroudforge_overlay.dll".encode_utf16().chain(Some(0)).collect();
        // SAFETY: a NUL-terminated UTF-16 module name; no reference count is taken.
        let module = unsafe { GetModuleHandleW(name.as_ptr()) };
        if module.is_null() {
            return None;
        }
        // SAFETY: each export is looked up by its NUL-terminated name and has the
        // signature declared in modules/game-overlay/src/scene.cpp (API version 1).
        unsafe {
            let version: unsafe extern "C" fn() -> i32 =
                std::mem::transmute(GetProcAddress(module, c"ShroudforgeOverlay_ApiVersion".as_ptr().cast())?);
            if version() != 1 {
                return None;
            }
            Some(Layer {
                set_zone: std::mem::transmute(GetProcAddress(module, c"ShroudforgeOverlay_SetZone".as_ptr().cast())?),
                remove_zone: std::mem::transmute(GetProcAddress(module, c"ShroudforgeOverlay_RemoveZone".as_ptr().cast())?),
                clear_owner: std::mem::transmute(GetProcAddress(module, c"ShroudforgeOverlay_ClearOwner".as_ptr().cast())?),
                get_status: std::mem::transmute(GetProcAddress(module, c"ShroudforgeOverlay_GetStatus".as_ptr().cast())?),
                // Optional: a layer of our build only.
                set_blob: GetProcAddress(module, c"ShroudforgeOverlay_SetBlob".as_ptr().cast())
                    .map(|function| std::mem::transmute(function)),
                drop_blob: GetProcAddress(module, c"ShroudforgeOverlay_DropBlob".as_ptr().cast())
                    .map(|function| std::mem::transmute(function)),
            })
        }
    }
}

#[cfg(not(windows))]
mod layer {
    use std::ffi::c_char;

    pub(super) struct Layer {
        pub(super) set_zone: unsafe extern "C" fn(*const c_char, *const c_char, *const c_char, *mut c_char, u32) -> i32,
        pub(super) remove_zone: unsafe extern "C" fn(*const c_char, *const c_char),
        pub(super) clear_owner: unsafe extern "C" fn(*const c_char),
        pub(super) get_status: unsafe extern "C" fn(*mut c_char, u32) -> i32,
        pub(super) set_blob: Option<unsafe extern "C" fn(*const c_char, u64, *const c_char, u64) -> i32>,
        pub(super) drop_blob: Option<unsafe extern "C" fn(*const c_char, u64)>,
    }

    pub(super) fn get() -> Option<Layer> {
        None
    }
}
