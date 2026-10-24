// Turns a keydown event into the key specification the mod runtime reads
// (modules/mod-runtime/lua/shroudforge/v1/game_keys.rs): Ctrl, Alt and Shift
// in that order, then the key, such as `Shift+F4` or `Ctrl+R`.

export type KeyEventLike = {
  key: string
  code: string
  ctrlKey: boolean
  altKey: boolean
  shiftKey: boolean
}

// Physical keys whose runtime name does not depend on the keyboard layout.
const CODE_NAMES: Record<string, string> = {
  Space: 'Space',
  Enter: 'Enter',
  NumpadEnter: 'Enter',
  Tab: 'Tab',
  Escape: 'Escape',
  Backspace: 'Backspace',
  Insert: 'Insert',
  Delete: 'Delete',
  Home: 'Home',
  End: 'End',
  PageUp: 'PageUp',
  PageDown: 'PageDown',
  ArrowLeft: 'Left',
  ArrowRight: 'Right',
  ArrowUp: 'Up',
  ArrowDown: 'Down',
  CapsLock: 'CapsLock',
  Pause: 'Pause',
  NumpadMultiply: 'NumpadMultiply',
  NumpadAdd: 'NumpadAdd',
  NumpadSubtract: 'NumpadSubtract',
  NumpadDecimal: 'NumpadDecimal',
  NumpadDivide: 'NumpadDivide',
  Minus: 'Minus',
  Equal: 'Equals',
  Comma: 'Comma',
  Period: 'Period',
  Slash: 'Slash',
  Backslash: 'Backslash',
  BracketLeft: 'LeftBracket',
  BracketRight: 'RightBracket',
  Quote: 'Quote',
  Backquote: 'Backquote',
  Semicolon: 'Semicolon',
}

const MODIFIER_KEYS = new Set(['Shift', 'Control', 'Alt', 'AltGraph', 'Meta', 'OS'])

function keyName(event: KeyEventLike): string | null {
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(event.key)) return event.key
  if (/^Digit[0-9]$/.test(event.code)) return event.code.slice('Digit'.length)
  if (/^Numpad[0-9]$/.test(event.code)) return event.code
  // Letters follow the layout, like the runtime's virtual keys: the key
  // labelled A on an AZERTY keyboard is `A`.
  if (/^Key[A-Z]$/.test(event.code))
    return /^[a-z]$/i.test(event.key) ? event.key.toUpperCase() : event.code.slice('Key'.length)
  return CODE_NAMES[event.code] ?? null
}

// The combination of a keydown event, or null while only modifiers are held,
// for a key the runtime does not know, or for Escape, which the game keeps
// for its pause menu and for closing its windows.
export function keyComboFromEvent(event: KeyEventLike): string | null {
  if (MODIFIER_KEYS.has(event.key)) return null
  const name = keyName(event)
  if (!name || name === 'Escape') return null
  return [event.ctrlKey && 'Ctrl', event.altKey && 'Alt', event.shiftKey && 'Shift', name]
    .filter(Boolean)
    .join('+')
}

// Backspace alone clears the binding; Shift+Backspace and others bind.
export function clearsKeyCombo(event: KeyEventLike): boolean {
  return event.code === 'Backspace' && !event.ctrlKey && !event.altKey && !event.shiftKey
}
