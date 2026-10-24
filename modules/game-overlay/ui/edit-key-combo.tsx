import { clearsKeyCombo, keyComboFromEvent } from '../../modloader/ui/format-key-combo'

// A key field filled by pressing the key or combination, such as Shift+F12,
// in the specification mods and the overlay layer read. Backspace clears it.
export function KeyComboInput({
  value,
  label,
  hint,
  onChange,
}: {
  value: string
  label: string
  hint: string
  onChange(value: string): void
}) {
  return (
    <input
      className="mod-input keybind-input"
      value={value}
      readOnly
      aria-label={label}
      title={hint}
      onKeyDown={(event) => {
        event.preventDefault()
        if (clearsKeyCombo(event)) {
          onChange('')
          return
        }
        const combo = keyComboFromEvent(event)
        if (combo) onChange(combo)
      }}
      onClick={(event) => event.currentTarget.focus()}
    />
  )
}
