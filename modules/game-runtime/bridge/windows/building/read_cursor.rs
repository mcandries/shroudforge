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

/// How long runtime operations waited since the last call, then reset
/// (diagnostic): "dispatch-wait 12x avg 25.10 max 40.20 ms; ...".
pub fn operation_timing() -> Option<String> {
    let timing = provider()?.operation_timing?;
    let mut buffer = [0u8; 2048];
    let length = unsafe { timing(buffer.as_mut_ptr(), buffer.len()) }.min(buffer.len() - 1);
    Some(String::from_utf8_lossy(&buffer[..length]).into_owned())
}
