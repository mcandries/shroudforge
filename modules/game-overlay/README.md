# Game overlay module

This module draws mod-defined zones (text, images, frames) over the game image. It is an implicit Vulkan layer, `shroudforge_overlay.dll`, shipped in `shroudforge/overlay` with its manifest `shroudforge_overlay.json`.

The bootstrap (`modules/entrypoints/windows-bootstrap/startup.cpp`) enables it in the game client only. It adds the folder to `VK_ADD_IMPLICIT_LAYER_PATH` and sets `SHROUDFORGE_OVERLAY_ENABLE=1` before Enshrouded creates its Vulkan instance. It skips this when `modules.gameOverlay.enabled` is false in `modloader-config.json`, or when `SHROUDFORGE_OVERLAY=0`. Processes started by the game inherit these variables, but the layer does nothing outside `enshrouded.exe`.

| Action | Implementation |
| --- | --- |
| Insert the layer, track devices, queues and swapchains | `src/layer_entry.cpp` |
| Draw the zones in the presented image with Dear ImGui | `src/renderer.cpp` |
| Lay out and draw zones and items | `src/draw_scene.cpp` |
| Store zones and options, C API for the runtime | `src/scene.cpp` |
| Decode PNG and JPEG images | `src/load_image.cpp` |
| Read the player options, handle the show/hide key | `src/player_options.cpp` |
| Player options in the Modloader (F9) | `ui/settings.ts`, `modules/loader-config/ui/show-module-settings.tsx` |
| Log to `shroudforge_overlay.log` beside the DLL | `src/log.cpp` |
| Present test with a readback | `tests/present_test.cpp` |

## Rendering

Mods describe zones (retained mode). At each `vkQueuePresentKHR` with at least one visible zone, the layer:

1. records a render pass over the presented image (load, draw, store);
2. submits it after the game's wait semaphores;
3. makes the real present wait on its own semaphore.

Without a visible zone, the present is passed through untouched and the layer does no GPU work. The ImGui backend starts on the first present that has something to draw, on that queue. Unsupported cases are not drawn: a swapchain without `COLOR_ATTACHMENT` usage, or a format other than 8-bit or 10-bit UNORM/SRGB (HDR formats are left alone).

Fonts are TrueType or OpenType files rasterized on demand by Dear ImGui 1.92. The default font is Segoe UI, or the ImGui default if it is missing. Images and fonts stay loaded for the session (at most 100 images).

## Player options

The player edits them in the Modloader (F9, Settings, Modules, Game overlay). They are saved in `modules.gameOverlay` of `modloader-config.json`:

| Key | Default | Effect |
| --- | --- | --- |
| `enabled` | `true` | Read by the bootstrap at game start: the layer is not loaded at all when false. Also read live by the layer, which stops drawing. |
| `scale` | `100` | Size of every zone, in percent (25 to 400), on top of the screen scale. |
| `opacity` | `100` | Opacity of every zone, in percent. |
| `hideKey` | `""` | Key or combination that shows or hides every zone while the game has focus, such as `Shift+F12`, in the names mods use (`game_keys.rs`); empty for none. Captured by pressing it in F9 (`ui/edit-key-combo.tsx`). |

The layer also logs when the mouse cursor is shown, hidden or confined, to find a signal for the game's menus (the zones are drawn over them for now).

The layer rereads the file at most once per second, only when its modification time changed, so changes apply while the game runs.

## C API

The runtime (same process) finds the layer with `GetModuleHandleW(L"shroudforge_overlay.dll")` and calls these exports. All strings are UTF-8.

| Export | Use |
| --- | --- |
| `int ShroudforgeOverlay_ApiVersion()` | Version of this API (1). |
| `int ShroudforgeOverlay_SetZone(owner, id, json, error, error_size)` | Create or replace zone `id` of mod `owner`. Returns 0, or -1 with a message. |
| `void ShroudforgeOverlay_RemoveZone(owner, id)` | Remove one zone. |
| `void ShroudforgeOverlay_ClearOwner(owner)` | Remove every zone of a mod. |
| `int ShroudforgeOverlay_GetStatus(out, out_size)` | `{"active", "width", "height", "scale"}` of the current output. |

Paths in zones must be absolute. Mods use the Lua API `shroudforge.overlay` (`set`, `remove`, `clear`, `status`, documented in `modules/mod-runtime/lua/shroudforge/v1/definitions/mod_api.lua`). The runtime resolves the `image` and `font` fields inside the mod's folder (`resolve_overlay_paths.rs`; zip packages are refused) and calls the layer (`game_overlay.rs`). A mod's zones are removed when it stops running (`session/record_runtime_state.rs`). Limits: 32 zones per mod, 256 items per zone, 4096 bytes per text, rows nested 3 deep.

## Zone format

Sizes and offsets are reference pixels of a 1920x1080 screen. They are multiplied by `screen height / 1080 × scale option`. Colors are `"#RRGGBB"`, `"#RRGGBBAA"` or `[r, g, b, a]`.

| Field | Meaning |
| --- | --- |
| `anchor` | Screen point: `top-left`, `top`, `top-right`, `left`, `center`, `right`, `bottom-left`, `bottom`, `bottom-right`, or `[x, y]` as fractions of the screen. Default `top-left`. |
| `pivot` | Point of the zone placed on the anchor, same values as fractions of the zone. Default: same as `anchor`. |
| `offset` | `[x, y]` moved from that position. |
| `size` | `[width, height]`; 0 fits the content. With a width, text wraps. |
| `order` | Drawing order, lower first. |
| `visible` | `false` hides the zone without removing it. |
| `background`, `border`, `border_width`, `rounding` | Plain background and border. |
| `frame` | Nine-slice image: `{image, slice = n or [left, top, right, bottom] (image pixels), scale, tint}`. |
| `padding`, `spacing` | Inner margin (`n` or `[x, y]`) and space between items. |
| `text` | Default text style of the zone: `font` (TTF path), `size`, `color`, `shadow`, `shadow_color`, `outline`, `outline_color`. |
| `items` | List, drawn top to bottom. |

Items (a plain string is a text item):

| `type` | Fields |
| --- | --- |
| `text` | `text`, the text style fields, `align` (`left`, `center`, `right`). |
| `image` | `image` (path), `size` (`n`, `[w, h]`, one side 0 keeps the ratio; default: image size), `tint`, `uv` `[u0, v0, u1, v1]` for an icon in an atlas, `rounding`, `align`. |
| `row` | `items` side by side, centered vertically, `spacing`, text style fields inherited by its items. |
| `spacer` | `size`: height. |
| `separator` | `color`, `thickness`: a line across the zone. |
| `progress` | `value` 0 to 1, `size`, `color`, `background`, `rounding`. |

## Test

Build with `dev/build/build_game_overlay.ps1`. Copy `build/game-overlay/Release/overlay_present_test.exe` beside the built layer as `enshrouded.exe`. Then run it with `VK_ADD_IMPLICIT_LAYER_PATH` set to that folder and `SHROUDFORGE_OVERLAY_ENABLE=1`:

```powershell
enshrouded.exe modules/game-overlay/tests/assets capture.bmp
```

It publishes three test zones, presents frames and saves the last presented image.
