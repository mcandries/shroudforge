use crate::windows::provider::provider;

pub fn world_cursor_read() -> Option<(Vec<u8>, u64)> {
    let provider = provider()?;
    let mut bytes = vec![0u8; 0xa0];
    let mut sequence = 0u64;
    let read = provider.world_cursor_read?;
    let ok = unsafe { read(bytes.as_mut_ptr(), bytes.len(), &mut sequence) };
    (ok && sequence != 0).then_some((bytes, sequence))
}

pub fn world_cursor_read_input(component_size: usize) -> Result<(Vec<u8>, u64), String> {
    let read = provider()
        .and_then(|provider| provider.world_cursor_read_input)
        .ok_or("native runtime lacks the reflected local cursor reader")?;
    if component_size == 0 || component_size > 65_536 {
        return Err("local cursor component size is outside the supported bounds".into());
    }
    let session = crate::windows::world_context::world_session_id();
    if session == 0 {
        return Err("local cursor is waiting for the current world session".into());
    }
    let mut bytes = vec![0u8; component_size];
    let mut sequence = 0u64;
    let result = unsafe {
        read(
            component_size,
            session,
            bytes.as_mut_ptr(),
            bytes.len(),
            &mut sequence,
        )
    };
    match result {
        1 if sequence != 0 && crate::windows::world_context::world_session_id() == session => {
            Ok((bytes, sequence))
        }
        1 | 4 => Err("local cursor world session changed or is unavailable".into()),
        2 => Err("local cursor component does not match the requested buffer".into()),
        3 => Err("local cursor original ECS contract is unavailable".into()),
        5 => {
            Err("Local cursor query is still scanning or changed. Retry on the next update".into())
        }
        6 => Err("local cursor input is ambiguous in the main-client ECS manager".into()),
        _ => Err("native local cursor reader returned an unsupported result".into()),
    }
}

/// Gives the native runtime the reflected layout of the local camera inside
/// ClientPlayerInput (component size, 9 byte offsets), so that a caller
/// drawing every frame can read it. False when the runtime lacks it.
pub fn set_local_camera_layout(component_size: usize, offsets: &[u32]) -> bool {
    let Some(set) = provider().and_then(|provider| provider.set_local_camera_layout) else {
        return false;
    };
    unsafe { set(component_size, offsets.as_ptr(), offsets.len()) == 1 }
}
