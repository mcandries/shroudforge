---
{
  "id": "control-keybind",
  "locale": "en",
  "title": "The `keybind` control",
  "summary": "For a keyboard shortcut stored by Modloader as a key value.",
  "navigation": {
    "path": [
      "extended.mod.json",
      "Controls",
      "Control types"
    ]
  }
}
---

# The `keybind` control

For a keyboard shortcut stored by Modloader as a key value. Set it as `control` on a setting.

```json
{
  "settings": {
    "example": {"value": "F7", "label": "Key binding", "control": "keybind"}
  }
}
```

The value in `value` should match the selected control. See [settings.<key>.control](#doc-field-settings-key-control) for the schema rules.

The player clicks the field and presses a key or a combination. Modloader stores the key specification that `shroudforge.input` reads: Ctrl, Alt and Shift in that order, then the key, such as `F7`, `Shift+F4` or `Ctrl+R`. Letters follow the keyboard layout, digits are their row position, and named keys use the runtime names (`Up`, `Space`, `Numpad5`). Backspace alone clears the binding. Escape is not accepted: the game keeps it for its pause menu and for closing windows. Pass the value to `shroudforge.input.watch` and read it with `take_presses`.
