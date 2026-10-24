---
{
  "id": "control-keybind",
  "locale": "de",
  "title": "Steuerelement `keybind`",
  "summary": "Für eine Tastenkombination, die der Modloader als Schlüsselwert speichert.",
  "navigation": {
    "path": [
      "extended.mod.json",
      "Steuerelemente",
      "Steuerelementtypen"
    ]
  }
}
---

# Steuerelement `keybind`

Für eine Tastenkombination, die der Modloader als Schlüsselwert speichert. Lege es bei einer Einstellung als `control` fest.

```json
{
  "settings": {
    "example": {"value": "F7", "label": "Tastenkürzel", "control": "keybind"}
  }
}
```

Der Wert in `value` sollte zum Steuerelement passen. Weitere Regeln stehen unter [settings.<key>.control](#doc-field-settings-key-control).

Der Spieler klickt in das Feld und drückt eine Taste oder eine Kombination. Der Modloader speichert die Tastenangabe, die `shroudforge.input` liest: Strg, Alt und Umschalt in dieser Reihenfolge, dann die Taste, zum Beispiel `F7`, `Shift+F4` oder `Ctrl+R`. Buchstaben folgen dem Tastaturlayout, Ziffern ihrer Position in der Zahlenreihe, benannte Tasten tragen die Laufzeitnamen (`Up`, `Space`, `Numpad5`). Rücktaste allein löscht die Belegung. Übergib den Wert an `shroudforge.input.watch` und lies ihn mit `take_presses`.
